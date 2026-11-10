#pragma once

#include <QHash>
#include <QRectF>
#include <QString>
#include <QVector>

#include <functional>

class QPainter;

// Writes a (possibly modified) copy of a PDF to disk using qpdf. This is the
// app's only PDF-writing path — everything else (PdfDocument/PdfView) is
// read-only via Poppler.
class PdfWriter {
public:
    struct Bookmark {
        QString title;
        int pageIndexZeroBased = 0; // in terms of the *display* order being saved
    };

    // Identifies an existing outline (bookmark) entry by its path of child
    // indices from the root (e.g. [0] = first top-level entry, [0,1] =
    // second child of the first top-level entry), matching the order
    // PdfDocument::outline() walks the tree in.
    struct OutlineRename {
        QVector<int> path;
        QString newTitle;
    };

    // Splices pages copied from a different PDF file into the saved output
    // — "Insert Pages from File". Positioned in terms of the *final*
    // (post-reorder) display arrangement: afterDisplayPosition = -1 means
    // "insert at the very beginning", otherwise the inserted pages land
    // immediately after that display position. pageIndices are 0-based
    // indices into sourcePath, in the order they should appear. Inserted
    // pages have no "original index" of their own (they didn't exist in
    // the document being saved), so they're never candidates for
    // pageHasMarkups/paintMarkups or bookmark targets, and can't be
    // deleted/duplicated/reordered again until after this Save completes
    // and they become normal pages of the saved file.
    struct ExternalInsertion {
        int afterDisplayPosition = -1;
        QString sourcePath;
        QVector<int> pageIndices;
    };

    // Where to anchor a real PDF sticky-note (/Text) annotation for a
    // markup's note, and its text. rectPoints is in the same natural
    // (unrotated, origin top-left) page point space as everything else —
    // the note icon is placed at its top-right corner.
    struct NoteAnchor {
        QRectF rectPoints;
        QString text;
    };

    // Loads sourcePath, applies the requested changes, and writes the result
    // to destPath (which may be the same path as sourcePath).
    //   newTitle/newAuthor: pass an empty string to leave that field as-is.
    //   pageOrderZeroBased: pass an empty vector to keep the original pages
    //     as-is; otherwise, each entry is an original page index, in the
    //     desired final (display) order. An original index can be omitted
    //     (that page is deleted) or repeated (that page is duplicated).
    //   newBookmarks: appended to the document's existing outline, pointing
    //     at the top of the given page (post-reorder, display index).
    //   renames: retitles existing outline entries in place.
    //   rotationQuarterTurns: added (not replaced — stacks with whatever
    //     /Rotate each page already had) to every final page's rotation, 0
    //     meaning no change. This is DirectPDF's single whole-document
    //     view rotation, applied uniformly to every page.
    //   pageHasMarkups/paintMarkups: flattens highlights, text boxes,
    //     shapes, and pasted images into the saved PDF, since none of those
    //     have any other save path (they only ever existed in PdfView's
    //     in-memory session state before this). For each *original* page
    //     index (same numbering as pageOrderZeroBased, independent of any
    //     reordering), pageHasMarkups(page) is checked first; if it returns
    //     true, paintMarkups(page, painter) is called to draw that page's
    //     markups onto a page-sized painter (1:1 point scale, origin (0,0)
    //     = page's top-left corner), which is then composited onto every
    //     copy of that page in the saved file (more than one, if the page
    //     was duplicated) as a flattened overlay. Pass empty std::functions
    //     if there's nothing to flatten.
    //   notesForPage: for each *original* page index, returns the note
    //     anchors to write as real /Text annotations (so the notes
    //     themselves — unlike the visual content above — survive as
    //     actual PDF comments, readable/editable in other PDF viewers too).
    //     Pass an empty std::function if there are no notes.
    //   pageLabels: free-text page labels (DirectPDF's Pages-panel Label
    //     field), written as single-page entries in the PDF's standard
    //     /PageLabels tree (prefix-only, no numbering style) — a real,
    //     standard PDF mechanism other viewers' page-label displays will
    //     also show, not just DirectPDF. Keyed by *original* page index.
    //   removedAnnotationOrdinalsForPage: for each *original* page index,
    //     returns the ordinal positions (within that page's own, pre-edit
    //     /Annots array — see PdfDocument::ExistingAnnotationInfo) of any
    //     existing, file-native annotations the user removed this session
    //     (e.g. a highlight made in Preview) — erased from /Annots in the
    //     saved output. Pass an empty std::function if none were removed.
    struct PageLabel {
        int sourcePageIndex = 0;
        QString text;
    };
    static bool save(const QString &sourcePath, const QString &destPath, const QString &newTitle,
                      const QString &newAuthor, const QVector<int> &pageOrderZeroBased,
                      const QVector<Bookmark> &newBookmarks, const QVector<OutlineRename> &renames,
                      int rotationQuarterTurns, const QVector<ExternalInsertion> &insertions,
                      const QVector<PageLabel> &pageLabels, const std::function<bool(int)> &pageHasMarkups,
                      const std::function<void(int, QPainter &)> &paintMarkups,
                      const std::function<QVector<NoteAnchor>(int)> &notesForPage,
                      const std::function<QVector<int>(int)> &removedAnnotationOrdinalsForPage, QString *errorMessage);

    // Reads back labels written by save() above: a /PageLabels /Nums entry
    // counts as a DirectPDF-style label only if it sets /P (prefix text)
    // and does NOT set /S (numbering style) — that combination means
    // "show just this text, no page number," which is unambiguous and is
    // exactly (and only) what save() ever writes for a labeled page. A
    // range that also sets /S (roman numerals, decimal restart, etc.) is
    // left alone, since that's ordinary page numbering, not a DirectPDF
    // label. Returns an empty map (not an error) if the file can't be
    // opened or has no /PageLabels at all.
    static QHash<int, QString> readPageLabels(const QString &path);
};
