#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <memory>

namespace Poppler {
class Document;
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

private:
    std::unique_ptr<Poppler::Document> m_doc;
    QString m_filePath;
    mutable std::unique_ptr<PDFDoc> m_coreDoc;
};
