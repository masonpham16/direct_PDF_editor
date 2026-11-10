#include "PdfView.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPolygon>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

qreal squaredDistanceToRect(const QPointF &p, const QRectF &r)
{
    const qreal dx = p.x() < r.left() ? r.left() - p.x() : (p.x() > r.right() ? p.x() - r.right() : 0.0);
    const qreal dy = p.y() < r.top() ? r.top() - p.y() : (p.y() > r.bottom() ? p.y() - r.bottom() : 0.0);
    return dx * dx + dy * dy;
}

bool isLineLikeShape(PdfView::ShapeKind kind)
{
    return kind == PdfView::ShapeKind::Line || kind == PdfView::ShapeKind::Arrow;
}

bool isPolygonShape(PdfView::ShapeKind kind)
{
    return kind == PdfView::ShapeKind::Polygon;
}

QString shapeKindName(PdfView::ShapeKind kind)
{
    switch (kind) {
    case PdfView::ShapeKind::Rectangle:
        return QObject::tr("Rectangle");
    case PdfView::ShapeKind::RoundedRectangle:
        return QObject::tr("Rounded Rectangle");
    case PdfView::ShapeKind::Ellipse:
        return QObject::tr("Ellipse");
    case PdfView::ShapeKind::Diamond:
        return QObject::tr("Diamond");
    case PdfView::ShapeKind::Line:
        return QObject::tr("Line");
    case PdfView::ShapeKind::Arrow:
        return QObject::tr("Arrow");
    case PdfView::ShapeKind::Polygon:
        return QObject::tr("Polygon");
    }
    return QObject::tr("Shape");
}

QString snippetOf(const QString &text, int maxLength = 60)
{
    QString simplified = text.simplified();
    if (simplified.length() > maxLength) {
        simplified = simplified.left(maxLength - 3) + QStringLiteral("...");
    }
    return simplified;
}

qreal squaredDistanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal lenSq = QPointF::dotProduct(ab, ab);
    if (lenSq <= 0.0) {
        const QPointF diff = p - a;
        return QPointF::dotProduct(diff, diff);
    }
    qreal t = QPointF::dotProduct(p - a, ab) / lenSq;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF diff = p - (a + t * ab);
    return QPointF::dotProduct(diff, diff);
}

// The arrowhead is a fixed screen-pixel size regardless of zoom, same as
// the resize handles — it stays legible without visually dominating a
// short arrow.
void drawArrowhead(QPainter &painter, const QPointF &tip, const QPointF &tail)
{
    constexpr qreal kHeadLength = 11.0;
    constexpr qreal kHeadAngle = 0.5; // radians, half-angle of the head

    QLineF shaft(tail, tip);
    if (shaft.length() < 0.01) {
        return;
    }
    const qreal angle = std::atan2(-(tip.y() - tail.y()), tip.x() - tail.x());
    const QPointF p1 = tip - QPointF(std::cos(angle - kHeadAngle), -std::sin(angle - kHeadAngle)) * kHeadLength;
    const QPointF p2 = tip - QPointF(std::cos(angle + kHeadAngle), -std::sin(angle + kHeadAngle)) * kHeadLength;

    const QPolygonF head({tip, p1, p2});
    painter.save();
    painter.setBrush(painter.pen().color());
    painter.drawPolygon(head);
    painter.restore();
}

// Scales every run's font point size by `factor` in place, preserving all
// other formatting (bold/italic/color/etc.) — used by "Size Text to Box".
void scaleDocumentFontSizes(QTextDocument *doc, qreal factor)
{
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            QTextCharFormat fmt = fragment.charFormat();
            qreal size = fmt.fontPointSize();
            if (size <= 0) {
                size = fmt.font().pointSizeF();
            }
            if (size <= 0) {
                size = 12.0;
            }
            fmt.setFontPointSize(qMax(1.0, size * factor));
            QTextCursor fragCursor(doc);
            fragCursor.setPosition(fragment.position());
            fragCursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            fragCursor.setCharFormat(fmt);
        }
    }
}

} // namespace

PdfView::PdfView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    verticalScrollBar()->setSingleStep(40);
    horizontalScrollBar()->setSingleStep(40);
    viewport()->setMouseTracking(true); // for resize-handle hover cursors
}

bool PdfView::openFile(const QString &filePath, QString *errorMessage)
{
    if (!m_doc.load(filePath, errorMessage)) {
        return false;
    }

    m_imageCache.clear();
    m_wordsCache.clear();
    m_imageBoxesCache.clear();
    m_currentPage = 0;
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    m_zoom = 1.0;
    m_rotationQuarters = 0;
    m_pastedImages.clear();
    m_draggingPastedIndex = -1;
    m_selectedPastedIndex = -1;
    m_selectedExistingAnnotationIndex = -1;
    m_searchPage = -1;
    m_searchStart = {};
    m_searchEnd = {};
    m_highlights.clear();
    m_bookmarkedPages.clear();
    m_existingAnnotations = m_doc.existingAnnotations();
    m_removedAnnotationOrdinals.clear();

    // Discard (not commit) any in-progress edit — it belongs to the
    // document being replaced.
    if (m_textEditWidget) {
        m_textEditWidget->removeEventFilter(this);
        m_textEditWidget->deleteLater();
        m_textEditWidget = nullptr;
    }
    m_editingTextBoxIndex = -1;
    m_textBoxes.clear();
    m_selectedTextBoxIndex = -1;
    m_draggingTextBoxIndex = -1;
    m_resizingTextBoxIndex = -1;
    m_resizeHandle = ResizeHandle::None;

    m_shapes.clear();
    m_selectedShapeIndex = -1;
    m_draggingShapeIndex = -1;
    m_resizingShapeIndex = -1;
    m_shapeResizeHandle = ResizeHandle::None;
    m_resizingPolygonVertex = -1;
    m_placingShape = false;
    m_shapePlacingPage = -1;
    m_placingPolygon = false;
    m_polygonPlacingPage = -1;
    m_polygonPoints.clear();

    recomputeLayout();
    updateScrollRanges();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    viewport()->update();

    emit documentChanged();
    emit zoomChanged(m_zoom);
    emit currentPageChanged(m_currentPage);
    emit rotationChanged(m_rotationQuarters);
    emit markupsChanged();
    return true;
}

double PdfView::scaleFactor() const
{
    return kLogicalDpi / 72.0 * m_zoom;
}

void PdfView::recomputeLayout()
{
    const int count = displayCount();
    m_layout.resize(count);

    qreal y = kPageGap;
    qreal maxWidth = 0;
    const qreal scale = scaleFactor();

    const bool swapDims = (m_rotationQuarters % 2) != 0;
    for (int i = 0; i < count; ++i) {
        QSizeF pts = m_doc.pageSizePoints(sourceForDisplay(i));
        if (swapDims) {
            pts.transpose();
        }
        const qreal w = pts.width() * scale;
        const qreal h = pts.height() * scale;
        m_layout[i] = {y, h, w};
        y += h + kPageGap;
        maxWidth = std::max(maxWidth, w);
    }

    m_contentHeight = y;
    m_contentWidth = maxWidth + 2 * kSideMargin;
}

int PdfView::sourceForDisplay(int displayIndex) const
{
    if (m_displayOrder.isEmpty()) {
        return std::clamp(displayIndex, 0, std::max(0, m_doc.pageCount() - 1));
    }
    const int clamped = std::clamp(displayIndex, 0, int(m_displayOrder.size()) - 1);
    return m_displayOrder[clamped];
}

int PdfView::displayForSource(int sourceIndex) const
{
    if (m_displayOrder.isEmpty()) {
        return std::clamp(sourceIndex, 0, std::max(0, m_doc.pageCount() - 1));
    }
    const int found = m_displayOrder.indexOf(sourceIndex);
    if (found >= 0) {
        return found;
    }
    // Source page isn't currently shown (shouldn't normally happen — every
    // source page should appear at least once in m_displayOrder — but stay
    // safe rather than returning a nonsensical index).
    return std::clamp(sourceIndex, 0, int(m_displayOrder.size()) - 1);
}

void PdfView::setPageOrder(const QVector<int> &displayOrder)
{
    if (m_displayOrder == displayOrder) {
        return;
    }
    m_displayOrder = displayOrder;
    recomputeLayout();
    updateScrollRanges();
    // Keep showing whichever source page the user was already positioned
    // at — now wherever it landed in the new arrangement — rather than
    // leaving the scrollbar at the same pixel offset, which would silently
    // swap in whatever other page now happens to occupy that offset. If
    // the current page was deleted outright, updateCurrentPageFromScroll()
    // (triggered synchronously by this scroll) self-corrects m_currentPage
    // to whatever's actually now visible.
    goToPage(m_currentPage);
    viewport()->update();
}

void PdfView::updateScrollRanges()
{
    const int vMax = std::max(0, int(std::ceil(m_contentHeight - viewport()->height())));
    const int hMax = std::max(0, int(std::ceil(m_contentWidth - viewport()->width())));
    verticalScrollBar()->setRange(0, vMax);
    verticalScrollBar()->setPageStep(viewport()->height());
    horizontalScrollBar()->setRange(0, hMax);
    horizontalScrollBar()->setPageStep(viewport()->width());
}

void PdfView::clearImageCache()
{
    m_imageCache.clear();
}

qreal PdfView::pageLeft(int pageIndex) const
{
    const int displayIndex = displayForSource(pageIndex);
    if (displayIndex < 0 || displayIndex >= m_layout.size()) {
        return kSideMargin;
    }
    return (m_contentWidth - m_layout[displayIndex].width) / 2.0;
}

const QImage &PdfView::pageImage(int pageIndex)
{
    auto it = m_imageCache.find(pageIndex);
    if (it != m_imageCache.end()) {
        return it.value();
    }
    const qreal dpr = devicePixelRatioF();
    const qreal dpi = kLogicalDpi * m_zoom * dpr;
    QImage img = m_doc.renderPage(pageIndex, dpi, m_rotationQuarters);
    img.setDevicePixelRatio(dpr);
    it = m_imageCache.insert(pageIndex, img);
    return it.value();
}

const QVector<PdfTextWord> &PdfView::wordsForPage(int pageIndex)
{
    auto it = m_wordsCache.find(pageIndex);
    if (it != m_wordsCache.end()) {
        return it.value();
    }
    it = m_wordsCache.insert(pageIndex, m_doc.extractWords(pageIndex));
    return it.value();
}

PdfView::TextPos PdfView::nearestCharPos(int pageIndex, const QPointF &pdfPoint) const
{
    // wordsForPage() is non-const (it caches); callers of this const method
    // only ever invoke it after the words are already cached.
    auto it = m_wordsCache.find(pageIndex);
    if (it == m_wordsCache.end() || it.value().isEmpty()) {
        return {};
    }
    const QVector<PdfTextWord> &words = it.value();

    TextPos best;
    qreal bestDist = std::numeric_limits<qreal>::max();
    for (int w = 0; w < words.size(); ++w) {
        const QVector<QRectF> &charBoxes = words[w].charBoxes;
        for (int c = 0; c < charBoxes.size(); ++c) {
            const qreal d = squaredDistanceToRect(pdfPoint, charBoxes[c]);
            if (d < bestDist) {
                bestDist = d;
                best = {w, c};
            }
        }
    }
    return best;
}

QVector<QRectF> PdfView::selectionRowRects(int pageIndex, TextPos lo, TextPos hi) const
{
    QVector<QRectF> rects;
    auto it = m_wordsCache.find(pageIndex);
    if (it == m_wordsCache.end()) {
        return rects;
    }
    const QVector<PdfTextWord> &words = it.value();

    QRectF current;
    QRectF last;
    bool haveCurrent = false;

    for (int w = lo.word; w <= hi.word && w < words.size(); ++w) {
        const QVector<QRectF> &charBoxes = words[w].charBoxes;
        const int cs = (w == lo.word) ? lo.ch : 0;
        const int ce = (w == hi.word) ? hi.ch : charBoxes.size() - 1;
        for (int c = cs; c <= ce && c < charBoxes.size(); ++c) {
            const QRectF &b = charBoxes[c];
            if (!haveCurrent) {
                current = b;
                haveCurrent = true;
            } else {
                const bool rowChanged = std::abs(b.top() - last.top()) > last.height() * 0.5;
                if (rowChanged) {
                    rects.append(current);
                    current = b;
                } else {
                    current = current.united(b);
                }
            }
            last = b;
        }
    }
    if (haveCurrent) {
        rects.append(current);
    }
    return rects;
}

QVector<PdfView::SearchMatch> PdfView::findAll(const QString &query)
{
    QVector<SearchMatch> results;
    if (query.trimmed().isEmpty() || !m_doc.isLoaded()) {
        return results;
    }

    for (int p = 0; p < m_doc.pageCount(); ++p) {
        const QVector<PdfTextWord> &words = wordsForPage(p);

        QString text;
        QVector<TextPos> positions;
        for (int w = 0; w < words.size(); ++w) {
            const PdfTextWord &word = words[w];
            for (int c = 0; c < word.text.length(); ++c) {
                text += word.text[c];
                positions.append({w, c});
            }
            if (w + 1 < words.size()) {
                const bool rowChanged = std::abs(words[w + 1].bbox.top() - word.bbox.top()) > word.bbox.height() * 0.5;
                if (rowChanged) {
                    text += QLatin1Char('\n');
                    positions.append(TextPos());
                } else if (word.spaceAfter) {
                    text += QLatin1Char(' ');
                    positions.append(TextPos());
                }
            }
        }

        int from = 0;
        while (true) {
            const int idx = text.indexOf(query, from, Qt::CaseInsensitive);
            if (idx < 0) {
                break;
            }
            const int endIdx = idx + query.length() - 1;
            const TextPos start = positions.value(idx);
            const TextPos end = positions.value(endIdx);
            if (start.isValid() && end.isValid()) {
                results.append({p, start, end});
            }
            from = idx + 1;
        }
    }
    return results;
}

