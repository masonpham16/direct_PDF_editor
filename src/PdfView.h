#pragma once

#include "PdfDocument.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QHash>
#include <QSet>
#include <QVector>

class QTextEdit;
class QPainter;

// Continuous, vertically-scrolling, zoomable PDF page view with word-based
// text selection. This is the "reader" surface; later milestones will layer
// markup editing on top of it (as an overlay painted after the page image).
class PdfView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit PdfView(QWidget *parent = nullptr);

    bool openFile(const QString &filePath, QString *errorMessage = nullptr);
    bool isDocumentLoaded() const { return m_doc.isLoaded(); }

    // Sets the display order shown in the main reading view: displayOrder[d]
    // is the SOURCE (original Poppler) page index shown at display position
    // d. Empty means identity (display order == source order). An index can
    // be omitted (page deleted from the view) or repeated (page duplicated),
    // matching MainWindow::m_pageOrder's contract. Everything else in this
    // class — m_currentPage, selection, search, bookmarks, highlights, text
    // boxes, shapes — stays keyed by SOURCE index; only layout/scroll
    // position and hit-testing need to translate through this.
    void setPageOrder(const QVector<int> &displayOrder);

    int pageCount() const { return m_doc.pageCount(); }
    int currentPage() const { return m_currentPage; }
    double zoom() const { return m_zoom; }
    int rotationQuarterTurns() const { return m_rotationQuarters; }
    bool hasTextSelection() const;
    bool hasImageSelection() const;
    PdfDocument &document() { return m_doc; }

    enum class SelectionMode { Text, Region, AddText, AddShape };
    SelectionMode selectionMode() const { return m_mode; }

    // Line and Arrow are "line-like" — defined by two endpoints rather than
    // a filled bounding box. Polygon is placed by clicking each vertex in
    // turn rather than a single click-drag; see the ShapeObject comment and
    // startPlacingPolygon() below.
    enum class ShapeKind { Rectangle, RoundedRectangle, Ellipse, Diamond, Line, Arrow, Polygon };
    // Arms AddShape mode for the given kind; the next click-drag on the page
    // places one shape, then (like Add Text) reverts to normal Text mode.
    // Not for use with ShapeKind::Polygon — see startPlacingPolygon().
    void startPlacingShape(ShapeKind kind);
    // Arms multi-click polygon placement: click each vertex in turn, then
    // double-click (or Enter) to close the shape, or Escape to cancel.
    void startPlacingPolygon();

    // A user-pasted image placed on top of a page.
    struct PastedImage {
        int page = -1;
        QRectF rectPoints; // natural (unrotated) page point space
        QImage image;
        bool operator==(const PastedImage &) const = default;
    };

    // A user-placed, editable text box. If a third object type gets added,
    // this duplication with PastedImage (select/drag/delete handling) is
    // worth unifying behind a common base; not done yet to keep this change
    // contained.
    struct TextBoxObject {
        int page = -1;
        QRectF rectPoints; // natural (unrotated) page point space; height auto-grows
        QString html;      // rich text content (QTextEdit HTML), formatting applied per-run
        // scaleFactor() in effect when html was captured — font sizes inside
        // the HTML are baked in device pixels at that zoom, so static
        // rendering at a different zoom corrects for it via this ratio.
        qreal htmlScaleFactor = 1.0;
        // Box chrome, independent of the text formatting above. Width 0 /
        // opacity 0 means "invisible" (default — plain floating text).
        QColor borderColor = Qt::black;
        qreal borderWidthPts = 0.0;
        QColor fillColor = Qt::white;
        qreal fillOpacity = 0.0; // 0..1
        QString note; // user comment, shown/edited in the Markups panel
        bool operator==(const TextBoxObject &) const = default;
    };

    // A user-placed basic shape. Shares the same border/fill style fields as
    // TextBoxObject by convention (not a common base — see the duplication
    // note on TextBoxObject) so the generic selectedBorderColor()-style
    // dispatch methods can treat them uniformly.
    //
    // For box-like kinds (Rectangle, RoundedRectangle, Ellipse, Diamond),
    // rectPoints is a normal (always-positive-size) bounding box. For
    // line-like kinds (Line, Arrow), rectPoints.topLeft()/bottomRight() are
    // instead the two literal endpoints in drag order — constructed via
    // QRectF(start, end) and deliberately never normalized, since Arrow
    // needs to know which end is the head. fillColor/fillOpacity are unused
    // for line-like kinds (nothing to fill). For Polygon, rectPoints is
    // unused and polygonPoints holds the vertices instead (closed implicitly
    // — the last point connects back to the first when drawn).
    struct ShapeObject {
        int page = -1;
        ShapeKind kind = ShapeKind::Rectangle;
        QRectF rectPoints; // natural (unrotated) page point space
        QVector<QPointF> polygonPoints; // Polygon only
        QColor borderColor = Qt::black;
        qreal borderWidthPts = 1.0;
        QColor fillColor = Qt::white;
        qreal fillOpacity = 0.0; // 0..1
        QString note; // user comment, shown/edited in the Markups panel
        bool operator==(const ShapeObject &) const = default;
    };

    // A position within a page's extracted text, identified as a specific
    // character of a specific word. Ordering matches reading order, since
    // both words and characters within a word come pre-ordered from Poppler.
    struct TextPos {
        int word = -1;
        int ch = -1;
        bool isValid() const { return word >= 0 && ch >= 0; }
        bool operator<(const TextPos &o) const { return word != o.word ? word < o.word : ch < o.ch; }
        bool operator==(const TextPos &o) const { return word == o.word && ch == o.ch; }
    };
    struct SearchMatch {
        int page = -1;
        TextPos start;
        TextPos end;
    };

    // A persistent user-made highlight over a text range. Stored by text
    // position (not a freeform rect) so it stays correctly placed across
    // zoom changes the same way the live text selection does.
    struct HighlightAnnotation {
        int page = -1;
        TextPos start;
        TextPos end;
        QColor color = QColor(255, 235, 59); // soft yellow, the common highlighter color
        QString note;
        bool operator==(const HighlightAnnotation &) const = default;
    };

    // A snapshot of everything that ends up as saved-PDF content (not any
    // UI-only state like selection/current page/zoom) — used by MainWindow
    // to implement undo/redo without needing to know PdfView's internals.
    struct MarkupSnapshot {
        QVector<HighlightAnnotation> highlights;
        QVector<TextBoxObject> textBoxes;
        QVector<ShapeObject> shapes;
        QVector<PastedImage> pastedImages;
        bool operator==(const MarkupSnapshot &) const = default;
    };
    MarkupSnapshot captureMarkupSnapshot() const;
    // Restores a previously-captured snapshot wholesale (replacing current
    // highlights/text boxes/shapes/pasted images) and clears any selection/
    // in-progress edit, since indices into the old state may no longer be
    // meaningful. Deliberately does NOT emit sessionContentChanged() — this
    // is the undo/redo path restoring a PAST state, not a new edit.
    void restoreMarkupSnapshot(const MarkupSnapshot &snapshot);

    // Searches every page's extracted text (treating each page as reading-
    // order text with inferred spaces/newlines, so phrases spanning words
    // are matched too) for substring occurrences of query, case-insensitive.
    QVector<SearchMatch> findAll(const QString &query);
    void showSearchMatch(const SearchMatch &match);
    void clearSearchHighlight();

    // Pages with at least one bookmark pointing to them get a small ribbon
    // marker drawn in the corner. MainWindow recomputes and pushes this set
    // whenever bookmarks change (added, or the document reloads).
    void setBookmarkedPages(const QSet<int> &pages);

    // Current text selection, if any (page == -1 if none). Used by "Add
    // Bookmark" to capture a precise jump target + highlight range.
    struct SelectionRange {
        int page = -1;
        TextPos start;
        TextPos end;
    };
    SelectionRange currentTextSelection() const;
    QString selectedText();

    // --- Persistent text highlights (used by the Highlights panel) ---
    // Unlike the transient selection/search highlights above, these stick
    // around on the page until removed, and carry an optional note.
    struct HighlightEntry {
        int index = -1;
        int page = -1;
        QString text; // snippet of the highlighted text, for display
        QString note;
    };
    // Converts the current text selection into a highlight; false (no-op)
    // if there is no selection.
    bool addHighlightFromSelection();
    int highlightCount() const { return m_highlights.size(); }
    HighlightEntry highlightEntry(int index);
    void setHighlightNote(int index, const QString &note);
    // Also becomes the sticky default for the next highlight created via
    // addHighlightFromSelection(), same idea as the text box/shape style
    // defaults below.
    void setHighlightColor(int index, const QColor &color);
    QColor highlightColor(int index) const;
    void removeHighlight(int index);
    void jumpToHighlight(int index);

    // --- Unified markup list (used by the Markups panel) ---
    // Every placed object — highlights, text boxes, and shapes — in one
    // flat list, so there's a single place to see everything on the
    // document, comment on any of it, and jump straight to its page.
    // `index` here is a position in this combined list (highlights first,
    // then text boxes, then shapes), NOT the same as e.g. highlightEntry()'s
    // index into just the highlights.
    enum class MarkupKind { Highlight, TextBox, Shape };
    struct MarkupEntry {
        int index = -1;
        MarkupKind kind = MarkupKind::Highlight;
        int page = -1;
        QString description; // text snippet (highlight/text box) or shape kind name
        QString note;
    };
    int markupCount() const;
    MarkupEntry markupEntry(int index);
    void setMarkupNote(int index, const QString &note);
    void removeMarkup(int index);
    void jumpToMarkup(int index);
    // Pasted images aren't part of the unified markup list above (no note/
    // jump-to support for them yet), but MainWindow still needs to know
    // whether any exist to detect unsaved changes.
    int pastedImageCount() const { return m_pastedImages.size(); }
    // True if sourcePage has at least one highlight/text box/shape/pasted
    // image — lets the save path skip pages with nothing to flatten.
    bool pageHasMarkups(int sourcePage) const;
    // Renders this page's highlights/text boxes/shapes/pasted images onto
    // painter at 1:1 point scale, origin (0,0) = this page's top-left
    // corner, no selection/editing chrome — used to flatten markups into
    // the saved PDF (see PdfWriter::save's markupPages parameter).
    void paintMarkupsForExport(QPainter &painter, int sourcePage) const
    {
        paintPageMarkups(painter, sourcePage, QPointF(0, 0), 1.0, false);
    }

    // One entry per highlight/text box/shape on sourcePage that has a
    // non-empty note, giving where to anchor it (natural, unrotated page
    // point space, origin top-left) — used to write real PDF sticky-note
    // (/Text) annotations alongside the flattened visual content, since
    // notes themselves aren't something to draw on the page.
    struct NoteExport {
        QRectF anchorRectPoints;
        QString text;
    };
    QVector<NoteExport> notesForPage(int sourcePage) const;

    // --- Text box formatting (used by FormatPanel) ---
    // The live rich-text editor while a text box is actively being edited
    // (double-clicked, or just placed), or nullptr otherwise. FormatPanel
    // drives formatting directly through QTextEdit's own rich-text API
    // (setFontBold, setTextColor, textCursor().insertList(), etc.) rather
    // than DirectPDF reimplementing any of that.
    QTextEdit *activeTextEditor() const { return m_textEditWidget; }
    void refocusActiveEditor();
    // Commits whatever's in the live text-box editor (if one is open) back
    // into its TextBoxObject::html, same as clicking away from it normally
    // would. MainWindow calls this before Save so a box being actively
    // typed into when Save happens still gets its latest content exported.
    void commitPendingEdits() { finishEditingTextBox(); }
    bool hasSelectedTextBox() const { return m_selectedTextBoxIndex >= 0; }
    // For a box that's merely selected (clicked once, not double-clicked
    // into) — starts editing it and selects all its content, then returns
    // the editor, so a FormatPanel control applies to the *whole* block
    // instead of silently doing nothing. Returns the already-active editor
    // unchanged (no forced re-select) if one already exists, so formatting
    // a specific in-progress selection while actually typing still works
    // exactly as before.
    QTextEdit *beginFormattingSelectedTextBox();
    // Non-invasive read of a selected-but-not-editing box's formatting, for
    // FormatPanel to display without the side effect of actually starting
    // an edit (unlike beginFormattingSelectedTextBox() above).
    struct TextFormatSnapshot {
        QString fontFamily = QStringLiteral("Helvetica");
        qreal fontPointSize = 14.0;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        QColor textColor = Qt::black;
        QColor highlightColor = Qt::transparent;
        Qt::Alignment alignment = Qt::AlignLeft;
    };
    TextFormatSnapshot selectedTextBoxFormatSnapshot() const;
    QColor selectedTextBoxBorderColor() const;
    qreal selectedTextBoxBorderWidthPts() const;
    QColor selectedTextBoxFillColor() const;
    qreal selectedTextBoxFillOpacity() const; // 0..1
    void setSelectedTextBoxBorder(const QColor &color, qreal widthPts);
    void setSelectedTextBoxFill(const QColor &color, qreal opacity);
    // Explicit, one-shot sizing actions (there is no auto-grow/auto-fit
    // while typing — the box stays exactly the size it was placed/resized
    // at, and overflowing text is clipped, until one of these is used).
    void fitSelectedTextBoxToContent();  // resize the box height to match its content
    void shrinkSelectedTextBoxToFit();   // scale the text's font sizes down to fit the current box

    // --- Shape styling (used by FormatPanel) ---
    bool hasSelectedShape() const { return m_selectedShapeIndex >= 0; }
    // Generic dispatch across whichever "styleable" object (text box or
    // shape) is currently selected, so FormatPanel's Box group can act on
    // either without caring which. Falls back to the text-box-specific
    // accessors above when a text box is selected; shapes use these.
    bool hasSelectedStyleable() const { return hasSelectedTextBox() || hasSelectedShape(); }
    // Line/Arrow have no interior to fill — FormatPanel disables the Fill
    // controls when this is true.
    bool hasSelectedLineLikeShape() const;
    QColor selectedBorderColor() const;
    qreal selectedBorderWidthPts() const;
    QColor selectedFillColor() const;
    qreal selectedFillOpacity() const; // 0..1
    void setSelectedBorder(const QColor &color, qreal widthPts);
    void setSelectedFill(const QColor &color, qreal opacity);

