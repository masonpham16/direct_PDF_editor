#include "PdfWriter.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFOutlineObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QFile>
#include <QFileInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>

#include <algorithm>

namespace {

// Renders one page's flattened markups (via paintMarkups) to a standalone
// single-page PDF sized to exactly match the destination page's visual
// (post-/Rotate) dimensions, then composites that page onto destPage as a
// Form XObject using qpdf's overlay machinery — invert_transformations
// handles making the stamp line up correctly even if destPage itself has a
// pre-existing /Rotate, since the stamp itself is always drawn upright.
bool stampPageMarkups(QPDF &qpdf, QPDFObjectHandle destPage, int sourcePage, const QString &scratchDir,
                       const std::function<void(int, QPainter &)> &paintMarkups, QString *errorMessage)
{
    QPDFPageObjectHelper destHelper(destPage);

    QPDFObjectHandle mediaBox = destHelper.getMediaBox();
    const double llx = mediaBox.getArrayItem(0).getNumericValue();
    const double lly = mediaBox.getArrayItem(1).getNumericValue();
    const double urx = mediaBox.getArrayItem(2).getNumericValue();
    const double ury = mediaBox.getArrayItem(3).getNumericValue();
    double w = urx - llx;
    double h = ury - lly;

    QPDFObjectHandle rotateObj = destHelper.getAttribute("/Rotate", false);
    int rotate = rotateObj.isInteger() ? rotateObj.getIntValueAsInt() : 0;
    rotate = ((rotate % 360) + 360) % 360;
    if (rotate == 90 || rotate == 270) {
        std::swap(w, h);
    }
    if (w <= 0 || h <= 0) {
        return true; // degenerate page geometry — nothing sane to stamp
    }

    const QString stampPath = scratchDir + QStringLiteral("/stamp-%1.pdf").arg(sourcePage);
    {
        QPdfWriter writer(stampPath);
        writer.setResolution(72); // so 1 painter unit == 1 point, matching PdfView's own point-space convention
        writer.setPageSize(QPageSize(QSizeF(w, h), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
        QPainter painter(&writer);
        paintMarkups(sourcePage, painter);
    }

    QPDF stampQpdf;
    stampQpdf.setImmediateCopyFrom(true); // let the temp file be removed right after this call
    try {
        stampQpdf.processFile(stampPath.toUtf8().constData());
    } catch (const std::exception &e) {
        QFile::remove(stampPath);
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to flatten markups for page %1: %2")
                                .arg(sourcePage + 1)
                                .arg(QString::fromStdString(e.what()));
        }
        return false;
    }

    QPDFPageDocumentHelper stampPdh(stampQpdf);
    std::vector<QPDFPageObjectHelper> stampPages = stampPdh.getAllPages();
    if (stampPages.empty()) {
        QFile::remove(stampPath);
        return true;
    }

    QPDFObjectHandle fo = stampPages[0].getFormXObjectForPage();
    QPDFObjectHandle foreignFo = qpdf.copyForeignObject(fo);
    QFile::remove(stampPath);

    QPDFObjectHandle resources = destHelper.getAttribute("/Resources", true);
    if (!resources.isDictionary()) {
        resources = QPDFObjectHandle::newDictionary();
        destPage.replaceKey("/Resources", resources);
    }
    int suffix = 1;
    const std::string name = resources.getUniqueResourceName("/DirectPdfStamp", suffix);

    const QPDFObjectHandle::Rectangle rect(0, 0, w, h);
    const std::string content =
        destHelper.placeFormXObject(foreignFo, name, rect, /*invert_transformations=*/true,
                                     /*allow_shrink=*/false, /*allow_expand=*/false);

    QPDFObjectHandle xobjectDict = resources.getKey("/XObject");
    if (!xobjectDict.isDictionary()) {
        xobjectDict = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/XObject", xobjectDict);
    }
    xobjectDict.replaceKey(name, foreignFo);

    destHelper.addPageContents(QPDFObjectHandle::newStream(&qpdf, "q\n" + content + "\nQ\n"), false);
    return true;
}

// Adds a real PDF sticky-note (/Text) annotation to destPage, anchored at
// the top-right corner of note.rectPoints. pageHeightPts converts from
// natural (unrotated, origin top-left) point space to PDF annotation space
// (origin bottom-left) — same convention as the rest of this file.
void addNoteAnnotation(QPDF &qpdf, QPDFObjectHandle destPage, double pageHeightPts, const PdfWriter::NoteAnchor &note)
{
    constexpr double kIconSize = 18.0;
    const double x = note.rectPoints.right();
    const double yPdf = pageHeightPts - note.rectPoints.top();

    QPDFObjectHandle rect = QPDFObjectHandle::newArray();
    rect.appendItem(QPDFObjectHandle::newReal(x, 2));
    rect.appendItem(QPDFObjectHandle::newReal(yPdf - kIconSize, 2));
    rect.appendItem(QPDFObjectHandle::newReal(x + kIconSize, 2));
    rect.appendItem(QPDFObjectHandle::newReal(yPdf, 2));

    QPDFObjectHandle annot = qpdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    annot.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
    annot.replaceKey("/Subtype", QPDFObjectHandle::newName("/Text"));
    annot.replaceKey("/Rect", rect);
    annot.replaceKey("/Contents", QPDFObjectHandle::newUnicodeString(note.text.toStdString()));
    annot.replaceKey("/Name", QPDFObjectHandle::newName("/Comment"));
    annot.replaceKey("/Open", QPDFObjectHandle::newBool(false));

    QPDFObjectHandle annots = destPage.getKey("/Annots");
    if (!annots.isArray()) {
        annots = QPDFObjectHandle::newArray();
        destPage.replaceKey("/Annots", annots);
    }
    annots.appendItem(annot);
}

// Visual (post-/Rotate) page height in points — same convention as
// stampPageMarkups's w/h computation, duplicated rather than shared since
// stampPageMarkups doesn't otherwise need to expose it.
double pageVisualHeightPts(QPDFObjectHandle page)
{
    QPDFPageObjectHelper helper(page);
    QPDFObjectHandle mediaBox = helper.getMediaBox();
    double w = mediaBox.getArrayItem(2).getNumericValue() - mediaBox.getArrayItem(0).getNumericValue();
    double h = mediaBox.getArrayItem(3).getNumericValue() - mediaBox.getArrayItem(1).getNumericValue();
    QPDFObjectHandle rotateObj = helper.getAttribute("/Rotate", false);
    int rotate = rotateObj.isInteger() ? rotateObj.getIntValueAsInt() : 0;
    rotate = ((rotate % 360) + 360) % 360;
    if (rotate == 90 || rotate == 270) {
        std::swap(w, h);
    }
    return h;
}

} // namespace

namespace {

// Appends a new top-level outline (bookmark) entry pointing at the top of
// the given page, creating the /Outlines tree if the document doesn't have
// one yet. Existing outline entries (the document's native table of
// contents, if any) are left untouched — qpdf loaded them as part of
// processFile(), so they simply round-trip through QPDFWriter unchanged.
void appendBookmark(QPDF &qpdf, const std::vector<QPDFObjectHandle> &pages, const PdfWriter::Bookmark &bookmark)
{
    if (bookmark.pageIndexZeroBased < 0 || bookmark.pageIndexZeroBased >= int(pages.size())) {
        return;
    }

    QPDFObjectHandle root = qpdf.getRoot();
    QPDFObjectHandle outlines = root.getKey("/Outlines");
    if (!outlines.isDictionary()) {
        outlines = qpdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));
        outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(0));
        root.replaceKey("/Outlines", outlines);
    }

    QPDFObjectHandle dest = QPDFObjectHandle::newArray();
    dest.appendItem(pages[bookmark.pageIndexZeroBased]);
    dest.appendItem(QPDFObjectHandle::newName("/Fit"));

    QPDFObjectHandle item = qpdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    item.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(bookmark.title.toStdString()));
    item.replaceKey("/Dest", dest);
    item.replaceKey("/Parent", outlines);

    QPDFObjectHandle last = outlines.getKey("/Last");
    if (!last.isDictionary()) {
        outlines.replaceKey("/First", item);
        outlines.replaceKey("/Last", item);
    } else {
        last.replaceKey("/Next", item);
        item.replaceKey("/Prev", last);
        outlines.replaceKey("/Last", item);
    }

    const long long count = outlines.getKey("/Count").isInteger() ? outlines.getKey("/Count").getIntValue() : 0;
    outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(count + 1));
}