void PdfView::showSearchMatch(const SearchMatch &match)
{
    m_searchPage = match.page;
    m_searchStart = match.start;
    m_searchEnd = match.end;

    const int displayIndex = displayForSource(match.page);
    if (displayIndex < 0 || displayIndex >= m_layout.size()) {
        viewport()->update();
        return;
    }
    const QVector<PdfTextWord> &words = wordsForPage(match.page);
    if (match.start.word < 0 || match.start.word >= words.size()) {
        viewport()->update();
        return;
    }
    const QRectF bbox = words[match.start.word].charBoxes.value(match.start.ch);
    const qreal targetY = m_layout[displayIndex].top + bbox.top() * scaleFactor();
    verticalScrollBar()->setValue(qMax(0, int(targetY - viewport()->height() * 0.3)));
    viewport()->update();
}

void PdfView::clearSearchHighlight()
{
    m_searchPage = -1;
    m_searchStart = {};
    m_searchEnd = {};
    viewport()->update();
}

void PdfView::setBookmarkedPages(const QSet<int> &pages)
{
    m_bookmarkedPages = pages;
    viewport()->update();
}

bool PdfView::addHighlightFromSelection()
{
    if (!hasTextSelection()) {
        return false;
    }
    HighlightAnnotation h;
    h.page = m_selPage;
    h.start = std::min(m_selAnchor, m_selHead);
    h.end = std::max(m_selAnchor, m_selHead);
    h.color = m_defaultHighlightColor;
    m_highlights.append(h);

    // The selection has become a permanent highlight; drop the transient
    // (blue) selection now that the yellow highlight covers the same text.
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};

    viewport()->update();
    emit highlightsChanged();
    emit markupsChanged();
    emit sessionContentChanged();
    return true;
}

PdfView::HighlightEntry PdfView::highlightEntry(int index)
{
    HighlightEntry entry;
    if (index < 0 || index >= m_highlights.size()) {
        return entry;
    }
    const HighlightAnnotation &h = m_highlights[index];
    entry.index = index;
    entry.page = h.page;
    entry.note = h.note;
    entry.text = textForRange(h.page, h.start, h.end);
    return entry;
}

void PdfView::setHighlightNote(int index, const QString &note)
{
    if (index < 0 || index >= m_highlights.size()) {
        return;
    }
    m_highlights[index].note = note;
    emit highlightsChanged();
    emit markupsChanged();
    emit sessionContentChanged();
}

void PdfView::setHighlightColor(int index, const QColor &color)
{
    if (index < 0 || index >= m_highlights.size()) {
        return;
    }
    m_highlights[index].color = color;
    m_defaultHighlightColor = color;
    viewport()->update();
    emit highlightsChanged();
    emit markupsChanged();
    emit sessionContentChanged();
}

QColor PdfView::highlightColor(int index) const
{
    if (index < 0 || index >= m_highlights.size()) {
        return m_defaultHighlightColor;
    }
    return m_highlights[index].color;
}

void PdfView::removeHighlight(int index)
{
    if (index < 0 || index >= m_highlights.size()) {
        return;
    }
    m_highlights.remove(index);
    viewport()->update();
    emit highlightsChanged();
    emit markupsChanged();
    emit sessionContentChanged();
}

void PdfView::jumpToHighlight(int index)
{
    if (index < 0 || index >= m_highlights.size()) {
        return;
    }
    const HighlightAnnotation &h = m_highlights[index];
    showSearchMatch({h.page, h.start, h.end});
}

int PdfView::markupCount() const
{
    return m_highlights.size() + m_textBoxes.size() + m_shapes.size();
}

PdfView::MarkupEntry PdfView::markupEntry(int index)
{
    MarkupEntry entry;
    if (index < 0 || index >= markupCount()) {
        return entry;
    }
    entry.index = index;

    if (index < m_highlights.size()) {
        const HighlightAnnotation &h = m_highlights[index];
        entry.kind = MarkupKind::Highlight;
        entry.page = h.page;
        entry.note = h.note;
        entry.description = snippetOf(textForRange(h.page, h.start, h.end));
        return entry;
    }
    index -= m_highlights.size();

    if (index < m_textBoxes.size()) {
        const TextBoxObject &box = m_textBoxes[index];
        entry.kind = MarkupKind::TextBox;
        entry.page = box.page;
        entry.note = box.note;
        QTextDocument doc;
        doc.setHtml(box.html);
        entry.description = snippetOf(doc.toPlainText());
        return entry;
    }
    index -= m_textBoxes.size();

    const ShapeObject &shape = m_shapes[index];
    entry.kind = MarkupKind::Shape;
    entry.page = shape.page;
    entry.note = shape.note;
    entry.description = shapeKindName(shape.kind);
    return entry;
}

void PdfView::setMarkupNote(int index, const QString &note)
{
    if (index < 0 || index >= markupCount()) {
        return;
    }
    if (index < m_highlights.size()) {
        setHighlightNote(index, note); // already emits both signals
        return;
    }
    index -= m_highlights.size();

    if (index < m_textBoxes.size()) {
        m_textBoxes[index].note = note;
    } else {
        m_shapes[index - m_textBoxes.size()].note = note;
    }
    emit markupsChanged();
}

void PdfView::removeMarkup(int index)
{
    if (index < 0 || index >= markupCount()) {
        return;
    }
    if (index < m_highlights.size()) {
        removeHighlight(index); // already emits both signals
        return;
    }
    index -= m_highlights.size();

    // Removing by arbitrary row (not necessarily whatever's currently
    // selected/being-edited/dragged) shifts every later index down by one —
    // unlike deleteSelectedObject(), which only ever removes the selected
    // index itself, so every one of these has to be reconciled here rather
    // than just reset to -1.
    const auto reconcile = [](int &state, int removedIndex) {
        if (state == removedIndex) {
            state = -1;
        } else if (state > removedIndex) {
            --state;
        }
    };

    if (index < m_textBoxes.size()) {
        if (m_editingTextBoxIndex == index) {
            // Tear down the live editor directly rather than via
            // finishEditingTextBox() — that would try to commit/remove
            // this same box itself, racing with the removal below.
            m_textEditWidget->removeEventFilter(this);
            m_textEditWidget->deleteLater();
            m_textEditWidget = nullptr;
            m_editingTextBoxIndex = -1;
        }
        m_textBoxes.remove(index);
        reconcile(m_selectedTextBoxIndex, index);
        reconcile(m_draggingTextBoxIndex, index);
        reconcile(m_resizingTextBoxIndex, index);
        reconcile(m_editingTextBoxIndex, index);
    } else {
        const int shapeIndex = index - m_textBoxes.size();
        m_shapes.remove(shapeIndex);
        reconcile(m_selectedShapeIndex, shapeIndex);
        reconcile(m_draggingShapeIndex, shapeIndex);
        if (m_resizingShapeIndex == shapeIndex) {
            m_resizingPolygonVertex = -1;
        }
        reconcile(m_resizingShapeIndex, shapeIndex);
    }
    viewport()->update();
    emit markupsChanged();
    emit textBoxStateChanged();
    emit sessionContentChanged();
}

void PdfView::jumpToMarkup(int index)
{
    if (index < 0 || index >= markupCount()) {
        return;
    }

    // Beyond just scrolling to the right page, actually select/highlight
    // the specific markup — otherwise, with more than one on the same
    // page, there'd be no way to tell from the main view which one a given
    // Markups panel row refers to. Clear every other selection kind first
    // so exactly one thing ever looks selected at a time.
    const bool hadObjectSelection = m_selectedPastedIndex >= 0 || m_selectedTextBoxIndex >= 0
        || m_selectedShapeIndex >= 0 || m_selectedExistingAnnotationIndex >= 0;
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    m_selectedPastedIndex = -1;
    m_selectedTextBoxIndex = -1;
    m_selectedShapeIndex = -1;
    m_selectedExistingAnnotationIndex = -1;
    if (hadObjectSelection) {
        emit textBoxStateChanged();
    }

    if (index < m_highlights.size()) {
        jumpToHighlight(index);
        return;
    }
    index -= m_highlights.size();

    if (index < m_textBoxes.size()) {
        m_selectedTextBoxIndex = index;
        scrollToShowRect(m_textBoxes[index].page, m_textBoxes[index].rectPoints);
    } else {
        const int shapeIndex = index - m_textBoxes.size();
        m_selectedShapeIndex = shapeIndex;
        const ShapeObject &shape = m_shapes[shapeIndex];
        if (isPolygonShape(shape.kind)) {
            QRectF bbox;
            for (const QPointF &pt : shape.polygonPoints) {
                bbox = bbox.isNull() ? QRectF(pt, QSizeF(0, 0)) : bbox.united(QRectF(pt, QSizeF(0, 0)));
            }
            scrollToShowRect(shape.page, bbox);
        } else {
            scrollToShowRect(shape.page, shape.rectPoints.normalized());
        }
    }
    viewport()->update();
    emit textBoxStateChanged();
}

const QVector<QRectF> &PdfView::imageBoxesForPage(int pageIndex)
{
    auto it = m_imageBoxesCache.find(pageIndex);
    if (it != m_imageBoxesCache.end()) {
        return it.value();
    }
    it = m_imageBoxesCache.insert(pageIndex, m_doc.imageBoxes(pageIndex));
    return it.value();
}

bool PdfView::tryStartImageSelection(int pageIndex, const QPointF &pdfPoint)
{
    const QVector<QRectF> &boxes = imageBoxesForPage(pageIndex);
    bool found = false;
    QRectF best;
    for (const QRectF &box : boxes) {
        if (box.contains(pdfPoint) && (!found || box.width() * box.height() < best.width() * best.height())) {
            best = box;
            found = true;
        }
    }
    if (!found) {
        return false;
    }

    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = pageIndex;
    m_regionAnchor = best.topLeft();
    m_regionHead = best.bottomRight();
    viewport()->update();
    return true;
}

QPointF PdfView::projectToPage(int pageIndex, const QPoint &viewportPos) const
{
    const int displayIndex = displayForSource(pageIndex);
    if (displayIndex < 0 || displayIndex >= m_layout.size()) {
        return {};
    }
    const PageLayout &pl = m_layout[displayIndex];
    const qreal left = pageLeft(pageIndex);
    const qreal globalX = viewportPos.x() + horizontalScrollBar()->value();
    const qreal globalY = viewportPos.y() + verticalScrollBar()->value();

    qreal localXpx = globalX - left;
    qreal localYpx = globalY - pl.top;
    localXpx = std::clamp(localXpx, 0.0, pl.width);
    localYpx = std::clamp(localYpx, 0.0, pl.height);

    const qreal scale = scaleFactor();
    return QPointF(localXpx / scale, localYpx / scale);
}

bool PdfView::hitTestPage(const QPoint &viewportPos, int *pageIndexOut, QPointF *pdfPointOut) const
{
    if (m_layout.isEmpty()) {
        return false;
    }
    const qreal globalY = viewportPos.y() + verticalScrollBar()->value();

    int bestPage = 0;
    qreal bestDist = std::numeric_limits<qreal>::max();
    for (int i = 0; i < m_layout.size(); ++i) {
        const qreal top = m_layout[i].top;
        const qreal bottom = top + m_layout[i].height;
        const qreal dist = globalY < top ? top - globalY : (globalY > bottom ? globalY - bottom : 0.0);
        if (dist < bestDist) {
            bestDist = dist;
            bestPage = i;
        }
    }

    const int bestSourcePage = sourceForDisplay(bestPage);
    if (pageIndexOut) {
        *pageIndexOut = bestSourcePage;
    }
    if (pdfPointOut) {
        *pdfPointOut = projectToPage(bestSourcePage, viewportPos);
    }
    return true;
}

void PdfView::updateCurrentPageFromScroll()
{
    if (m_layout.isEmpty()) {
        return;
    }
    const qreal refY = verticalScrollBar()->value() + viewport()->height() * 0.3;
    int found = m_layout.size() - 1;
    for (int i = 0; i < m_layout.size(); ++i) {
        const qreal top = m_layout[i].top;
        const qreal bottom = top + m_layout[i].height;
        if (refY >= top && refY <= bottom) {
            found = i;
            break;
        }
        if (refY < top) {
            found = i;
            break;
        }
    }
    const int foundSource = sourceForDisplay(found);
    if (foundSource != m_currentPage) {
        m_currentPage = foundSource;
        emit currentPageChanged(m_currentPage);
    }
}

void PdfView::zoomIn()
{
    setZoom(m_zoom * 1.25);
}

void PdfView::zoomOut()
{
    setZoom(m_zoom / 1.25);
}

void PdfView::setZoom(double zoom)
{
    zoom = std::clamp(zoom, 0.1, 8.0);
    if (qFuzzyCompare(zoom, m_zoom)) {
        return;
    }
    finishEditingTextBox(); // re-flowing the live editor's font size mid-zoom isn't worth it
    const int pageBefore = m_currentPage;
    m_zoom = zoom;
    clearImageCache();
    recomputeLayout();
    updateScrollRanges();
    goToPage(pageBefore);
    viewport()->update();
    emit zoomChanged(m_zoom);
}

