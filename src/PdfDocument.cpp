#include "PdfDocument.h"

#include <poppler-qt6.h>

// Core (non-Qt) poppler API, used only by imageBoxes() to get per-image
// geometry that the Qt bindings don't expose.
#include <GfxState.h>
#include <GlobalParams.h>
#include <OutputDev.h>
#include <PDFDoc.h>
#include <Page.h>
#include <goo/GooString.h>

#include <algorithm>
#include <tuple>
#include <utility>

namespace {

// Walks a page's content stream recording the on-page bounding box of every
// drawn image (regular, masked, or soft-masked), by transforming the image's
// unit square through the CTM active at each draw call. Configured (via
// upsideDown()) to use the same top-left-origin, point-space convention as
// PdfTextWord::bbox and the rendered page image.
class ImageBoxOutputDev : public OutputDev {
public:
    bool upsideDown() override { return true; }
    bool useDrawChar() override { return false; }
    bool interpretType3Chars() override { return false; }

    void drawImageMask(GfxState *state, Object *, Stream *, int, int, bool, bool, bool) override { record(state); }
    void drawImage(GfxState *state, Object *, Stream *, int, int, GfxImageColorMap *, bool, const int *, bool) override
    {
        record(state);
    }
    void drawMaskedImage(GfxState *state, Object *, Stream *, int, int, GfxImageColorMap *, bool, Stream *, int, int, bool, bool) override
    {
        record(state);
    }
    void drawSoftMaskedImage(GfxState *state, Object *, Stream *, int, int, GfxImageColorMap *, bool, Stream *, int, int, GfxImageColorMap *, bool) override
    {
        record(state);
    }

    QVector<QRectF> boxes;

private:
    void record(GfxState *state)
    {
        // Matrix::transform() isn't an exported symbol in the shared
        // library (it's treated as poppler-internal), so apply the affine
        // transform by hand using the matrix's public data array instead:
        // x' = a*x + c*y + e, y' = b*x + d*y + f, for m = [a b c d e f].
        Matrix m;
        state->getCTM(&m);
        auto apply = [&m](double x, double y) {
            return std::pair<double, double>(m.m[0] * x + m.m[2] * y + m.m[4], m.m[1] * x + m.m[3] * y + m.m[5]);
        };
        double xs[4];
        double ys[4];
        std::tie(xs[0], ys[0]) = apply(0, 0);
        std::tie(xs[1], ys[1]) = apply(1, 0);
        std::tie(xs[2], ys[2]) = apply(0, 1);
        std::tie(xs[3], ys[3]) = apply(1, 1);
        const double minX = *std::min_element(xs, xs + 4);
        const double maxX = *std::max_element(xs, xs + 4);
        const double minY = *std::min_element(ys, ys + 4);
        const double maxY = *std::max_element(ys, ys + 4);
        boxes.append(QRectF(minX, minY, maxX - minX, maxY - minY));
    }
};

} // namespace

PdfDocument::PdfDocument() = default;
PdfDocument::~PdfDocument() = default;

bool PdfDocument::load(const QString &filePath, QString *errorMessage)
{
    auto doc = std::unique_ptr<Poppler::Document>(Poppler::Document::load(filePath));
    if (!doc || doc->isLocked()) {
        if (errorMessage) {
            *errorMessage = !doc ? QStringLiteral("Could not open file (not a valid PDF?).")
                                 : QStringLiteral("PDF is password-protected (not yet supported).");
        }
        return false;
    }

    doc->setRenderHint(Poppler::Document::Antialiasing, true);
    doc->setRenderHint(Poppler::Document::TextAntialiasing, true);

    m_doc = std::move(doc);
    m_filePath = filePath;
    return true;
}

bool PdfDocument::isLoaded() const
{
    return m_doc != nullptr;
}

int PdfDocument::pageCount() const
{
    return m_doc ? m_doc->numPages() : 0;
}

QSizeF PdfDocument::pageSizePoints(int pageIndex) const
{
    if (!m_doc) {
        return {};
    }
    std::unique_ptr<Poppler::Page> page(m_doc->page(pageIndex));
    return page ? page->pageSizeF() : QSizeF();
}