public slots:
    void zoomIn();
    void zoomOut();
    void setZoom(double zoom);
    void zoomToActualSize();
    void zoomToFitWidth();
    void goToPage(int pageIndex);
    void copySelectionToClipboard();
    void setSelectionMode(SelectionMode mode);
    void selectAllOnPage();
    void rotateClockwise();
    void rotateCounterclockwise();
    void pasteImageFromClipboard();
    void deleteSelectedObject();

signals:
    void zoomChanged(double zoom);
    void currentPageChanged(int pageIndex);
    void documentChanged();
    void rotationChanged(int quarterTurns);
    void contextMenuRequestedAt(const QPoint &globalPos);
    // Fires whenever text-box editing starts/stops, the selected text box
    // changes, or the live cursor's formatting state changes (bold toggled,
    // alignment changed, etc.) — FormatPanel listens to this to refresh.
    void textBoxStateChanged();
    // Fires on any mode change, including the Add Text tool auto-reverting
    // to Text mode after placing one box — MainWindow syncs its toolbar
    // buttons' checked state from this rather than assuming it drove the
    // change itself.
    void selectionModeChanged(SelectionMode mode);
    // Fires whenever a highlight is added, removed, or has its note edited
    // — the Highlights panel listens to this to refresh its list.
    void highlightsChanged();
    // Fires whenever any placed object (highlight, text box, or shape) is
    // added, removed, or has its note edited — the Markups panel listens
    // to this to refresh its list.
    void markupsChanged();
    // Fires whenever a NEW user edit changes highlights/text boxes/shapes/
    // pasted images — add, remove, or commit (color/note edits too), but
    // NOT restoreMarkupSnapshot() itself, so MainWindow's undo stack
    // (which listens to this) doesn't record its own undo/redo as a new
    // edit. Page order, title/author, bookmarks, and rotation are tracked
    // separately by MainWindow, which already owns that state directly.
    void sessionContentChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct PageLayout {
        qreal top = 0;      // logical px, document space
        qreal height = 0;   // logical px
        qreal width = 0;    // logical px
    };

    void recomputeLayout();
    void updateScrollRanges();
    const QImage &pageImage(int pageIndex);
    double scaleFactor() const; // points -> logical px, at current zoom
    bool hitTestPage(const QPoint &viewportPos, int *pageIndexOut, QPointF *pdfPointOut) const;
    // Display order <-> source index translation (see setPageOrder()).
    // Both are identity passthroughs when m_displayOrder is empty, and both
    // clamp out-of-range input to a valid result rather than returning -1,
    // since callers use these for layout indexing that must stay in bounds.
    int sourceForDisplay(int displayIndex) const;
    int displayForSource(int sourceIndex) const;
    int displayCount() const { return m_displayOrder.isEmpty() ? m_doc.pageCount() : m_displayOrder.size(); }
    // Raw scroll-to-slot, operating directly on a display position rather
    // than translating from a source index first — used for Home/End, which
    // should mean "first/last page as currently arranged."
    void goToDisplayPosition(int displayIndex);
    const QVector<PdfTextWord> &wordsForPage(int pageIndex);
    TextPos nearestCharPos(int pageIndex, const QPointF &pdfPoint) const;
    QVector<QRectF> selectionRowRects(int pageIndex, TextPos lo, TextPos hi) const;
    QString textForRange(int pageIndex, TextPos lo, TextPos hi);
    const QVector<QRectF> &imageBoxesForPage(int pageIndex);
    bool tryStartImageSelection(int pageIndex, const QPointF &pdfPoint);
    void updateCurrentPageFromScroll();
    void clearImageCache();
    qreal pageLeft(int pageIndex) const;
    QPointF projectToPage(int pageIndex, const QPoint &viewportPos) const;
    int pastedImageAt(int pageIndex, const QPointF &pdfPoint) const;
    QRect pageScreenRect(int pageIndex) const;
    int textBoxAt(int pageIndex, const QPointF &pdfPoint) const;
    void startEditingTextBox(int index);
    void finishEditingTextBox();
    void updateTextEditGeometry();
    void updateTextEditBackground();
    qreal measureTextBoxContentHeightPts(int index) const;

    // Draws this page's highlights, pasted images, text boxes, and shapes
    // (the four "markup" layers — not the page image itself, nor any
    // transient selection/editing chrome) onto painter. origin is where
    // this page's top-left (natural, unrotated point-space) corner lands
    // in painter coordinates, and scale converts points to painter units.
    // interactive=true reproduces the exact on-screen look (selection
    // outlines/handles, hiding pasted images while view-rotated, skipping
    // the live-edit box in favor of its real QTextEdit widget) — used by
    // paintEvent(). interactive=false renders just the plain final content
    // with no UI chrome, regardless of any current selection/edit/view-
    // rotation state — used by paintMarkupsForExport() when flattening
    // markups into the saved PDF.
    void paintPageMarkups(QPainter &painter, int sourcePage, const QPointF &origin, qreal scale,
                          bool interactive) const;

    enum class ResizeHandle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
    ResizeHandle resizeHandleAt(const QRectF &rectPoints, const QPointF &pdfPoint) const;
    // Line-like shapes only ever report TopLeft (start point) or
    // BottomRight (end point) — the generic 8-way hit test above is
    // ambiguous for a shape with zero width or height (an axis-aligned
    // line), so this checks proximity to just the two endpoints instead.
    ResizeHandle lineHandleAt(const QRectF &rectPoints, const QPointF &pdfPoint) const;
    Qt::CursorShape cursorForHandle(ResizeHandle handle) const;
    void drawResizeHandles(QPainter &painter, const QRectF &screenRect) const;
    void drawEndpointHandles(QPainter &painter, const QPointF &p1, const QPointF &p2) const;
    // Index of the nearest polygon vertex within hit tolerance, or -1.
    int polygonVertexAt(const QVector<QPointF> &points, const QPointF &pdfPoint) const;
    void drawPolygonVertexHandles(QPainter &painter, const QVector<QPointF> &screenPoints) const;
    int shapeAt(int pageIndex, const QPointF &pdfPoint) const;
    // p1/p2 are the two corners (box-like) or endpoints (line-like) already
    // transformed to screen space.
    void drawShape(QPainter &painter, const ShapeObject &shape, const QPointF &p1, const QPointF &p2) const;
    // screenPoints are the polygon's vertices already transformed to screen
    // space, in order.
    void drawPolygonShape(QPainter &painter, const ShapeObject &shape, const QVector<QPointF> &screenPoints) const;
    void finishPlacingPolygon(bool commit); // commit=false discards (Escape / too few vertices)

    PdfDocument m_doc;
    QVector<PageLayout> m_layout; // indexed by DISPLAY position; see setPageOrder()
    QVector<int> m_displayOrder; // display position -> source page index; empty = identity
    QHash<int, QImage> m_imageCache;
    QHash<int, QVector<PdfTextWord>> m_wordsCache;
    QHash<int, QVector<QRectF>> m_imageBoxesCache;

    double m_zoom = 1.0;
    static constexpr double kLogicalDpi = 96.0;
    static constexpr double kPageGap = 12.0;
    static constexpr double kSideMargin = 16.0;

    int m_currentPage = 0;
    qreal m_contentWidth = 0;
    qreal m_contentHeight = 0;
    QPoint m_pressPos;

    // Selection is confined to a single page in this milestone.
    bool m_selecting = false;
    int m_selPage = -1;
    TextPos m_selAnchor;
    TextPos m_selHead;

    // Region ("snip") selection: a free rectangle for copying a crop of the
    // page (image, diagram, etc.) rather than extracted text.
    SelectionMode m_mode = SelectionMode::Text;
    bool m_regionSelecting = false;
    int m_regionPage = -1;
    QPointF m_regionAnchor;
    QPointF m_regionHead;

    // View-only page rotation (0-3 quarter turns clockwise); not written
    // back to the document. Click-to-select is paused while rotated rather
    // than re-deriving the full natural<->display coordinate transform for
    // every interaction path, since this is a secondary feature.
    int m_rotationQuarters = 0;

    QVector<PastedImage> m_pastedImages;
    int m_draggingPastedIndex = -1;
    QPointF m_dragGrabOffset; // press point minus object's top-left, in points
    int m_selectedPastedIndex = -1;

    int m_searchPage = -1;
    TextPos m_searchStart;
    TextPos m_searchEnd;

    QSet<int> m_bookmarkedPages;

    QVector<HighlightAnnotation> m_highlights;
    QColor m_defaultHighlightColor = QColor(255, 235, 59); // soft yellow, the common highlighter color

    // "Sticky" style state: whenever the user adjusts a text box's border/
    // fill, or its font/color while editing, remembered here so the next
    // NEW text box starts out styled the same way instead of resetting to a
    // fixed default every time — same idea as most word processors/design
    // tools carrying forward your last-used formatting.
    QColor m_defaultTextBoxBorderColor = Qt::black;
    qreal m_defaultTextBoxBorderWidthPts = 0.0;
    QColor m_defaultTextBoxFillColor = Qt::white;
    qreal m_defaultTextBoxFillOpacity = 0.0;
    QString m_defaultTextFontFamily = QStringLiteral("Helvetica");
    qreal m_defaultTextFontPointSize = 14.0;
    bool m_defaultTextBold = false;
    bool m_defaultTextItalic = false;
    bool m_defaultTextUnderline = false;
    QColor m_defaultTextColor = Qt::black;

    QVector<TextBoxObject> m_textBoxes;
    int m_selectedTextBoxIndex = -1;
    int m_draggingTextBoxIndex = -1;
    QPointF m_textDragGrabOffset;
    int m_editingTextBoxIndex = -1;
    QTextEdit *m_textEditWidget = nullptr;

    int m_resizingTextBoxIndex = -1;
    ResizeHandle m_resizeHandle = ResizeHandle::None;
    QRectF m_resizeStartRect;
    QPointF m_resizeStartPoint;

    // Same idea as the text box defaults above, for shapes.
    QColor m_defaultShapeBorderColor = Qt::black;
    qreal m_defaultShapeBorderWidthPts = 1.0;
    QColor m_defaultShapeFillColor = Qt::white;
    qreal m_defaultShapeFillOpacity = 0.0;

    QVector<ShapeObject> m_shapes;
    int m_selectedShapeIndex = -1;
    int m_draggingShapeIndex = -1;
    QPointF m_shapeDragGrabOffset;
    int m_resizingShapeIndex = -1;
    ResizeHandle m_shapeResizeHandle = ResizeHandle::None;
    QRectF m_shapeResizeStartRect;
    QPointF m_shapeResizeStartPoint;

    // AddShape placement: press = anchor, drag = live preview, release =
    // finalize + auto-revert to Text mode (one-shot, like Add Text).
    bool m_placingShape = false;
    int m_shapePlacingPage = -1;
    QPointF m_shapeStartPoint;
    QPointF m_shapeCurrentPoint;
    ShapeKind m_pendingShapeKind = ShapeKind::Rectangle;

    // Polygon placement: click adds a vertex, move updates the rubber-band
    // line from the last vertex to the cursor, double-click/Enter closes
    // and commits, Escape cancels. A different interaction shape than the
    // single click-drag the other shapes use, so it's tracked separately
    // rather than reusing m_placingShape.
    bool m_placingPolygon = false;
    int m_polygonPlacingPage = -1;
    QVector<QPointF> m_polygonPoints;
    QPointF m_polygonCursorPoint;

    // Dragging a single vertex of an already-placed, selected polygon.
    int m_resizingPolygonVertex = -1;
    // Dragging the whole polygon by its interior (translates every vertex
    // by the same delta from this snapshot + the press point).
    QVector<QPointF> m_polygonDragStartPoints;
    QPointF m_polygonDragStartMouse;
};