void PdfView::zoomToActualSize()
{
    setZoom(1.0);
}

void PdfView::zoomToFitWidth()
{
    if (!m_doc.isLoaded() || pageCount() == 0) {
        return;
    }
    const qreal widthPts = m_doc.pageSizePoints(m_currentPage).width();
    if (widthPts <= 0) {
        return;
    }
    const qreal avail = viewport()->width() - 2 * kSideMargin;
    const qreal newScale = avail / widthPts;
    setZoom(newScale * 72.0 / kLogicalDpi);
}

void PdfView::goToDisplayPosition(int displayIndex)
{
    if (m_layout.isEmpty()) {
        return;
    }
    displayIndex = std::clamp(displayIndex, 0, int(m_layout.size()) - 1);
    verticalScrollBar()->setValue(int(m_layout[displayIndex].top));
}

void PdfView::scrollToShowRect(int sourcePage, const QRectF &rectPoints)
{
    const int displayIndex = displayForSource(sourcePage);
    if (displayIndex < 0 || displayIndex >= m_layout.size()) {
        return;
    }
    const qreal centerY = m_layout[displayIndex].top + rectPoints.center().y() * scaleFactor();
    verticalScrollBar()->setValue(qMax(0, int(centerY - viewport()->height() / 2.0)));
}

void PdfView::goToPage(int pageIndex)
{
    goToDisplayPosition(displayForSource(pageIndex));
}

QString PdfView::selectedText()
{
    if (!hasTextSelection()) {
        return {};
    }
    return textForRange(m_selPage, std::min(m_selAnchor, m_selHead), std::max(m_selAnchor, m_selHead));
}

QString PdfView::textForRange(int pageIndex, TextPos lo, TextPos hi)
{
    const QVector<PdfTextWord> &words = wordsForPage(pageIndex);
    if (lo.word < 0 || hi.word >= words.size()) {
        return {};
    }

    QString result;
    for (int w = lo.word; w <= hi.word; ++w) {
        const PdfTextWord &word = words[w];
        const int cs = (w == lo.word) ? lo.ch : 0;
        const int ce = (w == hi.word) ? hi.ch : word.text.length() - 1;
        result += word.text.mid(cs, ce - cs + 1);

        const bool includesWordEnd = (ce == word.text.length() - 1);
        if (includesWordEnd && w < hi.word) {
            const bool rowChanged = w + 1 < words.size()
                && std::abs(words[w + 1].bbox.top() - word.bbox.top()) > word.bbox.height() * 0.5;
            if (rowChanged) {
                result += QLatin1Char('\n');
            } else if (word.spaceAfter) {
                result += QLatin1Char(' ');
            }
        }
    }
    return result;
}