// Walks the document's existing outline tree by a path of child indices
// from the root (matching the order PdfDocument::outline() produced) and
// retitles the node found there, if any.
void applyRename(QPDF &qpdf, const PdfWriter::OutlineRename &rename)
{
    if (rename.path.isEmpty()) {
        return;
    }
    QPDFOutlineDocumentHelper odh(qpdf);
    std::vector<QPDFOutlineObjectHelper> level = odh.getTopLevelOutlines();
    for (int depth = 0; depth < rename.path.size(); ++depth) {
        const int idx = rename.path[depth];
        if (idx < 0 || idx >= int(level.size())) {
            return;
        }
        if (depth == rename.path.size() - 1) {
            level[idx].getObjectHandle().replaceKey("/Title",
                                                      QPDFObjectHandle::newUnicodeString(rename.newTitle.toStdString()));
            return;
        }
        level = level[idx].getKids();
    }
}

} // namespace

bool PdfWriter::save(const QString &sourcePath, const QString &destPath, const QString &newTitle,
                      const QString &newAuthor, const QVector<int> &pageOrderZeroBased,
                      const QVector<Bookmark> &newBookmarks, const QVector<OutlineRename> &renames,
                      int rotationQuarterTurns, const QVector<ExternalInsertion> &insertions,
                      const QVector<PageLabel> &pageLabels, const std::function<bool(int)> &pageHasMarkups,
                      const std::function<void(int, QPainter &)> &paintMarkups,
                      const std::function<QVector<NoteAnchor>(int)> &notesForPage,
                      const std::function<QVector<int>(int)> &removedAnnotationOrdinalsForPage, QString *errorMessage)
{
    // qpdf reads lazily from the source file as it writes the output, so
    // writing directly over sourcePath while it's still the open source
    // risks corruption. Write to a sibling temp file and atomically replace
    // afterward instead, whenever source and destination are the same file.
    const bool inPlace = QFileInfo(sourcePath).canonicalFilePath() == QFileInfo(destPath).canonicalFilePath();
    const QString actualDest = inPlace ? destPath + QStringLiteral(".directpdf-tmp") : destPath;

    try {
        QPDF qpdf;
        qpdf.processFile(sourcePath.toUtf8().constData());

        QPDFPageDocumentHelper pdh(qpdf);
        std::vector<QPDFPageObjectHelper> pageHelpers = pdh.getAllPages();
        std::vector<QPDFObjectHandle> pages;
        pages.reserve(pageHelpers.size());
        for (QPDFPageObjectHelper &p : pageHelpers) {
            pages.push_back(p.getObjectHandle());
        }

        // sourceIndexForPages[i] is the original page index pages[i] came
        // from — identity if pages was never reordered, or a copy of
        // pageOrderZeroBased otherwise. Needed below to look up markups by
        // original page index regardless of any reordering/duplication.
        std::vector<int> sourceIndexForPages;
        sourceIndexForPages.reserve(pages.size());
        for (int i = 0; i < int(pages.size()); ++i) {
            sourceIndexForPages.push_back(i);
        }

        if (!pageOrderZeroBased.isEmpty()) {
            // No longer required to be a plain permutation: an original
            // page index missing from the list means it was deleted;
            // appearing more than once means it was duplicated.
            std::vector<QPDFObjectHandle> reordered;
            reordered.reserve(pageOrderZeroBased.size());
            std::vector<bool> usedOnce(pages.size(), false);
            for (int srcIndex : pageOrderZeroBased) {
                if (srcIndex < 0 || srcIndex >= int(pages.size())) {
                    if (errorMessage) {
                        *errorMessage = QStringLiteral("Internal error: invalid page index while reordering.");
                    }
                    return false;
                }
                QPDFObjectHandle pageToUse = pages[srcIndex];
                if (usedOnce[srcIndex]) {
                    // A duplicated page needs its own distinct page object —
                    // otherwise the same indirect reference would appear
                    // twice in /Pages, which most readers tolerate but isn't
                    // a real, independent page.
                    pageToUse = qpdf.makeIndirectObject(pageToUse.shallowCopy());
                } else {
                    usedOnce[srcIndex] = true;
                }
                reordered.push_back(pageToUse);
            }
            pages = reordered;
            sourceIndexForPages.assign(pageOrderZeroBased.begin(), pageOrderZeroBased.end());
        }

        for (const ExternalInsertion &insertion : insertions) {
            QPDF extQpdf;
            try {
                extQpdf.processFile(insertion.sourcePath.toUtf8().constData());
            } catch (const std::exception &e) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Could not open \"%1\" to insert pages from it: %2")
                                        .arg(insertion.sourcePath, QString::fromStdString(e.what()));
                }
                return false;
            }
            QPDFPageDocumentHelper extPdh(extQpdf);
            std::vector<QPDFPageObjectHelper> extPages = extPdh.getAllPages();

            std::vector<QPDFObjectHandle> toInsert;
            toInsert.reserve(insertion.pageIndices.size());
            for (int idx : insertion.pageIndices) {
                if (idx < 0 || idx >= int(extPages.size())) {
                    continue;
                }
                toInsert.push_back(qpdf.copyForeignObject(extPages[idx].getObjectHandle()));
            }

            const int insertPos = std::clamp(insertion.afterDisplayPosition + 1, 0, int(pages.size()));
            pages.insert(pages.begin() + insertPos, toInsert.begin(), toInsert.end());
            // -1 = "no original index" — these pages never existed in
            // sourcePath, so they're never candidates for markup stamping.
            sourceIndexForPages.insert(sourceIndexForPages.begin() + insertPos, toInsert.size(), -1);
        }

        if (!pageOrderZeroBased.isEmpty() || !insertions.isEmpty()) {
            QPDFObjectHandle pagesDict = qpdf.getRoot().getKey("/Pages");
            QPDFObjectHandle kids = QPDFObjectHandle::newArray();
            for (QPDFObjectHandle &p : pages) {
                kids.appendItem(p);
            }
            pagesDict.replaceKey("/Kids", kids);
            pagesDict.replaceKey("/Count", QPDFObjectHandle::newInteger(static_cast<long long>(pages.size())));
        }

        if (!newTitle.isEmpty() || !newAuthor.isEmpty()) {
            QPDFObjectHandle trailer = qpdf.getTrailer();
            QPDFObjectHandle info = trailer.getKey("/Info");
            if (!info.isDictionary()) {
                info = qpdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
                trailer.replaceKey("/Info", info);
            }
            if (!newTitle.isEmpty()) {
                info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(newTitle.toStdString()));
            }
            if (!newAuthor.isEmpty()) {
                info.replaceKey("/Author", QPDFObjectHandle::newUnicodeString(newAuthor.toStdString()));
            }
        }

        for (const OutlineRename &rename : renames) {
            applyRename(qpdf, rename);
        }

        for (const Bookmark &bookmark : newBookmarks) {
            appendBookmark(qpdf, pages, bookmark);
        }

        if (pageHasMarkups && paintMarkups) {
            QTemporaryDir scratchDir;
            if (!scratchDir.isValid()) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Could not create a scratch directory to flatten markups.");
                }
                return false;
            }
            for (int i = 0; i < int(pages.size()); ++i) {
                const int srcIndex = sourceIndexForPages[i];
                if (srcIndex < 0 || !pageHasMarkups(srcIndex)) {
                    continue; // srcIndex < 0: a page inserted from another file, never had markups to begin with
                }
                if (!stampPageMarkups(qpdf, pages[i], srcIndex, scratchDir.path(), paintMarkups, errorMessage)) {
                    return false;
                }
            }
        }

        if (notesForPage) {
            for (int i = 0; i < int(pages.size()); ++i) {
                const int srcIndex = sourceIndexForPages[i];
                if (srcIndex < 0) {
                    continue;
                }
                const QVector<NoteAnchor> notes = notesForPage(srcIndex);
                if (notes.isEmpty()) {
                    continue;
                }
                const double heightPts = pageVisualHeightPts(pages[i]);
                for (const NoteAnchor &note : notes) {
                    addNoteAnnotation(qpdf, pages[i], heightPts, note);
                }
            }
        }

        if (removedAnnotationOrdinalsForPage) {
            for (int i = 0; i < int(pages.size()); ++i) {
                const int srcIndex = sourceIndexForPages[i];
                if (srcIndex < 0) {
                    continue;
                }
                QVector<int> ordinals = removedAnnotationOrdinalsForPage(srcIndex);
                if (ordinals.isEmpty()) {
                    continue;
                }
                QPDFObjectHandle annots = pages[i].getKey("/Annots");
                if (!annots.isArray()) {
                    continue;
                }
                // Highest ordinal first, so erasing one doesn't shift the
                // array positions of the others still to be erased.
                std::sort(ordinals.begin(), ordinals.end(), std::greater<int>());
                for (int ordinal : ordinals) {
                    if (ordinal >= 0 && ordinal < annots.getArrayNItems()) {
                        annots.eraseItem(ordinal);
                    }
                }
            }
        }

        if (!pageLabels.isEmpty()) {
            // A single-page range per labeled page, with only /P (prefix)
            // set and no /S (numbering style) — per the PDF spec, omitting
            // /S means the displayed label is the prefix text alone, with
            // no number appended. This is a real, standard PDF mechanism
            // (other viewers' page-label displays show it too), not a
            // DirectPDF-only convention, even though DirectPDF is
            // currently the only thing that *writes* labels this way.
            QHash<int, QString> labelBySource;
            for (const PageLabel &label : pageLabels) {
                if (!label.text.isEmpty()) {
                    labelBySource.insert(label.sourcePageIndex, label.text);
                }
            }
            QPDFObjectHandle nums = QPDFObjectHandle::newArray();
            for (int i = 0; i < int(pages.size()); ++i) {
                const int srcIndex = sourceIndexForPages[i];
                const auto it = srcIndex >= 0 ? labelBySource.constFind(srcIndex) : labelBySource.constEnd();
                const bool hasLabel = it != labelBySource.constEnd();
                const bool prevHasLabel = i > 0
                    && sourceIndexForPages[i - 1] >= 0
                    && labelBySource.contains(sourceIndexForPages[i - 1]);
                if (hasLabel) {
                    QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
                    dict.replaceKey("/P", QPDFObjectHandle::newUnicodeString(it.value().toStdString()));
                    nums.appendItem(QPDFObjectHandle::newInteger(i));
                    nums.appendItem(dict);
                } else if (prevHasLabel) {
                    // Resume normal decimal numbering matching this page's
                    // true position, so the custom label doesn't "leak"
                    // onto every following page the way number-tree ranges
                    // otherwise would.
                    QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
                    dict.replaceKey("/S", QPDFObjectHandle::newName("/D"));
                    dict.replaceKey("/St", QPDFObjectHandle::newInteger(i + 1));
                    nums.appendItem(QPDFObjectHandle::newInteger(i));
                    nums.appendItem(dict);
                }
            }
            QPDFObjectHandle pageLabelsDict = qpdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
            pageLabelsDict.replaceKey("/Nums", nums);
            qpdf.getRoot().replaceKey("/PageLabels", pageLabelsDict);
        }

        if (rotationQuarterTurns != 0) {
            // Applied after markup stamping, not before: markups are
            // authored in each page's native (pre-session-rotation)
            // orientation (PdfView hides/pauses them while a view rotation
            // is active), so rotating the whole page — content and the
            // now-baked-in markup overlay together — afterward keeps
            // everything aligned, exactly like rotating an already-
            // flattened page in any other PDF viewer would.
            for (QPDFObjectHandle &p : pages) {
                QPDFPageObjectHelper(p).rotatePage(rotationQuarterTurns * 90, /*relative=*/true);
            }
        }

        {
            QPDFWriter writer(qpdf, actualDest.toUtf8().constData());
            writer.write();
        }
    } catch (const std::exception &e) {
        if (errorMessage) {
            *errorMessage = QString::fromStdString(e.what());
        }
        if (inPlace) {
            QFile::remove(actualDest);
        }
        return false;
    }

    if (inPlace) {
        QFile::remove(destPath);
        if (!QFile::rename(actualDest, destPath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Wrote the file but could not replace the original.");
            }
            return false;
        }
    }
    return true;
}