QImage PdfDocument::renderPage(int pageIndex, qreal dpi, int rotationQuarterTurns) const
{
    if (!m_doc) {
        return {};
    }
    std::unique_ptr<Poppler::Page> page(m_doc->page(pageIndex));
    if (!page) {
        return {};
    }
    const auto rotation = static_cast<Poppler::Page::Rotation>(((rotationQuarterTurns % 4) + 4) % 4);
    return page->renderToImage(dpi, dpi, -1, -1, -1, -1, rotation);
}

QImage PdfDocument::renderPageRegion(int pageIndex, qreal dpi, const QRectF &regionPoints) const
{
    if (!m_doc) {
        return {};
    }
    std::unique_ptr<Poppler::Page> page(m_doc->page(pageIndex));
    if (!page) {
        return {};
    }

    const qreal pxPerPt = dpi / 72.0;
    const int x = qRound(regionPoints.left() * pxPerPt);
    const int y = qRound(regionPoints.top() * pxPerPt);
    const int w = qMax(1, qRound(regionPoints.width() * pxPerPt));
    const int h = qMax(1, qRound(regionPoints.height() * pxPerPt));
    return page->renderToImage(dpi, dpi, x, y, w, h);
}

QVector<PdfTextWord> PdfDocument::extractWords(int pageIndex) const
{
    QVector<PdfTextWord> words;
    if (!m_doc) {
        return words;
    }
    std::unique_ptr<Poppler::Page> page(m_doc->page(pageIndex));
    if (!page) {
        return words;
    }

    const auto boxes = page->textList();
    words.reserve(static_cast<int>(boxes.size()));
    for (const auto &box : boxes) {
        const QString text = box->text();
        QVector<QRectF> charBoxes;
        charBoxes.reserve(text.length());
        for (int i = 0; i < text.length(); ++i) {
            charBoxes.append(box->charBoundingBox(i));
        }
        words.append(PdfTextWord{text, box->boundingBox(), box->hasSpaceAfter(), charBoxes});
    }
    return words;
}

QVector<QRectF> PdfDocument::imageBoxes(int pageIndex) const
{
    QVector<QRectF> result;
    if (m_filePath.isEmpty()) {
        return result;
    }

    if (!m_coreDoc) {
        if (!globalParams) {
            globalParams = std::make_unique<GlobalParams>();
        }
        auto name = std::make_unique<GooString>(m_filePath.toUtf8().constData());
        auto coreDoc = std::make_unique<PDFDoc>(std::move(name));
        if (!coreDoc->isOk()) {
            return result;
        }
        m_coreDoc = std::move(coreDoc);
    }

    Page *page = m_coreDoc->getPage(pageIndex + 1); // core API is 1-indexed
    if (!page) {
        return result;
    }

    ImageBoxOutputDev dev;
    page->displaySlice(&dev, 72.0, 72.0, 0, false, true, -1, -1, -1, -1, false);
    return dev.boxes;
}

PdfDocument::DocumentInfo PdfDocument::documentInfo() const
{
    DocumentInfo info;
    if (!m_doc) {
        return info;
    }
    info.title = m_doc->title();
    info.author = m_doc->author();
    info.subject = m_doc->subject();
    info.creator = m_doc->creator();
    info.producer = m_doc->producer();
    info.pageCount = m_doc->numPages();
    const Poppler::Document::PdfVersion v = m_doc->getPdfVersion();
    info.versionMajor = v.major;
    info.versionMinor = v.minor;
    return info;
}

namespace {
QVector<PdfDocument::OutlineEntry> convertOutline(const QVector<Poppler::OutlineItem> &items)
{
    QVector<PdfDocument::OutlineEntry> result;
    result.reserve(items.size());
    for (const Poppler::OutlineItem &item : items) {
        PdfDocument::OutlineEntry entry;
        entry.title = item.name();
        if (const auto dest = item.destination()) {
            entry.pageIndex = dest->pageNumber() - 1; // Poppler destinations are 1-indexed
        }
        if (item.hasChildren()) {
            entry.children = convertOutline(item.children());
        }
        result.append(entry);
    }
    return result;
}
} // namespace