void PdfView::copySelectionToClipboard()
{
    // A region selection takes priority if one is active, whether it came
    // from the marquee "Select Image/Region" tool or from clicking directly
    // on a detected embedded image in normal text mode.
    if (m_regionPage >= 0) {
        const QRectF region = QRectF(m_regionAnchor, m_regionHead).normalized();
        if (region.width() < 1.0 || region.height() < 1.0) {
            return;
        }
        constexpr qreal kCopyDpi = 300.0;
        const QImage img = m_doc.renderPageRegion(m_regionPage, kCopyDpi, region);
        if (!img.isNull()) {
            QGuiApplication::clipboard()->setImage(img);
        }
        return;
    }

    const QString text = selectedText();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void PdfView::setSelectionMode(SelectionMode mode)
{
    if (mode == m_mode) {
        return;
    }
    finishEditingTextBox();
    m_mode = mode;

    // Clear whichever selection belongs to the mode we're leaving.
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    m_regionSelecting = false;
    m_selectedPastedIndex = -1;
    m_selectedTextBoxIndex = -1;
    m_selectedShapeIndex = -1;
    m_selectedExistingAnnotationIndex = -1;
    m_placingShape = false;
    m_shapePlacingPage = -1;
    m_placingPolygon = false;
    m_polygonPlacingPage = -1;
    m_polygonPoints.clear();

    viewport()->setCursor(m_mode == SelectionMode::Region   ? Qt::CrossCursor
                           : m_mode == SelectionMode::AddText ? Qt::IBeamCursor
                           : m_mode == SelectionMode::AddShape ? Qt::CrossCursor
                                                               : Qt::ArrowCursor);
    viewport()->update();
    emit textBoxStateChanged();
    emit selectionModeChanged(m_mode);
}

void PdfView::startPlacingShape(ShapeKind kind)
{
    // Switching tools mid-placement (e.g. clicking Rectangle while still
    // mid-polygon) — setSelectionMode() below is a no-op if already in
    // AddShape mode, so this has to be cancelled explicitly here too.
    if (m_placingPolygon) {
        finishPlacingPolygon(false);
    }
    m_pendingShapeKind = kind;
    setSelectionMode(SelectionMode::AddShape);
}

void PdfView::startPlacingPolygon()
{
    if (m_placingPolygon) {
        finishPlacingPolygon(false);
    }
    m_pendingShapeKind = ShapeKind::Polygon;
    setSelectionMode(SelectionMode::AddShape);
}

bool PdfView::hasTextSelection() const
{
    return m_selPage >= 0 && m_selAnchor.isValid() && m_selHead.isValid();
}

bool PdfView::hasImageSelection() const
{
    return m_regionPage >= 0;
}

PdfView::SelectionRange PdfView::currentTextSelection() const
{
    if (!hasTextSelection()) {
        return {};
    }
    SelectionRange range;
    range.page = m_selPage;
    range.start = std::min(m_selAnchor, m_selHead);
    range.end = std::max(m_selAnchor, m_selHead);
    return range;
}

void PdfView::selectAllOnPage()
{
    if (!m_doc.isLoaded()) {
        return;
    }
    const QVector<PdfTextWord> &words = wordsForPage(m_currentPage);

    int first = 0;
    while (first < words.size() && words[first].charBoxes.isEmpty()) {
        ++first;
    }
    int last = words.size() - 1;
    while (last >= 0 && words[last].charBoxes.isEmpty()) {
        --last;
    }
    if (first > last) {
        return;
    }

    m_regionPage = -1;
    m_selectedPastedIndex = -1;
    m_selectedExistingAnnotationIndex = -1;
    m_selPage = m_currentPage;
    m_selAnchor = {first, 0};
    m_selHead = {last, int(words[last].charBoxes.size()) - 1};
    viewport()->update();
}

void PdfView::rotateClockwise()
{
    finishEditingTextBox();
    m_rotationQuarters = (m_rotationQuarters + 1) % 4;
    clearImageCache();
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    const int pageBefore = m_currentPage;
    recomputeLayout();
    updateScrollRanges();
    goToPage(pageBefore);
    viewport()->update();
    emit rotationChanged(m_rotationQuarters);
}

void PdfView::rotateCounterclockwise()
{
    finishEditingTextBox();
    m_rotationQuarters = (m_rotationQuarters + 3) % 4;
    clearImageCache();
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    const int pageBefore = m_currentPage;
    recomputeLayout();
    updateScrollRanges();
    goToPage(pageBefore);
    viewport()->update();
    emit rotationChanged(m_rotationQuarters);
}

int PdfView::pastedImageAt(int pageIndex, const QPointF &pdfPoint) const
{
    for (int i = m_pastedImages.size() - 1; i >= 0; --i) {
        const PastedImage &obj = m_pastedImages[i];
        if (obj.page == pageIndex && obj.rectPoints.contains(pdfPoint)) {
            return i;
        }
    }
    return -1;
}

int PdfView::existingAnnotationAt(int pageIndex, const QPointF &pdfPoint) const
{
    for (int i = m_existingAnnotations.size() - 1; i >= 0; --i) {
        const PdfDocument::ExistingAnnotationInfo &info = m_existingAnnotations[i];
        if (info.pageIndex == pageIndex && info.boundaryPoints.contains(pdfPoint)) {
            return i;
        }
    }
    return -1;
}

QRect PdfView::pageScreenRect(int pageIndex) const
{
    const int displayIndex = displayForSource(pageIndex);
    if (displayIndex < 0 || displayIndex >= m_layout.size()) {
        return {};
    }
    const PageLayout &pl = m_layout[displayIndex];
    const int scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();
    const qreal left = pageLeft(pageIndex);
    return QRect(int(std::round(left - scrollX)), int(std::round(pl.top - scrollY)), int(std::round(pl.width)),
                 int(std::round(pl.height)));
}

int PdfView::textBoxAt(int pageIndex, const QPointF &pdfPoint) const
{
    for (int i = m_textBoxes.size() - 1; i >= 0; --i) {
        const TextBoxObject &obj = m_textBoxes[i];
        if (obj.page == pageIndex && obj.rectPoints.contains(pdfPoint)) {
            return i;
        }
    }
    return -1;
}

int PdfView::shapeAt(int pageIndex, const QPointF &pdfPoint) const
{
    const qreal tol = 8.0 / scaleFactor(); // ~8 screen px hit tolerance, converted to points
    for (int i = m_shapes.size() - 1; i >= 0; --i) {
        const ShapeObject &obj = m_shapes[i];
        if (obj.page != pageIndex) {
            continue;
        }
        if (isLineLikeShape(obj.kind)) {
            if (squaredDistanceToSegment(pdfPoint, obj.rectPoints.topLeft(), obj.rectPoints.bottomRight())
                <= tol * tol) {
                return i;
            }
        } else if (isPolygonShape(obj.kind)) {
            if (QPolygonF(obj.polygonPoints).containsPoint(pdfPoint, Qt::OddEvenFill)) {
                return i;
            }
        } else if (obj.rectPoints.contains(pdfPoint)) {
            return i;
        }
    }
    return -1;
}

PdfView::ResizeHandle PdfView::resizeHandleAt(const QRectF &rectPoints, const QPointF &pdfPoint) const
{
    const qreal tol = 8.0 / scaleFactor(); // ~8 screen px hit tolerance, converted to points
    const QRectF &r = rectPoints;
    const bool nearLeft = std::abs(pdfPoint.x() - r.left()) <= tol;
    const bool nearRight = std::abs(pdfPoint.x() - r.right()) <= tol;
    const bool nearTop = std::abs(pdfPoint.y() - r.top()) <= tol;
    const bool nearBottom = std::abs(pdfPoint.y() - r.bottom()) <= tol;
    const bool withinX = pdfPoint.x() >= r.left() - tol && pdfPoint.x() <= r.right() + tol;
    const bool withinY = pdfPoint.y() >= r.top() - tol && pdfPoint.y() <= r.bottom() + tol;

    if (nearLeft && nearTop) {
        return ResizeHandle::TopLeft;
    }
    if (nearRight && nearTop) {
        return ResizeHandle::TopRight;
    }
    if (nearLeft && nearBottom) {
        return ResizeHandle::BottomLeft;
    }
    if (nearRight && nearBottom) {
        return ResizeHandle::BottomRight;
    }
    if (nearTop && withinX) {
        return ResizeHandle::Top;
    }
    if (nearBottom && withinX) {
        return ResizeHandle::Bottom;
    }
    if (nearLeft && withinY) {
        return ResizeHandle::Left;
    }
    if (nearRight && withinY) {
        return ResizeHandle::Right;
    }
    return ResizeHandle::None;
}

PdfView::ResizeHandle PdfView::lineHandleAt(const QRectF &rectPoints, const QPointF &pdfPoint) const
{
    const qreal tol = 8.0 / scaleFactor();
    if (QLineF(pdfPoint, rectPoints.topLeft()).length() <= tol) {
        return ResizeHandle::TopLeft; // start point
    }
    if (QLineF(pdfPoint, rectPoints.bottomRight()).length() <= tol) {
        return ResizeHandle::BottomRight; // end point
    }
    return ResizeHandle::None;
}

int PdfView::polygonVertexAt(const QVector<QPointF> &points, const QPointF &pdfPoint) const
{
    const qreal tol = 8.0 / scaleFactor();
    for (int i = 0; i < points.size(); ++i) {
        if (QLineF(pdfPoint, points[i]).length() <= tol) {
            return i;
        }
    }
    return -1;
}

Qt::CursorShape PdfView::cursorForHandle(ResizeHandle handle) const
{
    switch (handle) {
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case ResizeHandle::Top:
    case ResizeHandle::Bottom:
        return Qt::SizeVerCursor;
    case ResizeHandle::Left:
    case ResizeHandle::Right:
        return Qt::SizeHorCursor;
    default:
        return Qt::ArrowCursor;
    }
}

void PdfView::updateTextEditGeometry()
{
    if (!m_textEditWidget || m_editingTextBoxIndex < 0 || m_editingTextBoxIndex >= m_textBoxes.size()) {
        return;
    }
    const TextBoxObject &obj = m_textBoxes[m_editingTextBoxIndex];
    const QRect pageRect = pageScreenRect(obj.page);
    const qreal scale = scaleFactor();
    const QRect r(pageRect.left() + int(std::round(obj.rectPoints.left() * scale)),
                  pageRect.top() + int(std::round(obj.rectPoints.top() * scale)),
                  int(std::round(obj.rectPoints.width() * scale)), int(std::round(obj.rectPoints.height() * scale)));
    m_textEditWidget->setGeometry(r);
}

void PdfView::updateTextEditBackground()
{
    if (!m_textEditWidget || m_editingTextBoxIndex < 0 || m_editingTextBoxIndex >= m_textBoxes.size()) {
        return;
    }
    const TextBoxObject &obj = m_textBoxes[m_editingTextBoxIndex];
    QColor fill = obj.fillColor;
    fill.setAlphaF(float(obj.fillOpacity));
    const int borderPx = qMax(0, int(std::round(obj.borderWidthPts * scaleFactor())));
    const QString border = borderPx > 0
        ? QString("%1px solid %2").arg(borderPx).arg(obj.borderColor.name(QColor::HexRgb))
        : QStringLiteral("none");
    m_textEditWidget->setStyleSheet(
        QString("QTextEdit { background: %1; border: %2; }").arg(fill.name(QColor::HexArgb), border));
}

void PdfView::startEditingTextBox(int index)
{
    if (index < 0 || index >= m_textBoxes.size()) {
        return;
    }
    if (m_editingTextBoxIndex == index && m_textEditWidget) {
        m_textEditWidget->setFocus();
        return;
    }
    if (m_editingTextBoxIndex >= 0) {
        finishEditingTextBox();
    }

    m_editingTextBoxIndex = index;
    m_selectedTextBoxIndex = index;
    m_selectedPastedIndex = -1;
    m_selectedExistingAnnotationIndex = -1;
    const TextBoxObject &obj = m_textBoxes[index];

    m_textEditWidget = new QTextEdit(viewport());
    m_textEditWidget->setAcceptRichText(true);
    if (!obj.html.isEmpty()) {
        m_textEditWidget->setHtml(obj.html);
    } else {
        // Fresh box, no content yet: start from whatever formatting the
        // user was last using (see the "sticky defaults" members) rather
        // than a fixed Helvetica 14 every time.
        QFont font = m_textEditWidget->font();
        font.setFamily(m_defaultTextFontFamily);
        font.setBold(m_defaultTextBold);
        font.setItalic(m_defaultTextItalic);
        font.setUnderline(m_defaultTextUnderline);
        font.setPixelSize(qMax(1, int(std::round(m_defaultTextFontPointSize * scaleFactor()))));
        m_textEditWidget->setFont(font);
        m_textEditWidget->setTextColor(m_defaultTextColor);
    }
    m_textEditWidget->setFrameStyle(QFrame::NoFrame);
    m_textEditWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textEditWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textEditWidget->setLineWrapMode(QTextEdit::WidgetWidth);
    updateTextEditBackground();

    // Deliberately no auto-grow-on-type here: the box stays exactly the size
    // it was placed/resized at. Overflowing text is clipped when rendering
    // (see paintEvent) rather than pushing the box around — use "Fit Box to
    // Text" / "Size Text to Box" in the Format panel to resolve overflow
    // explicitly instead.
    connect(m_textEditWidget, &QTextEdit::cursorPositionChanged, this, &PdfView::textBoxStateChanged);
    connect(m_textEditWidget, &QTextEdit::currentCharFormatChanged, this, &PdfView::textBoxStateChanged);

    m_textEditWidget->installEventFilter(this);
    updateTextEditGeometry();
    m_textEditWidget->show();
    m_textEditWidget->setFocus();
    QTextCursor cursor = m_textEditWidget->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_textEditWidget->setTextCursor(cursor);

    viewport()->update();
    emit textBoxStateChanged();
}

void PdfView::finishEditingTextBox()
{
    if (m_editingTextBoxIndex < 0 || !m_textEditWidget) {
        return;
    }
    const int idx = m_editingTextBoxIndex;
    const bool isEmpty = m_textEditWidget->document()->isEmpty();
    const QString html = m_textEditWidget->toHtml();

    if (!isEmpty) {
        // Remember the formatting at the cursor's last position as the new
        // "sticky default" for the next fresh text box.
        m_defaultTextFontFamily = m_textEditWidget->fontFamily();
        if (m_textEditWidget->fontPointSize() > 0) {
            m_defaultTextFontPointSize = m_textEditWidget->fontPointSize();
        }
        m_defaultTextBold = m_textEditWidget->fontWeight() > QFont::Normal;
        m_defaultTextItalic = m_textEditWidget->fontItalic();
        m_defaultTextUnderline = m_textEditWidget->fontUnderline();
        m_defaultTextColor = m_textEditWidget->textColor();
    }

    m_textEditWidget->removeEventFilter(this);
    m_textEditWidget->deleteLater();
    m_textEditWidget = nullptr;
    m_editingTextBoxIndex = -1;

    if (idx >= 0 && idx < m_textBoxes.size()) {
        if (isEmpty) {
            m_textBoxes.remove(idx);
            m_selectedTextBoxIndex = -1;
        } else {
            m_textBoxes[idx].html = html;
            m_textBoxes[idx].htmlScaleFactor = scaleFactor();
            m_selectedTextBoxIndex = idx;
            // Only committed (non-empty) boxes ever appear in the Markups
            // panel, so only their commit needs to announce a change —
            // discarding an empty box never showed up there in the first
            // place.
            emit markupsChanged();
            emit sessionContentChanged();
        }
    }
    viewport()->update();
    emit textBoxStateChanged();
}

void PdfView::refocusActiveEditor()
{
    if (m_textEditWidget) {
        m_textEditWidget->setFocus();
    }
}

QTextEdit *PdfView::beginFormattingSelectedTextBox()
{
    if (m_textEditWidget) {
        return m_textEditWidget;
    }
    if (!hasSelectedTextBox()) {
        return nullptr;
    }
    startEditingTextBox(m_selectedTextBoxIndex);
    if (m_textEditWidget) {
        QTextCursor cursor = m_textEditWidget->textCursor();
        cursor.select(QTextCursor::Document);
        m_textEditWidget->setTextCursor(cursor);
    }
    return m_textEditWidget;
}

PdfView::TextFormatSnapshot PdfView::selectedTextBoxFormatSnapshot() const
{
    TextFormatSnapshot snap;
    if (!hasSelectedTextBox()) {
        return snap;
    }
    const TextBoxObject &obj = m_textBoxes[m_selectedTextBoxIndex];
    if (obj.html.isEmpty()) {
        return snap;
    }
    QTextDocument doc;
    doc.setHtml(obj.html);
    QTextCursor probe(&doc);
    probe.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    const QTextCharFormat fmt = probe.charFormat();
    const QFont font = fmt.font();
    if (!font.family().isEmpty()) {
        snap.fontFamily = font.family();
    }
    if (fmt.fontPointSize() > 0) {
        snap.fontPointSize = fmt.fontPointSize();
    } else if (font.pointSizeF() > 0) {
        snap.fontPointSize = font.pointSizeF();
    }
    snap.bold = font.bold();
    snap.italic = font.italic();
    snap.underline = font.underline();
    if (fmt.foreground().style() != Qt::NoBrush) {
        snap.textColor = fmt.foreground().color();
    }
    if (fmt.background().style() != Qt::NoBrush) {
        snap.highlightColor = fmt.background().color();
    }
    const QTextBlock block = doc.begin();
    if (block.isValid()) {
        snap.alignment = block.blockFormat().alignment();
    }
    return snap;
}

QColor PdfView::selectedTextBoxBorderColor() const
{
    return hasSelectedTextBox() ? m_textBoxes[m_selectedTextBoxIndex].borderColor : QColor(Qt::black);
}

qreal PdfView::selectedTextBoxBorderWidthPts() const
{
    return hasSelectedTextBox() ? m_textBoxes[m_selectedTextBoxIndex].borderWidthPts : 0.0;
}

QColor PdfView::selectedTextBoxFillColor() const
{
    return hasSelectedTextBox() ? m_textBoxes[m_selectedTextBoxIndex].fillColor : QColor(Qt::white);
}

qreal PdfView::selectedTextBoxFillOpacity() const
{
    return hasSelectedTextBox() ? m_textBoxes[m_selectedTextBoxIndex].fillOpacity : 0.0;
}

void PdfView::setSelectedTextBoxBorder(const QColor &color, qreal widthPts)
{
    if (!hasSelectedTextBox()) {
        return;
    }
    m_textBoxes[m_selectedTextBoxIndex].borderColor = color;
    m_textBoxes[m_selectedTextBoxIndex].borderWidthPts = qMax(0.0, widthPts);
    m_defaultTextBoxBorderColor = color;
    m_defaultTextBoxBorderWidthPts = qMax(0.0, widthPts);
    if (m_editingTextBoxIndex == m_selectedTextBoxIndex) {
        updateTextEditBackground();
    }
    viewport()->update();
    emit textBoxStateChanged();
}

void PdfView::setSelectedTextBoxFill(const QColor &color, qreal opacity)
{
    if (!hasSelectedTextBox()) {
        return;
    }
    m_textBoxes[m_selectedTextBoxIndex].fillColor = color;
    m_textBoxes[m_selectedTextBoxIndex].fillOpacity = std::clamp(opacity, 0.0, 1.0);
    m_defaultTextBoxFillColor = color;
    m_defaultTextBoxFillOpacity = std::clamp(opacity, 0.0, 1.0);
    if (m_editingTextBoxIndex == m_selectedTextBoxIndex) {
        updateTextEditBackground();
    }
    viewport()->update();
    emit textBoxStateChanged();
}

bool PdfView::hasSelectedLineLikeShape() const
{
    return hasSelectedShape() && isLineLikeShape(m_shapes[m_selectedShapeIndex].kind);
}

QColor PdfView::selectedBorderColor() const
{
    if (hasSelectedTextBox()) {
        return selectedTextBoxBorderColor();
    }
    return hasSelectedShape() ? m_shapes[m_selectedShapeIndex].borderColor : QColor(Qt::black);
}

qreal PdfView::selectedBorderWidthPts() const
{
    if (hasSelectedTextBox()) {
        return selectedTextBoxBorderWidthPts();
    }
    return hasSelectedShape() ? m_shapes[m_selectedShapeIndex].borderWidthPts : 0.0;
}

QColor PdfView::selectedFillColor() const
{
    if (hasSelectedTextBox()) {
        return selectedTextBoxFillColor();
    }
    return hasSelectedShape() ? m_shapes[m_selectedShapeIndex].fillColor : QColor(Qt::white);
}

qreal PdfView::selectedFillOpacity() const
{
    if (hasSelectedTextBox()) {
        return selectedTextBoxFillOpacity();
    }
    return hasSelectedShape() ? m_shapes[m_selectedShapeIndex].fillOpacity : 0.0;
}

void PdfView::setSelectedBorder(const QColor &color, qreal widthPts)
{
    if (hasSelectedTextBox()) {
        setSelectedTextBoxBorder(color, widthPts);
        return;
    }
    if (!hasSelectedShape()) {
        return;
    }
    m_shapes[m_selectedShapeIndex].borderColor = color;
    m_shapes[m_selectedShapeIndex].borderWidthPts = qMax(0.0, widthPts);
    m_defaultShapeBorderColor = color;
    m_defaultShapeBorderWidthPts = qMax(0.0, widthPts);
    viewport()->update();
    emit textBoxStateChanged();
}

void PdfView::setSelectedFill(const QColor &color, qreal opacity)
{
    if (hasSelectedTextBox()) {
        setSelectedTextBoxFill(color, opacity);
        return;
    }
    if (!hasSelectedShape()) {
        return;
    }
    m_shapes[m_selectedShapeIndex].fillColor = color;
    m_shapes[m_selectedShapeIndex].fillOpacity = std::clamp(opacity, 0.0, 1.0);
    m_defaultShapeFillColor = color;
    m_defaultShapeFillOpacity = std::clamp(opacity, 0.0, 1.0);
    viewport()->update();
    emit textBoxStateChanged();
}

qreal PdfView::measureTextBoxContentHeightPts(int index) const
{
    if (index < 0 || index >= m_textBoxes.size()) {
        return 0.0;
    }
    const TextBoxObject &obj = m_textBoxes[index];
    if (m_editingTextBoxIndex == index && m_textEditWidget) {
        const qreal scale = scaleFactor();
        return (m_textEditWidget->document()->size().height() + 8) / scale;
    }
    QTextDocument doc;
    doc.setHtml(obj.html);
    const qreal scale = obj.htmlScaleFactor > 0.0 ? obj.htmlScaleFactor : 1.0;
    doc.setTextWidth(obj.rectPoints.width() * scale);
    return (doc.size().height() + 8) / scale;
}

void PdfView::fitSelectedTextBoxToContent()
{
    if (!hasSelectedTextBox()) {
        return;
    }
    const qreal neededHeightPts = qMax(18.0, measureTextBoxContentHeightPts(m_selectedTextBoxIndex));
    m_textBoxes[m_selectedTextBoxIndex].rectPoints.setHeight(neededHeightPts);
    if (m_editingTextBoxIndex == m_selectedTextBoxIndex) {
        updateTextEditGeometry();
    }
    viewport()->update();
    emit textBoxStateChanged();
}

void PdfView::shrinkSelectedTextBoxToFit()
{
    if (!hasSelectedTextBox()) {
        return;
    }
    TextBoxObject &obj = m_textBoxes[m_selectedTextBoxIndex];
    const qreal availableHeightPts = obj.rectPoints.height();
    const qreal neededHeightPts = measureTextBoxContentHeightPts(m_selectedTextBoxIndex);
    if (neededHeightPts <= availableHeightPts || neededHeightPts <= 0.0) {
        return; // already fits
    }
    const qreal factor = qMax(0.1, (availableHeightPts / neededHeightPts) * 0.95);

    if (m_editingTextBoxIndex == m_selectedTextBoxIndex && m_textEditWidget) {
        scaleDocumentFontSizes(m_textEditWidget->document(), factor);
    } else {
        QTextDocument doc;
        doc.setHtml(obj.html);
        scaleDocumentFontSizes(&doc, factor);
        obj.html = doc.toHtml();
    }
    viewport()->update();
    emit textBoxStateChanged();
}

void PdfView::pasteImageFromClipboard()
{
    if (!m_doc.isLoaded()) {
        return;
    }
    const QImage img = QGuiApplication::clipboard()->image();
    if (img.isNull() || img.width() <= 0 || img.height() <= 0) {
        return;
    }

    const QSizeF pageSize = m_doc.pageSizePoints(m_currentPage);
    const qreal aspect = qreal(img.height()) / qreal(img.width());

    // Clipboard images carry no PDF-point size; start at a reasonable
    // fraction of the page and let the user drag it if they want it bigger.
    qreal w = qMin(pageSize.width() * 0.5, qreal(img.width()) * 72.0 / 96.0);
    qreal h = w * aspect;
    if (pageSize.height() > 0 && h > pageSize.height() * 0.5) {
        h = pageSize.height() * 0.5;
        w = h / aspect;
    }

    const QPoint viewportCenter(viewport()->width() / 2, viewport()->height() / 2);
    const QPointF center = projectToPage(m_currentPage, viewportCenter);

    PastedImage obj;
    obj.page = m_currentPage;
    obj.rectPoints = QRectF(center.x() - w / 2.0, center.y() - h / 2.0, w, h);
    obj.image = img;
    m_pastedImages.append(obj);

    m_selectedPastedIndex = m_pastedImages.size() - 1;
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    viewport()->update();
    emit sessionContentChanged();
}

void PdfView::deleteSelectedObject()
{
    if (m_selectedPastedIndex >= 0 && m_selectedPastedIndex < m_pastedImages.size()) {
        m_pastedImages.remove(m_selectedPastedIndex);
        m_selectedPastedIndex = -1;
        m_draggingPastedIndex = -1;
        viewport()->update();
        emit sessionContentChanged();
        return;
    }
    if (m_selectedTextBoxIndex >= 0 && m_selectedTextBoxIndex < m_textBoxes.size()) {
        m_textBoxes.remove(m_selectedTextBoxIndex);
        m_selectedTextBoxIndex = -1;
        m_draggingTextBoxIndex = -1;
        m_resizingTextBoxIndex = -1;
        viewport()->update();
        emit textBoxStateChanged();
        emit markupsChanged();
        emit sessionContentChanged();
        return;
    }
    if (m_selectedShapeIndex >= 0 && m_selectedShapeIndex < m_shapes.size()) {
        m_shapes.remove(m_selectedShapeIndex);
        m_selectedShapeIndex = -1;
        m_draggingShapeIndex = -1;
        m_resizingShapeIndex = -1;
        m_resizingPolygonVertex = -1;
        viewport()->update();
        emit textBoxStateChanged();
        emit markupsChanged();
        emit sessionContentChanged();
        return;
    }
    if (m_selectedExistingAnnotationIndex >= 0 && m_selectedExistingAnnotationIndex < m_existingAnnotations.size()) {
        // Not an emit sessionContentChanged() here, same as the Markups
        // panel's Remove action — removing a file-native annotation isn't
        // Undo-covered (see removeExistingAnnotationAt's header comment).
        removeExistingAnnotationAt(m_selectedExistingAnnotationIndex);
        emit textBoxStateChanged();
    }
}

bool PdfView::pageHasMarkups(int sourcePage) const
{
    for (const HighlightAnnotation &h : m_highlights) {
        if (h.page == sourcePage) {
            return true;
        }
    }
    for (const PastedImage &obj : m_pastedImages) {
        if (obj.page == sourcePage) {
            return true;
        }
    }
    for (const TextBoxObject &obj : m_textBoxes) {
        if (obj.page == sourcePage) {
            return true;
        }
    }
    for (const ShapeObject &obj : m_shapes) {
        if (obj.page == sourcePage) {
            return true;
        }
    }
    return false;
}

QVector<PdfView::NoteExport> PdfView::notesForPage(int sourcePage) const
{
    QVector<NoteExport> result;

    for (const HighlightAnnotation &h : m_highlights) {
        if (h.page != sourcePage || h.note.isEmpty()) {
            continue;
        }
        const QVector<QRectF> rows = selectionRowRects(sourcePage, h.start, h.end);
        QRectF anchor;
        for (const QRectF &r : rows) {
            anchor = anchor.isNull() ? r : anchor.united(r);
        }
        if (!anchor.isNull()) {
            result.append({anchor, h.note});
        }
    }
    for (const TextBoxObject &obj : m_textBoxes) {
        if (obj.page == sourcePage && !obj.note.isEmpty()) {
            result.append({obj.rectPoints, obj.note});
        }
    }
    for (const ShapeObject &obj : m_shapes) {
        if (obj.page != sourcePage || obj.note.isEmpty()) {
            continue;
        }
        if (isPolygonShape(obj.kind)) {
            QRectF bbox;
            for (const QPointF &pt : obj.polygonPoints) {
                bbox = bbox.isNull() ? QRectF(pt, QSizeF(0, 0)) : bbox.united(QRectF(pt, QSizeF(0, 0)));
            }
            if (!bbox.isNull()) {
                result.append({bbox, obj.note});
            }
        } else {
            result.append({obj.rectPoints.normalized(), obj.note});
        }
    }
    return result;
}

PdfDocument::ExistingAnnotationInfo PdfView::existingAnnotationAt(int index) const
{
    if (index < 0 || index >= m_existingAnnotations.size()) {
        return {};
    }
    return m_existingAnnotations[index];
}

QString PdfView::existingAnnotationDescription(int index)
{
    if (index < 0 || index >= m_existingAnnotations.size()) {
        return {};
    }
    const PdfDocument::ExistingAnnotationInfo &info = m_existingAnnotations[index];
    if (!info.contents.trimmed().isEmpty()) {
        return snippetOf(info.contents);
    }

    // No author-written comment — for a highlight-style markup, fall back
    // to a snippet of whatever page text the annotation's box covers, same
    // idea as how a DirectPDF-made highlight describes itself.
    QString underlyingText;
    for (const PdfTextWord &word : wordsForPage(info.pageIndex)) {
        if (word.bbox.intersects(info.boundaryPoints)) {
            if (!underlyingText.isEmpty()) {
                underlyingText += QLatin1Char(' ');
            }
            underlyingText += word.text;
        }
    }
    if (!underlyingText.trimmed().isEmpty()) {
        return snippetOf(underlyingText);
    }
    return tr("%1 annotation").arg(info.typeLabel);
}

void PdfView::removeExistingAnnotationAt(int index)
{
    if (index < 0 || index >= m_existingAnnotations.size()) {
        return;
    }
    const PdfDocument::ExistingAnnotationInfo info = m_existingAnnotations[index];
    m_doc.removeExistingAnnotation(info.pageIndex, info.ordinalInPage);
    m_removedAnnotationOrdinals.append({info.pageIndex, info.ordinalInPage});
    m_existingAnnotations.remove(index);
    if (m_selectedExistingAnnotationIndex == index) {
        m_selectedExistingAnnotationIndex = -1;
    } else if (m_selectedExistingAnnotationIndex > index) {
        --m_selectedExistingAnnotationIndex;
    }
    clearImageCache(); // the page's rendered image was baked with this annotation in it
    viewport()->update();
    emit markupsChanged();
    emit existingAnnotationRemoved(info.pageIndex);
}

void PdfView::jumpToExistingAnnotation(int index)
{
    if (index < 0 || index >= m_existingAnnotations.size()) {
        return;
    }
    m_selPage = -1;
    m_selAnchor = {};
    m_selHead = {};
    m_regionPage = -1;
    m_selectedPastedIndex = -1;
    m_selectedTextBoxIndex = -1;
    m_selectedShapeIndex = -1;
    const PdfDocument::ExistingAnnotationInfo &info = m_existingAnnotations[index];
    m_selectedExistingAnnotationIndex = index;
    scrollToShowRect(info.pageIndex, info.boundaryPoints);
    viewport()->update();
    emit textBoxStateChanged();
}

QVector<int> PdfView::removedAnnotationOrdinalsForPage(int sourcePage) const
{
    QVector<int> result;
    for (const QPair<int, int> &removed : m_removedAnnotationOrdinals) {
        if (removed.first == sourcePage) {
            result.append(removed.second);
        }
    }
    return result;
}

PdfView::MarkupSnapshot PdfView::captureMarkupSnapshot() const
{
    MarkupSnapshot snapshot;
    snapshot.highlights = m_highlights;
    snapshot.textBoxes = m_textBoxes;
    snapshot.shapes = m_shapes;
    snapshot.pastedImages = m_pastedImages;
    return snapshot;
}

void PdfView::restoreMarkupSnapshot(const MarkupSnapshot &snapshot)
{
    // Discard (not commit, unlike finishEditingTextBox()) any live edit —
    // the text box being edited is about to be wholesale-replaced anyway,
    // and committing here would emit sessionContentChanged() mid-restore,
    // corrupting the undo stack that's mid-operation right now.
    if (m_editingTextBoxIndex >= 0 && m_textEditWidget) {
        m_textEditWidget->removeEventFilter(this);
        m_textEditWidget->deleteLater();
        m_textEditWidget = nullptr;
        m_editingTextBoxIndex = -1;
    }
    m_highlights = snapshot.highlights;
    m_textBoxes = snapshot.textBoxes;
    m_shapes = snapshot.shapes;
    m_pastedImages = snapshot.pastedImages;

    // Old indices may no longer refer to anything meaningful.
    m_selectedTextBoxIndex = -1;
    m_draggingTextBoxIndex = -1;
    m_resizingTextBoxIndex = -1;
    m_selectedShapeIndex = -1;
    m_draggingShapeIndex = -1;
    m_resizingShapeIndex = -1;
    m_resizingPolygonVertex = -1;
    m_selectedPastedIndex = -1;
    m_draggingPastedIndex = -1;

    viewport()->update();
    emit textBoxStateChanged();
    emit highlightsChanged();
    emit markupsChanged();
}

void PdfView::paintPageMarkups(QPainter &painter, int sourcePage, const QPointF &origin, qreal scale,
                                bool interactive) const
{
    for (const HighlightAnnotation &h : m_highlights) {
        if (h.page != sourcePage) {
            continue;
        }
        const QVector<QRectF> rowRects = selectionRowRects(sourcePage, h.start, h.end);
        painter.save();
        painter.setPen(Qt::NoPen);
        QColor fill = h.color;
        fill.setAlphaF(0.45f);
        painter.setBrush(fill);
        for (const QRectF &rpt : rowRects) {
            const QRectF r(origin.x() + rpt.left() * scale, origin.y() + rpt.top() * scale, rpt.width() * scale,
                            rpt.height() * scale);
            painter.drawRect(r);
        }
        painter.restore();
    }

    // Pasted objects assume natural (unrotated) page coordinates; on
    // screen, hide them while a view rotation is active rather than show
    // them mismatched against the rotated page underneath. Export always
    // wants the plain, unrotated content regardless of the live view's
    // (session-only, never-saved) rotation state.
    if (interactive && m_rotationQuarters != 0) {
        return;
    }

    for (int p = 0; p < m_pastedImages.size(); ++p) {
        const PastedImage &obj = m_pastedImages[p];
        if (obj.page != sourcePage) {
            continue;
        }
        const QRectF r(origin.x() + obj.rectPoints.left() * scale, origin.y() + obj.rectPoints.top() * scale,
                        obj.rectPoints.width() * scale, obj.rectPoints.height() * scale);
        painter.drawImage(r, obj.image);
        if (interactive) {
            painter.save();
            QPen pen(p == m_selectedPastedIndex ? QColor(30, 110, 255) : QColor(0, 0, 0, 90));
            pen.setWidth(p == m_selectedPastedIndex ? 2 : 1);
            if (p == m_selectedPastedIndex) {
                pen.setStyle(Qt::DashLine);
            }
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r);
            painter.restore();
        }
    }

    for (int t = 0; t < m_textBoxes.size(); ++t) {
        const TextBoxObject &obj = m_textBoxes[t];
        if (obj.page != sourcePage) {
            continue;
        }
        const QRectF r(origin.x() + obj.rectPoints.left() * scale, origin.y() + obj.rectPoints.top() * scale,
                        obj.rectPoints.width() * scale, obj.rectPoints.height() * scale);

        if (interactive && t == m_editingTextBoxIndex) {
            // The live QTextEdit widget draws its own fill/border/text;
            // just add a faint outline so an otherwise invisible (no
            // border/fill) box still shows its bounds while being edited.
            painter.save();
            QPen pen(QColor(30, 110, 255, 160));
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r);
            painter.restore();
            drawResizeHandles(painter, r);
            continue;
        }

        painter.save();
        if (obj.fillOpacity > 0.0) {
            QColor fill = obj.fillColor;
            fill.setAlphaF(float(obj.fillOpacity));
            painter.fillRect(r, fill);
        }
        if (obj.borderWidthPts > 0.0) {
            QPen pen(obj.borderColor);
            pen.setWidthF(qMax(1.0, obj.borderWidthPts * scale));
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r);
        }

        // obj.html's font sizes are in device pixels as of whatever scale
        // was active when it was captured (that's always scaleFactor() on
        // screen; export passes scale=1.0, i.e. "device pixel == point");
        // correct for the difference by scaling the paint transform rather
        // than the (zoom-independent, point-space) layout.
        const qreal storedScale = obj.htmlScaleFactor > 0.0 ? obj.htmlScaleFactor : scale;
        const qreal correction = scale / storedScale;
        QTextDocument doc;
        doc.setHtml(obj.html);
        doc.setTextWidth(r.width() / correction);
        painter.save();
        // Box size is fixed (no auto-grow) — clip hard rather than letting
        // overflowing text spill past the box bounds.
        painter.setClipRect(r);
        painter.translate(r.topLeft());
        painter.scale(correction, correction);
        doc.drawContents(&painter, QRectF(0, 0, r.width() / correction, r.height() / correction));
        painter.restore();

        if (interactive && t == m_selectedTextBoxIndex) {
            QPen pen(QColor(30, 110, 255));
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r);
            drawResizeHandles(painter, r);
        }
        painter.restore();
    }

    for (int s = 0; s < m_shapes.size(); ++s) {
        const ShapeObject &obj = m_shapes[s];
        if (obj.page != sourcePage) {
            continue;
        }
        if (isPolygonShape(obj.kind)) {
            QVector<QPointF> screenPoints;
            screenPoints.reserve(obj.polygonPoints.size());
            for (const QPointF &pt : obj.polygonPoints) {
                screenPoints.append(QPointF(origin.x() + pt.x() * scale, origin.y() + pt.y() * scale));
            }
            drawPolygonShape(painter, obj, screenPoints);
            if (interactive && s == m_selectedShapeIndex) {
                drawPolygonVertexHandles(painter, screenPoints);
            }
            continue;
        }
        // topLeft()/bottomRight() give the two real corners for box-like
        // shapes, or the two literal endpoints (in order) for line-like
        // ones — transforming them as plain points works for both without
        // needing to special-case direction.
        const QPointF a = obj.rectPoints.topLeft();
        const QPointF b = obj.rectPoints.bottomRight();
        const QPointF p1(origin.x() + a.x() * scale, origin.y() + a.y() * scale);
        const QPointF p2(origin.x() + b.x() * scale, origin.y() + b.y() * scale);
        drawShape(painter, obj, p1, p2);
        if (interactive && s == m_selectedShapeIndex) {
            painter.save();
            if (isLineLikeShape(obj.kind)) {
                drawEndpointHandles(painter, p1, p2);
            } else {
                QPen pen(QColor(30, 110, 255));
                pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                const QRectF r(p1, p2);
                painter.drawRect(r);
                drawResizeHandles(painter, r);
            }
            painter.restore();
        }
    }

    // The selected existing (file-native) annotation, if any on this page
    // — nothing of ours to actually draw (it's already baked into the
    // page image Poppler rendered), just a selection outline so there's
    // visible feedback for what Delete/Backspace is about to remove.
    if (interactive && m_selectedExistingAnnotationIndex >= 0
        && m_selectedExistingAnnotationIndex < m_existingAnnotations.size()) {
        const PdfDocument::ExistingAnnotationInfo &info = m_existingAnnotations[m_selectedExistingAnnotationIndex];
        if (info.pageIndex == sourcePage) {
            const QRectF &b = info.boundaryPoints;
            const QRectF r(origin.x() + b.left() * scale, origin.y() + b.top() * scale, b.width() * scale,
                           b.height() * scale);
            painter.save();
            QPen pen(QColor(30, 110, 255));
            pen.setWidth(2);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r);
            painter.restore();
        }
    }
}

