#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <memory>
#include <vector>

namespace Poppler {
class Document;
class Annotation;
}
class PDFDoc; // poppler core, used only for image geometry (see imageBoxes())

// A word (or short run) of PDF text with its bounding box in PDF point
// space (origin top-left, 1 unit = 1/72 inch), as extracted from the page's
// native text layer. Used for selection, highlighting, and future search.
struct PdfTextWord {
    QString text;
    QRectF bbox;
    bool spaceAfter = false;
    // Per-character bounding boxes, charBoxes[i] for text[i]. Lets selection
    // work at character granularity (like every other app) instead of
    // snapping to whole words.
    QVector<QRectF> charBoxes;
};

// Thin wrapper around the underlying PDF engine (currently Poppler-Qt6).
// Keeping engine calls behind this class means the rest of the app only
// ever talks to PdfDocument/PdfTextWord, so the engine can be swapped later
// without touching UI code.
class PdfDocument {
public:
    PdfDocument();
    ~PdfDocument();

    bool load(const QString &filePath, QString *errorMessage = nullptr);
    bool isLoaded() const;

    int pageCount() const;
    QString filePath() const { return m_filePath; }

    // Page size in PDF points (1/72 inch), independent of zoom.
    QSizeF pageSizePoints(int pageIndex) const;

    // Rasterizes a page at the given dots-per-inch. rotationQuarterTurns is
    // 0-3, the number of 90-degree clockwise turns applied for viewing only
    // (not written back to the document).
    QImage renderPage(int pageIndex, qreal dpi, int rotationQuarterTurns = 0) const;

    // Rasterizes just a sub-region of a page (in PDF points) at the given
    // dots-per-inch. Used for the image/region "snip" selection tool.
    QImage renderPageRegion(int pageIndex, qreal dpi, const QRectF &regionPoints) const;

    // Extracts per-word text boxes for selection/copy, in point space.
    QVector<PdfTextWord> extractWords(int pageIndex) const;

    // Bounding boxes (PDF points, top-left origin) of embedded raster images
    // on a page, for click-to-select-image. Walks the page's content stream
    // via the poppler core API, since the Qt bindings don't expose per-image
    // geometry directly.
    QVector<QRectF> imageBoxes(int pageIndex) const;

    struct DocumentInfo {
        QString title;
        QString author;
        QString subject;
        QString creator;
        QString producer;
        int pageCount = 0;
        int versionMajor = 0;
        int versionMinor = 0;
    };
    DocumentInfo documentInfo() const;

    struct OutlineEntry {
        QString title;
        int pageIndex = -1; // 0-indexed; -1 if this entry has no resolvable destination
        QVector<OutlineEntry> children;
    };
    QVector<OutlineEntry> outline() const;

    // A markup-type annotation (highlight, underline, note, shape, ink,
    // etc.) already present in the file — from Preview, Acrobat, or any
    // other PDF tool, as opposed to something placed in this DirectPDF
    // session.
    struct ExistingAnnotationInfo {
        int pageIndex = -1;
        // Position within this page's raw /Annots array (counting every
        // annotation, not just markup-type ones) — a stable identifier
        // PdfWriter::save() uses to delete the matching entry from the
        // file's own /Annots array.
        int ordinalInPage = -1;
        QString typeLabel;     // "Highlight", "Underline", "Note", "Shape", "Freehand", ...
        QRectF boundaryPoints; // PDF point space, top-left origin — same convention as PdfTextWord::bbox
        QString contents;      // the annotation's own /Contents text (e.g. a sticky-note comment), often empty
    };
    // Enumerates every markup-type annotation across the whole file (skips
    // links, form widgets, and embedded-media annotations — not something
    // a user would think of as "a markup"). Recomputed fresh on every
    // call — cheap, since it's just metadata, no rendering — so this is
    // deliberately NOT cached here; callers that want to hang on to the
    // result (PdfView does, for the Markups panel) cache it themselves.
    QVector<ExistingAnnotationInfo> existingAnnotations() const;
    // Removes the annotation at ordinalInPage (see ExistingAnnotationInfo)
    // from the live, in-memory document — renderPage() stops drawing it
    // immediately. This does NOT touch the file on disk; PdfWriter::save()
    // is separately responsible for removing the matching entry from the
    // file's own /Annots array. Returns false if the position is invalid.
    bool removeExistingAnnotation(int pageIndex, int ordinalInPage) const;

private:
    std::unique_ptr<Poppler::Document> m_doc;
    QString m_filePath;
    mutable std::unique_ptr<PDFDoc> m_coreDoc;
};