QVector<PdfDocument::OutlineEntry> PdfDocument::outline() const
{
    if (!m_doc) {
        return {};
    }
    return convertOutline(m_doc->outline());
}

namespace {
QString annotationTypeLabel(const Poppler::Annotation *annotation)
{
    switch (annotation->subType()) {
    case Poppler::Annotation::AText:
        return QStringLiteral("Note");
    case Poppler::Annotation::ALine:
        return QStringLiteral("Line");
    case Poppler::Annotation::AGeom:
        return QStringLiteral("Shape");
    case Poppler::Annotation::AHighlight: {
        const auto *highlight = static_cast<const Poppler::HighlightAnnotation *>(annotation);
        switch (highlight->highlightType()) {
        case Poppler::HighlightAnnotation::Underline:
            return QStringLiteral("Underline");
        case Poppler::HighlightAnnotation::Squiggly:
            return QStringLiteral("Squiggly Underline");
        case Poppler::HighlightAnnotation::StrikeOut:
            return QStringLiteral("Strikethrough");
        case Poppler::HighlightAnnotation::Highlight:
        default:
            return QStringLiteral("Highlight");
        }
    }
    case Poppler::Annotation::AStamp:
        return QStringLiteral("Stamp");
    case Poppler::Annotation::AInk:
        return QStringLiteral("Freehand");
    case Poppler::Annotation::ACaret:
        return QStringLiteral("Caret");
    default:
        return QStringLiteral("Annotation");
    }
}

// Subtypes a user would recognize as "a markup someone placed" — excludes
// links, form fields, and embedded-media annotations (attachment/sound/
// movie/screen/rich media), which aren't that.
bool isMarkupSubtype(Poppler::Annotation::SubType subType)
{
    switch (subType) {
    case Poppler::Annotation::AText:
    case Poppler::Annotation::ALine:
    case Poppler::Annotation::AGeom:
    case Poppler::Annotation::AHighlight:
    case Poppler::Annotation::AStamp:
    case Poppler::Annotation::AInk:
    case Poppler::Annotation::ACaret:
        return true;
    default:
        return false;
    }
}
} // namespace

QVector<PdfDocument::ExistingAnnotationInfo> PdfDocument::existingAnnotations() const
{
    QVector<ExistingAnnotationInfo> result;
    if (!m_doc) {
        return result;
    }

    for (int pageIndex = 0; pageIndex < m_doc->numPages(); ++pageIndex) {
        std::unique_ptr<Poppler::Page> page(m_doc->page(pageIndex));
        if (!page) {
            continue;
        }
        const QSizeF pageSize = page->pageSizeF();
        int ordinalInPage = 0;
        for (const auto &annotation : page->annotations()) {
            const int thisOrdinal = ordinalInPage++;
            if (!annotation || !isMarkupSubtype(annotation->subType())) {
                continue;
            }
            const QRectF normalized = annotation->boundary();
            ExistingAnnotationInfo info;
            info.pageIndex = pageIndex;
            info.ordinalInPage = thisOrdinal;
            info.typeLabel = annotationTypeLabel(annotation.get());
            info.boundaryPoints = QRectF(normalized.x() * pageSize.width(), normalized.y() * pageSize.height(),
                                          normalized.width() * pageSize.width(), normalized.height() * pageSize.height());
            info.contents = annotation->contents();
            result.append(info);
        }
    }
    return result;
}

bool PdfDocument::removeExistingAnnotation(int pageIndex, int ordinalInPage) const
{
    if (!m_doc) {
        return false;
    }
    std::unique_ptr<Poppler::Page> page(m_doc->page(pageIndex));
    if (!page) {
        return false;
    }
    auto annotations = page->annotations();
    if (ordinalInPage < 0 || ordinalInPage >= static_cast<int>(annotations.size())) {
        return false;
    }
    page->removeAnnotation(annotations[ordinalInPage].get());
    // removeAnnotation() already frees the underlying core object; letting
    // our own copy's destructor run too (when `annotations` goes out of
    // scope below) would free it a second time and crash.
    annotations[ordinalInPage].release();
    return true;
}