void PdfView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), QColor(82, 82, 86));

    if (!m_doc.isLoaded()) {
        painter.setPen(Qt::white);
        painter.drawText(viewport()->rect(), Qt::AlignCenter, tr("Open a PDF to get started (File > Open...)"));
        return;
    }

    const int scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();
    const int viewportBottom = scrollY + viewport()->height();
    const qreal scale = scaleFactor();

    for (int i = 0; i < m_layout.size(); ++i) {
        const PageLayout &pl = m_layout[i];
        const qreal bottom = pl.top + pl.height;
        if (bottom < scrollY) {
            continue;
        }
        if (pl.top > viewportBottom) {
            break;
        }

        const int sourcePage = sourceForDisplay(i);
        const qreal left = pageLeft(sourcePage);
        const QRect targetRect(int(std::round(left - scrollX)), int(std::round(pl.top - scrollY)),
                                int(std::round(pl.width)), int(std::round(pl.height)));

        painter.fillRect(targetRect, Qt::white);
        painter.drawImage(targetRect, pageImage(sourcePage));
        painter.setPen(QColor(0, 0, 0, 60));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(targetRect.adjusted(0, 0, -1, -1));

        if (m_bookmarkedPages.contains(sourcePage)) {
            constexpr int kRibbonWidth = 22;
            constexpr int kRibbonHeight = 30;
            const int rx = targetRect.right() - kRibbonWidth - 6;
            const int ry = targetRect.top();
            const QPolygon ribbon({
                QPoint(rx, ry),
                QPoint(rx + kRibbonWidth, ry),
                QPoint(rx + kRibbonWidth, ry + kRibbonHeight),
                QPoint(rx + kRibbonWidth / 2, ry + kRibbonHeight - kRibbonWidth / 2),
                QPoint(rx, ry + kRibbonHeight),
            });
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(150, 100, 0), 1));
            painter.setBrush(QColor(255, 193, 7));
            painter.drawPolygon(ribbon);
            painter.restore();
        }

        if (sourcePage == m_selPage && m_selAnchor.isValid() && m_selHead.isValid()) {
            const TextPos lo = std::min(m_selAnchor, m_selHead);
            const TextPos hi = std::max(m_selAnchor, m_selHead);
            const QVector<QRectF> rowRects = selectionRowRects(sourcePage, lo, hi);
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(60, 120, 255, 90));
            for (const QRectF &rpt : rowRects) {
                const QRectF r(targetRect.left() + rpt.left() * scale, targetRect.top() + rpt.top() * scale,
                                rpt.width() * scale, rpt.height() * scale);
                painter.drawRect(r);
            }
            painter.restore();
        }

        if (sourcePage == m_searchPage && m_searchStart.isValid() && m_searchEnd.isValid()) {
            const QVector<QRectF> rowRects = selectionRowRects(sourcePage, m_searchStart, m_searchEnd);
            painter.save();
            painter.setPen(QPen(QColor(255, 140, 0), 2));
            painter.setBrush(QColor(255, 200, 0, 110));
            for (const QRectF &rpt : rowRects) {
                const QRectF r(targetRect.left() + rpt.left() * scale, targetRect.top() + rpt.top() * scale,
                                rpt.width() * scale, rpt.height() * scale);
                painter.drawRect(r);
            }
            painter.restore();
        }

        if (sourcePage == m_regionPage) {
            const QRectF reg = QRectF(m_regionAnchor, m_regionHead).normalized();
            const QRectF r(targetRect.left() + reg.left() * scale, targetRect.top() + reg.top() * scale,
                            reg.width() * scale, reg.height() * scale);
            painter.save();
            QPen pen(QColor(30, 110, 255));
            pen.setWidth(2);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(QColor(60, 120, 255, 40));
            painter.drawRect(r);
            painter.restore();
        }

        paintPageMarkups(painter, sourcePage, targetRect.topLeft(), scale, /*interactive=*/true);

        // Live preview previews below assume natural (unrotated) page
        // coordinates, same as the placed objects above — hide them while a
        // view rotation is active rather than show them mismatched against
        // the rotated page underneath.
        if (m_rotationQuarters == 0) {
            // Live drag preview while placing a new shape (AddShape mode).
            if (m_placingShape && m_shapePlacingPage == sourcePage) {
                const QPointF p1(targetRect.left() + m_shapeStartPoint.x() * scale,
                                  targetRect.top() + m_shapeStartPoint.y() * scale);
                const QPointF p2(targetRect.left() + m_shapeCurrentPoint.x() * scale,
                                  targetRect.top() + m_shapeCurrentPoint.y() * scale);
                painter.save();
                QPen pen(QColor(30, 110, 255));
                pen.setStyle(Qt::DashLine);
                pen.setWidth(2);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                if (isLineLikeShape(m_pendingShapeKind)) {
                    painter.drawLine(p1, p2);
                    if (m_pendingShapeKind == ShapeKind::Arrow) {
                        drawArrowhead(painter, p2, p1);
                    }
                } else {
                    const QRectF r = QRectF(p1, p2).normalized();
                    switch (m_pendingShapeKind) {
                    case ShapeKind::Rectangle:
                        painter.drawRect(r);
                        break;
                    case ShapeKind::RoundedRectangle: {
                        const qreal radius = qMin(12.0, qMin(r.width(), r.height()) * 0.25);
                        painter.drawRoundedRect(r, radius, radius);
                        break;
                    }
                    case ShapeKind::Ellipse:
                        painter.drawEllipse(r);
                        break;
                    case ShapeKind::Diamond: {
                        const QPointF poly[4] = {
                            QPointF(r.center().x(), r.top()),
                            QPointF(r.right(), r.center().y()),
                            QPointF(r.center().x(), r.bottom()),
                            QPointF(r.left(), r.center().y()),
                        };
                        painter.drawPolygon(poly, 4);
                        break;
                    }
                    case ShapeKind::Line:
                    case ShapeKind::Arrow:
                    case ShapeKind::Polygon:
                        break;
                    }
                }
                painter.restore();
            }

            // Live preview while placing a polygon: the vertices placed so
            // far, connected in order, plus a rubber-band line from the
            // last one to the cursor.
            if (m_placingPolygon && m_polygonPlacingPage == sourcePage && !m_polygonPoints.isEmpty()) {
                QVector<QPointF> screenPoints;
                screenPoints.reserve(m_polygonPoints.size());
                for (const QPointF &pt : m_polygonPoints) {
                    screenPoints.append(
                        QPointF(targetRect.left() + pt.x() * scale, targetRect.top() + pt.y() * scale));
                }
                const QPointF cursorScreen(targetRect.left() + m_polygonCursorPoint.x() * scale,
                                            targetRect.top() + m_polygonCursorPoint.y() * scale);
                painter.save();
                QPen pen(QColor(30, 110, 255));
                pen.setStyle(Qt::DashLine);
                pen.setWidth(2);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                for (int v = 0; v + 1 < screenPoints.size(); ++v) {
                    painter.drawLine(screenPoints[v], screenPoints[v + 1]);
                }
                painter.drawLine(screenPoints.last(), cursorScreen);
                painter.restore();
                drawPolygonVertexHandles(painter, screenPoints);
            }
        }
    }
}