QHash<int, QString> PdfWriter::readPageLabels(const QString &path)
{
    QHash<int, QString> result;
    try {
        QPDF qpdf;
        qpdf.processFile(path.toUtf8().constData());

        QPDFObjectHandle pageLabelsDict = qpdf.getRoot().getKey("/PageLabels");
        if (!pageLabelsDict.isDictionary()) {
            return result;
        }
        QPDFObjectHandle nums = pageLabelsDict.getKey("/Nums");
        if (!nums.isArray()) {
            return result;
        }
        const int n = nums.getArrayNItems();
        for (int i = 0; i + 1 < n; i += 2) {
            QPDFObjectHandle idxObj = nums.getArrayItem(i);
            QPDFObjectHandle dict = nums.getArrayItem(i + 1);
            if (!idxObj.isInteger() || !dict.isDictionary() || dict.hasKey("/S")) {
                continue; // malformed, or ordinary page numbering (not a DirectPDF label)
            }
            QPDFObjectHandle pObj = dict.getKey("/P");
            if (!pObj.isString()) {
                continue;
            }
            result.insert(idxObj.getIntValueAsInt(), QString::fromStdString(pObj.getUTF8Value()));
        }
    } catch (const std::exception &) {
        // Best-effort — a malformed/unreadable file just yields no labels.
    }
    return result;
}