void PdfView::drawResizeHandles(QPainter &painter, const QRectF &screenRect) const
{
    constexpr qreal kHandleSize = 7.0;
    const QPointF points[8] = {
        screenRect.topLeft(),     QPointF(screenRect.center().x(), screenRect.top()),
        screenRect.topRight(),    QPointF(screenRect.right(), screenRect.center().y()),
        screenRect.bottomRight(), QPointF(screenRect.center().x(), screenRect.bottom()),
        screenRect.bottomLeft(),  QPointF(screenRect.left(), screenRect.center().y()),
    };
    painter.save();
    painter.setPen(QPen(QColor(30, 110, 255), 1));
    painter.setBrush(Qt::white);
    for (const QPointF &p : points) {
        painter.drawRect(QRectF(p.x() - kHandleSize / 2, p.y() - kHandleSize / 2, kHandleSize, kHandleSize));
    }
    painter.restore();
}

void PdfView::drawEndpointHandles(QPainter &painter, const QPointF &p1, const QPointF &p2) const
{
    constexpr qreal kHandleSize = 7.0;
    painter.save();
    painter.setPen(QPen(QColor(30, 110, 255), 1));
    painter.setBrush(Qt::white);
    for (const QPointF &p : {p1, p2}) {
        painter.drawRect(QRectF(p.x() - kHandleSize / 2, p.y() - kHandleSize / 2, kHandleSize, kHandleSize));
    }
    painter.restore();
}

void PdfView::drawShape(QPainter &painter, const ShapeObject &shape, const QPointF &p1, const QPointF &p2) const
{
    painter.save();

    if (isLineLikeShape(shape.kind)) {
        QPen pen(shape.borderColor);
        pen.setWidthF(qMax(1.0, shape.borderWidthPts * scaleFactor()));
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(p1, p2);
        if (shape.kind == ShapeKind::Arrow) {
            drawArrowhead(painter, p2, p1);
        }
        painter.restore();
        return;
    }

    const QRectF screenRect(p1, p2);
    if (shape.fillOpacity > 0.0) {
        QColor fill = shape.fillColor;
        fill.setAlphaF(float(shape.fillOpacity));
        painter.setBrush(fill);
    } else {
        painter.setBrush(Qt::NoBrush);
    }
    if (shape.borderWidthPts > 0.0) {
        QPen pen(shape.borderColor);
        pen.setWidthF(qMax(1.0, shape.borderWidthPts * scaleFactor()));
        painter.setPen(pen);
    } else {
        painter.setPen(Qt::NoPen);
    }

    switch (shape.kind) {
    case ShapeKind::Rectangle:
        painter.drawRect(screenRect);
        break;
    case ShapeKind::RoundedRectangle: {
        const qreal radius = qMin(12.0, qMin(screenRect.width(), screenRect.height()) * 0.25);
        painter.drawRoundedRect(screenRect, radius, radius);
        break;
    }
    case ShapeKind::Ellipse:
        painter.drawEllipse(screenRect);
        break;
    case ShapeKind::Diamond: {
        const QPointF poly[4] = {
            QPointF(screenRect.center().x(), screenRect.top()),
            QPointF(screenRect.right(), screenRect.center().y()),
            QPointF(screenRect.center().x(), screenRect.bottom()),
            QPointF(screenRect.left(), screenRect.center().y()),
        };
        painter.drawPolygon(poly, 4);
        break;
    }
    case ShapeKind::Line:
    case ShapeKind::Arrow:
        break; // handled above
    case ShapeKind::Polygon:
        break; // handled by drawPolygonShape() instead — different point model
    }
    painter.restore();
}

void PdfView::drawPolygonVertexHandles(QPainter &painter, const QVector<QPointF> &screenPoints) const
{
    constexpr qreal kHandleSize = 7.0;
    painter.save();
    painter.setPen(QPen(QColor(30, 110, 255), 1));
    painter.setBrush(Qt::white);
    for (const QPointF &p : screenPoints) {
        painter.drawRect(QRectF(p.x() - kHandleSize / 2, p.y() - kHandleSize / 2, kHandleSize, kHandleSize));
    }
    painter.restore();
}

void PdfView::drawPolygonShape(QPainter &painter, const ShapeObject &shape, const QVector<QPointF> &screenPoints) const
{
    if (screenPoints.size() < 2) {
        return;
    }
    painter.save();
    if (shape.fillOpacity > 0.0) {
        QColor fill = shape.fillColor;
        fill.setAlphaF(float(shape.fillOpacity));
        painter.setBrush(fill);
    } else {
        painter.setBrush(Qt::NoBrush);
    }
    if (shape.borderWidthPts > 0.0) {
        QPen pen(shape.borderColor);
        pen.setWidthF(qMax(1.0, shape.borderWidthPts * scaleFactor()));
        painter.setPen(pen);
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.drawPolygon(QPolygonF(screenPoints));
    painter.restore();
}

void PdfView::finishPlacingPolygon(bool commit)
{
    if (!m_placingPolygon) {
        return;
    }
    m_placingPolygon = false;
    if (commit && m_polygonPoints.size() >= 3) {
        ShapeObject obj;
        obj.page = m_polygonPlacingPage;
        obj.kind = ShapeKind::Polygon;
        obj.polygonPoints = m_polygonPoints;
        obj.borderColor = m_defaultShapeBorderColor;
        obj.borderWidthPts = m_defaultShapeBorderWidthPts;
        obj.fillColor = m_defaultShapeFillColor;
        obj.fillOpacity = m_defaultShapeFillOpacity;
        m_shapes.append(obj);
        m_selectedShapeIndex = m_shapes.size() - 1;
        emit markupsChanged();
        emit sessionContentChanged();
    }
    m_polygonPoints.clear();
    m_polygonPlacingPage = -1;

    // One polygon per tool activation, same as the other shapes: revert to
    // normal Text mode immediately rather than staying armed.
    m_mode = SelectionMode::Text;
    viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
    emit selectionModeChanged(m_mode);
    emit textBoxStateChanged();
}

void PdfView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRanges();
    updateTextEditGeometry();
}

void PdfView::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    updateCurrentPageFromScroll();
    updateTextEditGeometry();
    viewport()->update();
}

void PdfView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_doc.isLoaded()) {
        return;
    }
    m_pressPos = event->pos();

    // Coordinate-transform math for click interaction isn't worth doubling
    // up for the rare rotated case; pause selection/drag until rotation is
    // reset to 0 (scrolling/zooming still work fine either way).
    if (m_rotationQuarters != 0) {
        return;
    }

    int page = -1;
    QPointF pt;
    if (!hitTestPage(event->pos(), &page, &pt)) {
        return;
    }

    // A click reaching here landed outside the floating text editor (clicks
    // inside it are consumed by the editor widget itself), so it's a
    // deliberate "click away" — commit whatever was being edited.
    if (m_editingTextBoxIndex >= 0) {
        finishEditingTextBox();
    }

    // Any deliberate click on the page dismisses a search/bookmark
    // highlight left over from jumping here, the same way browsers do.
    if (m_searchPage >= 0) {
        clearSearchHighlight();
    }

    if (m_mode == SelectionMode::AddText) {
        TextBoxObject obj;
        obj.page = page;
        const qreal defaultWidthPts = qMin(200.0, m_doc.pageSizePoints(page).width() - pt.x() - 8.0);
        obj.rectPoints = QRectF(pt.x(), pt.y(), qMax(80.0, defaultWidthPts), 20.0);
        obj.borderColor = m_defaultTextBoxBorderColor;
        obj.borderWidthPts = m_defaultTextBoxBorderWidthPts;
        obj.fillColor = m_defaultTextBoxFillColor;
        obj.fillOpacity = m_defaultTextBoxFillOpacity;
        m_textBoxes.append(obj);
        startEditingTextBox(m_textBoxes.size() - 1);

        // One box per tool activation: drop back to normal mode immediately
        // rather than staying "armed" to place another on the next click.
        // Done directly (not via setSelectionMode(), which would call
        // finishEditingTextBox() and tear down the edit we just started).
        m_mode = SelectionMode::Text;
        viewport()->setCursor(Qt::ArrowCursor);
        emit selectionModeChanged(m_mode);
        return;
    }

    if (m_mode == SelectionMode::AddShape && m_pendingShapeKind == ShapeKind::Polygon) {
        if (!m_placingPolygon) {
            m_placingPolygon = true;
            m_polygonPlacingPage = page;
            m_polygonPoints.clear();
        } else if (page != m_polygonPlacingPage) {
            return; // ignore clicks that land on a different page mid-placement
        }
        m_polygonPoints.append(pt);
        m_polygonCursorPoint = pt;
        viewport()->update();
        return;
    }

    if (m_mode == SelectionMode::AddShape) {
        m_placingShape = true;
        m_shapePlacingPage = page;
        m_shapeStartPoint = pt;
        m_shapeCurrentPoint = pt;
        viewport()->update();
        return;
    }

    if (m_mode == SelectionMode::Text) {
        if (m_selectedTextBoxIndex >= 0 && m_selectedTextBoxIndex < m_textBoxes.size()
            && m_textBoxes[m_selectedTextBoxIndex].page == page) {
            const ResizeHandle handle = resizeHandleAt(m_textBoxes[m_selectedTextBoxIndex].rectPoints, pt);
            if (handle != ResizeHandle::None) {
                m_resizingTextBoxIndex = m_selectedTextBoxIndex;
                m_resizeHandle = handle;
                m_resizeStartRect = m_textBoxes[m_selectedTextBoxIndex].rectPoints;
                m_resizeStartPoint = pt;
                return;
            }
        }
        if (m_selectedShapeIndex >= 0 && m_selectedShapeIndex < m_shapes.size()
            && m_shapes[m_selectedShapeIndex].page == page) {
            const ShapeObject &sel = m_shapes[m_selectedShapeIndex];
            if (isPolygonShape(sel.kind)) {
                const int vertex = polygonVertexAt(sel.polygonPoints, pt);
                if (vertex >= 0) {
                    m_resizingShapeIndex = m_selectedShapeIndex;
                    m_resizingPolygonVertex = vertex;
                    return;
                }
            } else {
                const ResizeHandle handle =
                    isLineLikeShape(sel.kind) ? lineHandleAt(sel.rectPoints, pt) : resizeHandleAt(sel.rectPoints, pt);
                if (handle != ResizeHandle::None) {
                    m_resizingShapeIndex = m_selectedShapeIndex;
                    m_shapeResizeHandle = handle;
                    m_shapeResizeStartRect = m_shapes[m_selectedShapeIndex].rectPoints;
                    m_shapeResizeStartPoint = pt;
                    return;
                }
            }
        }

        const int hitText = textBoxAt(page, pt);
        if (hitText >= 0) {
            m_selPage = -1;
            m_selAnchor = {};
            m_selHead = {};
            m_regionPage = -1;
            m_selectedPastedIndex = -1;
            m_selectedShapeIndex = -1;
            m_selectedExistingAnnotationIndex = -1;
            m_selectedTextBoxIndex = hitText;
            m_draggingTextBoxIndex = hitText;
            m_textDragGrabOffset = pt - m_textBoxes[hitText].rectPoints.topLeft();
            viewport()->update();
            emit textBoxStateChanged();
            return;
        }

        const int hitShape = shapeAt(page, pt);
        if (hitShape >= 0) {
            m_selPage = -1;
            m_selAnchor = {};
            m_selHead = {};
            m_regionPage = -1;
            m_selectedPastedIndex = -1;
            m_selectedTextBoxIndex = -1;
            m_selectedExistingAnnotationIndex = -1;
            m_selectedShapeIndex = hitShape;
            if (isPolygonShape(m_shapes[hitShape].kind)) {
                m_draggingShapeIndex = hitShape;
                m_polygonDragStartPoints = m_shapes[hitShape].polygonPoints;
                m_polygonDragStartMouse = pt;
            } else {
                m_draggingShapeIndex = hitShape;
                m_shapeDragGrabOffset = pt - m_shapes[hitShape].rectPoints.topLeft();
            }
            viewport()->update();
            emit textBoxStateChanged();
            return;
        }

        const int hitPasted = pastedImageAt(page, pt);
        if (hitPasted >= 0) {
            m_selPage = -1;
            m_selAnchor = {};
            m_selHead = {};
            m_regionPage = -1;
            m_selectedTextBoxIndex = -1;
            m_selectedShapeIndex = -1;
            m_selectedExistingAnnotationIndex = -1;
            m_selectedPastedIndex = hitPasted;
            m_draggingPastedIndex = hitPasted;
            m_dragGrabOffset = pt - m_pastedImages[hitPasted].rectPoints.topLeft();
            viewport()->update();
            return;
        }

        const int hitExisting = existingAnnotationAt(page, pt);
        if (hitExisting >= 0) {
            m_selPage = -1;
            m_selAnchor = {};
            m_selHead = {};
            m_regionPage = -1;
            m_selectedTextBoxIndex = -1;
            m_selectedShapeIndex = -1;
            m_selectedPastedIndex = -1;
            m_selectedExistingAnnotationIndex = hitExisting;
            viewport()->update();
            emit textBoxStateChanged();
            return;
        }
    }
    if (m_selectedPastedIndex >= 0 || m_selectedTextBoxIndex >= 0 || m_selectedShapeIndex >= 0
        || m_selectedExistingAnnotationIndex >= 0) {
        emit textBoxStateChanged();
    }
    m_selectedPastedIndex = -1;
    m_selectedTextBoxIndex = -1;
    m_selectedShapeIndex = -1;
    m_selectedExistingAnnotationIndex = -1;

    if (m_mode == SelectionMode::Region) {
        m_selPage = -1;
        m_selAnchor = {};
        m_selHead = {};
        m_regionSelecting = true;
        m_regionPage = page;
        m_regionAnchor = pt;
        m_regionHead = pt;
        viewport()->update();
        return;
    }

    // In normal text mode, clicking directly on an embedded image selects
    // that image as a whole (for copy) instead of starting a text drag.
    if (tryStartImageSelection(page, pt)) {
        return;
    }
    m_regionPage = -1;

    wordsForPage(page); // ensure cached before nearestCharPos (const) runs
    m_selecting = true;
    m_selPage = page;
    m_selAnchor = nearestCharPos(page, pt);
    m_selHead = m_selAnchor;
    viewport()->update();
}

void PdfView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingPastedIndex >= 0) {
        const QPointF pt = projectToPage(m_pastedImages[m_draggingPastedIndex].page, event->pos());
        PastedImage &obj = m_pastedImages[m_draggingPastedIndex];
        obj.rectPoints.moveTopLeft(pt - m_dragGrabOffset);
        viewport()->update();
        return;
    }

    if (m_draggingTextBoxIndex >= 0) {
        const QPointF pt = projectToPage(m_textBoxes[m_draggingTextBoxIndex].page, event->pos());
        TextBoxObject &obj = m_textBoxes[m_draggingTextBoxIndex];
        obj.rectPoints.moveTopLeft(pt - m_textDragGrabOffset);
        viewport()->update();
        return;
    }

    if (m_draggingShapeIndex >= 0) {
        ShapeObject &obj = m_shapes[m_draggingShapeIndex];
        const QPointF pt = projectToPage(obj.page, event->pos());
        if (isPolygonShape(obj.kind)) {
            const QPointF delta = pt - m_polygonDragStartMouse;
            for (int v = 0; v < obj.polygonPoints.size(); ++v) {
                obj.polygonPoints[v] = m_polygonDragStartPoints[v] + delta;
            }
        } else {
            obj.rectPoints.moveTopLeft(pt - m_shapeDragGrabOffset);
        }
        viewport()->update();
        return;
    }

    if (m_placingShape && m_shapePlacingPage >= 0) {
        m_shapeCurrentPoint = projectToPage(m_shapePlacingPage, event->pos());
        viewport()->update();
        return;
    }

    if (m_placingPolygon && m_polygonPlacingPage >= 0) {
        m_polygonCursorPoint = projectToPage(m_polygonPlacingPage, event->pos());
        viewport()->update();
        return;
    }

    if (m_resizingShapeIndex >= 0 && m_resizingPolygonVertex >= 0 && m_resizingShapeIndex < m_shapes.size()) {
        ShapeObject &obj = m_shapes[m_resizingShapeIndex];
        if (m_resizingPolygonVertex < obj.polygonPoints.size()) {
            obj.polygonPoints[m_resizingPolygonVertex] = projectToPage(obj.page, event->pos());
            viewport()->update();
        }
        return;
    }

    if (m_resizingTextBoxIndex >= 0 && m_resizingTextBoxIndex < m_textBoxes.size()) {
        TextBoxObject &obj = m_textBoxes[m_resizingTextBoxIndex];
        const QPointF pt = projectToPage(obj.page, event->pos());
        const qreal dx = pt.x() - m_resizeStartPoint.x();
        const qreal dy = pt.y() - m_resizeStartPoint.y();
        constexpr qreal kMinSize = 24.0;
        QRectF r = m_resizeStartRect;

        const bool left = m_resizeHandle == ResizeHandle::TopLeft || m_resizeHandle == ResizeHandle::Left
            || m_resizeHandle == ResizeHandle::BottomLeft;
        const bool right = m_resizeHandle == ResizeHandle::TopRight || m_resizeHandle == ResizeHandle::Right
            || m_resizeHandle == ResizeHandle::BottomRight;
        const bool top = m_resizeHandle == ResizeHandle::TopLeft || m_resizeHandle == ResizeHandle::Top
            || m_resizeHandle == ResizeHandle::TopRight;
        const bool bottom = m_resizeHandle == ResizeHandle::BottomLeft || m_resizeHandle == ResizeHandle::Bottom
            || m_resizeHandle == ResizeHandle::BottomRight;

        if (left) {
            const qreal newLeft = qMin(m_resizeStartRect.left() + dx, m_resizeStartRect.right() - kMinSize);
            r.setLeft(newLeft);
        } else if (right) {
            const qreal newRight = qMax(m_resizeStartRect.right() + dx, m_resizeStartRect.left() + kMinSize);
            r.setRight(newRight);
        }
        if (top) {
            const qreal newTop = qMin(m_resizeStartRect.top() + dy, m_resizeStartRect.bottom() - kMinSize);
            r.setTop(newTop);
        } else if (bottom) {
            const qreal newBottom = qMax(m_resizeStartRect.bottom() + dy, m_resizeStartRect.top() + kMinSize);
            r.setBottom(newBottom);
        }

        obj.rectPoints = r;
        if (m_editingTextBoxIndex == m_resizingTextBoxIndex) {
            updateTextEditGeometry();
        }
        viewport()->update();
        return;
    }

    if (m_resizingShapeIndex >= 0 && m_resizingShapeIndex < m_shapes.size()) {
        ShapeObject &obj = m_shapes[m_resizingShapeIndex];
        const QPointF pt = projectToPage(obj.page, event->pos());

        if (isLineLikeShape(obj.kind)) {
            // Just the one endpoint moves — no min-size clamp needed (a
            // short line/arrow is perfectly valid, unlike a degenerate box).
            if (m_shapeResizeHandle == ResizeHandle::TopLeft) {
                obj.rectPoints.setTopLeft(pt);
            } else if (m_shapeResizeHandle == ResizeHandle::BottomRight) {
                obj.rectPoints.setBottomRight(pt);
            }
            viewport()->update();
            return;
        }

        const qreal dx = pt.x() - m_shapeResizeStartPoint.x();
        const qreal dy = pt.y() - m_shapeResizeStartPoint.y();
        constexpr qreal kMinSize = 12.0;
        QRectF r = m_shapeResizeStartRect;

        const bool left = m_shapeResizeHandle == ResizeHandle::TopLeft || m_shapeResizeHandle == ResizeHandle::Left
            || m_shapeResizeHandle == ResizeHandle::BottomLeft;
        const bool right = m_shapeResizeHandle == ResizeHandle::TopRight || m_shapeResizeHandle == ResizeHandle::Right
            || m_shapeResizeHandle == ResizeHandle::BottomRight;
        const bool top = m_shapeResizeHandle == ResizeHandle::TopLeft || m_shapeResizeHandle == ResizeHandle::Top
            || m_shapeResizeHandle == ResizeHandle::TopRight;
        const bool bottom = m_shapeResizeHandle == ResizeHandle::BottomLeft
            || m_shapeResizeHandle == ResizeHandle::Bottom || m_shapeResizeHandle == ResizeHandle::BottomRight;

        if (left) {
            const qreal newLeft = qMin(m_shapeResizeStartRect.left() + dx, m_shapeResizeStartRect.right() - kMinSize);
            r.setLeft(newLeft);
        } else if (right) {
            const qreal newRight = qMax(m_shapeResizeStartRect.right() + dx, m_shapeResizeStartRect.left() + kMinSize);
            r.setRight(newRight);
        }
        if (top) {
            const qreal newTop = qMin(m_shapeResizeStartRect.top() + dy, m_shapeResizeStartRect.bottom() - kMinSize);
            r.setTop(newTop);
        } else if (bottom) {
            const qreal newBottom = qMax(m_shapeResizeStartRect.bottom() + dy, m_shapeResizeStartRect.top() + kMinSize);
            r.setBottom(newBottom);
        }

        obj.rectPoints = r;
        viewport()->update();
        return;
    }

    if (m_mode == SelectionMode::Region) {
        if (!m_regionSelecting || m_regionPage < 0) {
            return;
        }
        m_regionHead = projectToPage(m_regionPage, event->pos());
        viewport()->update();
        return;
    }

    // Hover feedback: show a resize cursor near the selected box's/shape's edges.
    if (m_mode == SelectionMode::Text && m_selectedTextBoxIndex >= 0 && m_selectedTextBoxIndex < m_textBoxes.size()) {
        const TextBoxObject &sel = m_textBoxes[m_selectedTextBoxIndex];
        int page = -1;
        QPointF pt;
        if (hitTestPage(event->pos(), &page, &pt) && page == sel.page) {
            viewport()->setCursor(cursorForHandle(resizeHandleAt(sel.rectPoints, pt)));
        }
    }
    if (m_mode == SelectionMode::Text && m_selectedShapeIndex >= 0 && m_selectedShapeIndex < m_shapes.size()) {
        const ShapeObject &sel = m_shapes[m_selectedShapeIndex];
        int page = -1;
        QPointF pt;
        if (hitTestPage(event->pos(), &page, &pt) && page == sel.page) {
            if (isPolygonShape(sel.kind)) {
                const bool onVertex = polygonVertexAt(sel.polygonPoints, pt) >= 0;
                viewport()->setCursor(onVertex ? Qt::SizeAllCursor : Qt::ArrowCursor);
            } else {
                const ResizeHandle handle =
                    isLineLikeShape(sel.kind) ? lineHandleAt(sel.rectPoints, pt) : resizeHandleAt(sel.rectPoints, pt);
                viewport()->setCursor(cursorForHandle(handle));
            }
        }
    }

    if (!m_selecting || m_selPage < 0) {
        return;
    }
    const QPointF pt = projectToPage(m_selPage, event->pos());
    const TextPos pos = nearestCharPos(m_selPage, pt);
    if (!(pos == m_selHead)) {
        m_selHead = pos;
        viewport()->update();
    }
}

void PdfView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (m_draggingPastedIndex >= 0) {
        m_draggingPastedIndex = -1;
        return;
    }

    if (m_draggingTextBoxIndex >= 0) {
        m_draggingTextBoxIndex = -1;
        return;
    }

    if (m_resizingTextBoxIndex >= 0) {
        m_resizingTextBoxIndex = -1;
        m_resizeHandle = ResizeHandle::None;
        return;
    }

    if (m_draggingShapeIndex >= 0) {
        m_draggingShapeIndex = -1;
        return;
    }

    if (m_resizingShapeIndex >= 0) {
        m_resizingShapeIndex = -1;
        m_shapeResizeHandle = ResizeHandle::None;
        m_resizingPolygonVertex = -1;
        return;
    }

    if (m_placingShape) {
        m_placingShape = false;
        constexpr qreal kMinSize = 12.0;
        QRectF r;
        if (isLineLikeShape(m_pendingShapeKind)) {
            // Never normalized — direction (which end is the start vs. the
            // arrow's head) has to survive intact.
            r = QRectF(m_shapeStartPoint, m_shapeCurrentPoint);
            if (QLineF(m_shapeStartPoint, m_shapeCurrentPoint).length() < kMinSize) {
                // Too short to be a deliberate drag (e.g. a plain click) —
                // place a sensible default-length line anchored at the
                // click point instead of a near-invisible sliver.
                r = QRectF(m_shapeStartPoint, m_shapeStartPoint + QPointF(100.0, 0.0));
            }
        } else {
            r = QRectF(m_shapeStartPoint, m_shapeCurrentPoint).normalized();
            if (r.width() < kMinSize || r.height() < kMinSize) {
                r = QRectF(m_shapeStartPoint, QSizeF(100.0, 70.0));
            }
        }
        ShapeObject obj;
        obj.page = m_shapePlacingPage;
        obj.kind = m_pendingShapeKind;
        obj.rectPoints = r;
        obj.borderColor = m_defaultShapeBorderColor;
        obj.borderWidthPts = m_defaultShapeBorderWidthPts;
        obj.fillColor = m_defaultShapeFillColor;
        obj.fillOpacity = m_defaultShapeFillOpacity;
        m_shapes.append(obj);
        m_selectedShapeIndex = m_shapes.size() - 1;
        m_shapePlacingPage = -1;
        emit markupsChanged();
        emit sessionContentChanged();

        // One shape per tool activation, same as Add Text: revert to normal
        // Text mode immediately rather than staying armed.
        m_mode = SelectionMode::Text;
        viewport()->setCursor(Qt::ArrowCursor);
        viewport()->update();
        emit selectionModeChanged(m_mode);
        emit textBoxStateChanged();
        return;
    }

    if (m_mode == SelectionMode::Region) {
        m_regionSelecting = false;
        const int dragDistance = (event->pos() - m_pressPos).manhattanLength();
        if (dragDistance < 3) {
            m_regionPage = -1;
        }
        viewport()->update();
        return;
    }

    m_selecting = false;

    const int dragDistance = (event->pos() - m_pressPos).manhattanLength();
    if (dragDistance < 3) {
        // Plain click, no drag: clear the selection rather than leaving a
        // single character highlighted.
        m_selPage = -1;
        m_selAnchor = {};
        m_selHead = {};
        viewport()->update();
    }
}

void PdfView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_doc.isLoaded() || m_rotationQuarters != 0) {
        return;
    }
    if (m_placingPolygon) {
        // The second click of this double-click already added a vertex via
        // mousePressEvent (Qt delivers press before doubleClick) — just
        // close the shape using the points placed so far, without adding
        // another one.
        finishPlacingPolygon(true);
        return;
    }
    if (m_mode != SelectionMode::Text) {
        return;
    }
    int page = -1;
    QPointF pt;
    if (!hitTestPage(event->pos(), &page, &pt)) {
        return;
    }
    const int hitText = textBoxAt(page, pt);
    if (hitText >= 0) {
        startEditingTextBox(hitText);
    }
}

bool PdfView::eventFilter(QObject *watched, QEvent *event)
{
    // Deliberately not tied to FocusOut: clicking a formatting control in
    // the Format panel moves focus away from the editor too, and that must
    // NOT end the edit (the whole point is to format the text you're
    // editing without losing your place). Ending an edit is only triggered
    // explicitly — clicking elsewhere on the page (mousePressEvent) or Esc.
    if (watched == m_textEditWidget && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            finishEditingTextBox();
            return true;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

bool PdfView::event(QEvent *event)
{
    // Trackpad pinch-to-zoom arrives as a native gesture, not a wheel
    // event — Ctrl+scroll (wheelEvent below) is a separate, deliberate
    // zoom gesture that pinch doesn't go through at all.
    if (event->type() == QEvent::NativeGesture) {
        auto *gestureEvent = static_cast<QNativeGestureEvent *>(event);
        if (gestureEvent->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(zoom() * (1.0 + gestureEvent->value()));
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void PdfView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() > 0) {
            zoomIn();
        } else if (event->angleDelta().y() < 0) {
            zoomOut();
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PdfView::keyPressEvent(QKeyEvent *event)
{
    if (m_placingPolygon) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishPlacingPolygon(true);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            finishPlacingPolygon(false);
            event->accept();
            return;
        }
    }
    if (event->matches(QKeySequence::Copy)) {
        copySelectionToClipboard();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAllOnPage();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        pasteImageFromClipboard();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_selectedPastedIndex >= 0 || m_selectedTextBoxIndex >= 0 || m_selectedShapeIndex >= 0
            || m_selectedExistingAnnotationIndex >= 0) {
            deleteSelectedObject();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && m_searchPage >= 0) {
        clearSearchHighlight();
        event->accept();
        return;
    }

    const int step = viewport()->height();
    switch (event->key()) {
    case Qt::Key_PageDown:
        verticalScrollBar()->setValue(verticalScrollBar()->value() + step);
        return;
    case Qt::Key_PageUp:
        verticalScrollBar()->setValue(verticalScrollBar()->value() - step);
        return;
    case Qt::Key_Home:
        if (event->modifiers() & Qt::ControlModifier) {
            goToDisplayPosition(0);
            return;
        }
        break;
    case Qt::Key_End:
        if (event->modifiers() & Qt::ControlModifier) {
            goToDisplayPosition(displayCount() - 1);
            return;
        }
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PdfView::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_doc.isLoaded()) {
        return;
    }
    emit contextMenuRequestedAt(event->globalPos());
}
