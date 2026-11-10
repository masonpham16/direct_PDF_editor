#pragma once

#include "PdfView.h"
#include "PdfWriter.h"

#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QVector>

class ThumbnailPanel;
class FormatPanel;
class QDockWidget;
class QLabel;
class QDragEnterEvent;
class QDropEvent;
class QCloseEvent;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QMenu;
class QEvent;
class QLineEdit;
class QTableWidget;
class QTableWidgetItem;
class QToolBar;
class QToolButton;
class QTimer;
class QUndoStack;
class QUndoGroup;
class QTabWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    struct DocumentTab;

    // Opens path in a brand-new tab and makes it active. Used for File >
    // Open, recent files, drag-and-drop, and the initial command-line
    // argument — every case of "show me this file" now just adds a tab
    // rather than replacing whatever's already open. Returns the new tab,
    // or nullptr (and the tab removed again) if the file couldn't be
    // opened.
    DocumentTab *openPathInNewTab(const QString &path);

    // What a Bookmarks-panel entry jumps to and (if it came from a text
    // selection) highlights. Tracked separately from the QTreeWidgetItem's
    // displayed text so renaming doesn't disturb it.
    struct BookmarkTarget {
        int page = -1;
        bool hasTextRange = false;
        PdfView::TextPos start;
        PdfView::TextPos end;
        QVector<int> nativePath;  // path into the file's existing outline; empty if pending
        int pendingIndex = -1;    // index into the tab's pendingBookmarks; -1 if a native outline entry
    };

    // Everything undo/redo needs to restore: page arrangement plus every
    // highlight/text box/shape/pasted image. Deliberately does NOT cover
    // title/author/bookmarks/renames/rotation — those are lower-frequency,
    // dialog-driven edits where "undo" isn't really the expected
    // interaction (Properties/Bookmarks panel already let you just change
    // them back), so scoping undo to page + markup operations covers the
    // cases people actually reach for Ctrl+Z after.
    struct SessionSnapshot {
        QVector<int> pageOrder;
        PdfView::MarkupSnapshot markups;
        bool operator==(const SessionSnapshot &) const = default;
    };

    // One open document: its view plus every piece of state that used to
    // live directly on MainWindow back when there was only ever one
    // document open at a time. One of these exists per open tab; the
    // dock panels (Pages/Outline/Markups/Format) stay single, shared
    // widgets that get repopulated from whichever DocumentTab is active
    // rather than being duplicated per tab.
    struct DocumentTab {
        PdfView *view = nullptr;
        QString currentFilePath;
        // Pending, unsaved changes — live in the main reading view and
        // Pages/Outline panels immediately, but only actually written to
        // disk by Save.
        // display position -> source page index; empty = identity. An
        // index can be omitted (deleted) or repeated (duplicated) — no
        // longer guaranteed to be a plain permutation of every page.
        QVector<int> pageOrder;
        QHash<int, QImage> thumbnailImageCache; // source page index -> rendered thumbnail
        int copiedSourcePage = -1; // single-page internal clipboard for Copy/Paste Page (per-tab, not shared)
        QVector<PdfWriter::Bookmark> pendingBookmarks;
        // Empty means "no pending change" (PdfWriter::save's contract).
        // Clearing a field to blank in the Properties dialog is treated
        // as "no change" rather than "explicitly set to empty" — a minor
        // simplification.
        QString pendingTitle;
        QString pendingAuthor;
        QVector<PdfWriter::OutlineRename> pendingRenames;
        // Empty until the first autosave actually writes something for
        // this tab (see writeRecoverySnapshotFor()).
        QString recoveryFilePath;
        QUndoStack *undoStack = nullptr;
        // Running baseline for onSessionContentChanged()'s diff — kept in
        // sync by every push (whether or not it actually pushed a
        // command) and by restoreSessionSnapshot(), so it always reflects
        // "what the undo stack currently thinks is true".
        SessionSnapshot lastSnapshot;
    };

    SessionSnapshot captureSessionSnapshot() const;
    // Used by the undo stack's commands; also usable directly. Does not
    // itself push anything onto the undo stack. Always acts on the
    // currently active tab — safe because a tab's undo stack can only
    // become the QUndoGroup's active stack (and thus only have undo()/
    // redo() invoked on it) while that tab is the active one.
    void restoreSessionSnapshot(const SessionSnapshot &snapshot);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onOpenTriggered();
    void onZoomChanged(double zoom);
    void onCurrentPageChanged(int pageIndex);
    void onDocumentChanged();
    void onRotationChanged(int quarterTurns);
    void onContextMenuRequested(const QPoint &globalPos);
    void onGoToPageRequested();
    void onPrintTriggered();
    void onPropertiesTriggered();
    void onThumbnailActivated(int sourcePageIndex);
    void onThumbnailReorderRequested(int sourceDisplayPosition, int targetDisplayPosition, bool after);
    void onThumbnailDeleteRequested(int displayPosition);
    void onThumbnailCopyRequested(int displayPosition);
    void onThumbnailPasteRequested(int displayPosition);
    void onThumbnailContextMenu(int displayPosition, const QPoint &globalPos);
    void onOutlineItemActivated(QTreeWidgetItem *item);
    void onOutlineItemChanged(QTreeWidgetItem *item, int column);
    void onOutlineContextMenu(const QPoint &pos);
    void onRecentFileTriggered();
    void onSaveTriggered();
    void onSaveAsTriggered();
    void onAddBookmarkTriggered();
    void onRevertTriggered();
    void onFullScreenToggled(bool checked);
    void onAboutTriggered();
    void onShortcutsTriggered();
    void onUserManualTriggered();
    void onFindTriggered();
    void onFindTextChanged(const QString &text);
    void onFindNext();
    void onFindPrevious();
    void onFindBarClosed();
    void onAddHighlightTriggered();
    void refreshMarkupsPanel();
    void onMarkupCellClicked(int row, int column);
    void onMarkupNoteEdited(QTableWidgetItem *item);
    void onMarkupsContextMenu(const QPoint &pos);
    // Connected to PdfView::sessionContentChanged() — a highlight/text box/
    // shape/pasted image was just added, removed, or committed. Diffs
    // against the active tab's lastSnapshot and pushes an undo command if
    // anything actually changed. Only ever fires for the active tab in
    // practice, since a background tab's PdfView isn't interactable.
    void onSessionContentChanged();
    // QTabWidget::currentChanged — refreshes every "current document" UI
    // element (panels, toolbar state, window title, undo group) to match
    // whichever tab just became active.
    void onTabChanged(int index);
    // QTabWidget::tabCloseRequested — prompts to save if that tab has
    // unsaved changes, then removes and destroys it. Closing the last tab
    // leaves the window in its normal empty state rather than quitting.
    void onTabCloseRequested(int index);

private:
    // Window size/position and dock panel layout (which edge each panel is
    // docked to, splitter sizes, hidden/shown) persisted across restarts
    // via QSettings — restoreWindowLayout() is called once, right after the
    // dock widgets exist (buildDockPanels()) and before the window is first
    // shown; saveWindowLayout() runs from closeEvent().
    void restoreWindowLayout();
    void saveWindowLayout();

    void buildActions();
    void buildToolbar();
    void buildMenus();
    void buildStatusBar();
    void buildDockPanels();
    void populateOutline();
    void updateRecentFilesMenu();
    void addToRecentFiles(const QString &path);
    void updateFindStatus();
    void refreshBookmarkIndicators();
    // Reloads tab's content from path in place (same tab, same PdfView) —
    // used for the post-Save reload and Revert to Saved, as opposed to
    // openPathInNewTab()'s "add a new tab" behavior. Returns false (tab
    // left untouched) if the file couldn't be opened.
    bool reloadTabFromPath(DocumentTab *tab, const QString &path);
    bool doSave(DocumentTab *tab, const QString &destPath,
                const QVector<PdfWriter::ExternalInsertion> &insertions = {});
    // True if there's anything Save would actually write for this tab:
    // title/author, page order, bookmarks, outline renames, rotation, or
    // any placed markup (highlight/text box/shape/pasted image).
    bool hasUnsavedChanges(DocumentTab *tab) const;
    // Prompts Save/Discard/Cancel if hasUnsavedChanges(tab); returns true
    // if it's then safe to proceed (saved, discarded, or nothing to
    // lose), false if the user cancelled. Shared by tab-close and
    // whole-window-close.
    bool confirmDiscardUnsavedChanges(DocumentTab *tab);

    // --- Undo/redo (page order + markups; see SessionSnapshot) ---
    // Captures the current state as `before`, lets the caller already have
    // mutated it (direct mutation, same as before undo/redo existed), then
    // diffs against the now-current state and pushes a command if anything
    // changed — simpler than threading "before" state through every call
    // site's own control flow. Also used by onSessionContentChanged() with
    // before = the active tab's lastSnapshot for markup edits PdfView
    // already applied. Always acts on the active tab.
    void pushUndoIfChanged(const QString &text, const SessionSnapshot &before);
    QUndoGroup *m_undoGroup = nullptr;

    // --- Autosave / crash recovery ---
    // Periodically flattens every open tab's in-session state to its own
    // scratch PDF under recoveryDirPath() so a crash doesn't lose
    // everything back to the last real Save. Not a substitute for Save —
    // the recovered file is a separate, fully-flattened copy (markups/
    // rotation baked in, no longer individually editable), meant to be
    // reviewed and then Saved As back over the original once recovered.
    static QString recoveryDirPath();
    // Iterates every open tab and calls writeRecoverySnapshotFor() on each
    // with unsaved changes — connected to m_autosaveTimer.
    void writeRecoverySnapshot();
    void writeRecoverySnapshotFor(DocumentTab *tab);
    // Deletes tab->recoveryFilePath (if any) and forgets it — called
    // whenever that tab's changes are no longer at risk (after a real
    // Save, after the user explicitly discards, or when the tab closes).
    void cleanupRecoveryFile(DocumentTab *tab);
    // Called once at startup: offers to recover any recovery file left
    // behind by a session that didn't exit cleanly (cleanupRecoveryFile()
    // always runs on a clean save/discard/close, so anything still
    // registered here means the app didn't get to do that last time).
    void checkForRecoverableFiles();
    QVector<int> currentPageOrderOrIdentity() const;
    int effectivePageCount() const;
    int currentDisplayPosition() const;
    void goToAdjacentDisplayPage(int delta);
    void upsertOutlineRename(const QVector<int> &path, const QString &newTitle);
    // Rebuilds the Pages panel to match the active tab's pageOrder (or
    // identity order, if empty), rendering each unique source page at
    // most once (cached thereafter) regardless of how many times it
    // appears — e.g. once for a duplicated page's two on-screen copies.
    void refreshThumbnailOrder();
    void copyPageAt(int displayPosition);
    void pastePageAfter(int displayPosition);
    void deletePageAt(int displayPosition);
    // Prompts for a PDF file and a page range, then immediately performs a
    // real Save (like clicking Save) with those pages spliced in after
    // afterDisplayPosition (-1 = at the very beginning). Unlike the other
    // page operations above, this is NOT deferred until Save and is NOT
    // covered by undo — merging in pages from a different file is treated
    // the same as any other Save, not an in-session tweak.
    void insertPagesFromFile(int afterDisplayPosition);

    // Creates a new, empty tab (new PdfView + undo stack, registered in
    // m_tabsByView) and makes it active, but does not load any file into
    // it — callers load the document themselves, then likely remove the
    // tab again on failure. Shared by openPathInNewTab() and the crash-
    // recovery flow.
    DocumentTab *createTab();
    // Removes and destroys tab with NO save/discard prompt — callers
    // (onTabCloseRequested(), closeEvent()) are responsible for calling
    // confirmDiscardUnsavedChanges() first. Safe to call for the last
    // remaining tab, leaving zero tabs open.
    void destroyTab(DocumentTab *tab);
    DocumentTab *currentTab() const;
    DocumentTab *tabForView(PdfView *view) const { return m_tabsByView.value(view); }
    PdfView *currentView() const;
    // Refreshes every piece of shared UI (panels, toolbar/action enabled
    // state, window title, page/zoom labels) to reflect whichever tab is
    // now active — shared by onTabChanged() and onDocumentChanged() (the
    // latter fires when the active tab's own document is replaced in
    // place, e.g. after Save or Revert).
    void refreshForActiveTab();

    QHash<PdfView *, DocumentTab *> m_tabsByView;
    QTabWidget *m_tabWidget = nullptr;

    QWidget *m_findBar = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLabel *m_findCountLabel = nullptr;
    QVector<PdfView::SearchMatch> m_searchMatches;
    int m_searchIndex = -1;
    QLabel *m_pageLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QSpinBox *m_pageSpinBox = nullptr;
    QToolButton *m_prevPageButton = nullptr;
    QToolButton *m_nextPageButton = nullptr;

    ThumbnailPanel *m_thumbnailPanel = nullptr;
    QTreeWidget *m_outlineTree = nullptr;
    FormatPanel *m_formatPanel = nullptr;
    QToolBar *m_formatToolBar = nullptr;
    QDockWidget *m_markupsDock = nullptr;
    QTableWidget *m_markupsTable = nullptr;
    QDockWidget *m_toolboxDock = nullptr;

    QMenu *m_recentFilesMenu = nullptr;

    QAction *m_openAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_selectAllAction = nullptr;
    QAction *m_zoomInAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_zoomActualAction = nullptr;
    QAction *m_zoomFitWidthAction = nullptr;
    QAction *m_selectImageAction = nullptr;
    QAction *m_addTextAction = nullptr;
    QAction *m_rectShapeAction = nullptr;
    QAction *m_roundedRectShapeAction = nullptr;
    QAction *m_ellipseShapeAction = nullptr;
    QAction *m_diamondShapeAction = nullptr;
    QAction *m_lineShapeAction = nullptr;
    QAction *m_arrowShapeAction = nullptr;
    QAction *m_polygonShapeAction = nullptr;
    QAction *m_rotateCwAction = nullptr;
    QAction *m_rotateCcwAction = nullptr;
    QAction *m_printAction = nullptr;
    QAction *m_propertiesAction = nullptr;
    QAction *m_revertAction = nullptr;
    QAction *m_fullScreenAction = nullptr;
    QAction *m_aboutAction = nullptr;
    QAction *m_shortcutsAction = nullptr;
    QAction *m_userManualAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_addBookmarkAction = nullptr;
    QAction *m_addHighlightAction = nullptr;
    QAction *m_toggleBottomPanelsAction = nullptr;
    QAction *m_copyPageAction = nullptr;
    QAction *m_pastePageAction = nullptr;
    QAction *m_duplicatePageAction = nullptr;
    QAction *m_deletePageAction = nullptr;
    QAction *m_insertPagesFromFileAction = nullptr;
    QAction *m_closeTabAction = nullptr;

    QHash<QTreeWidgetItem *, BookmarkTarget> m_bookmarkTargets;

    QTimer *m_autosaveTimer = nullptr;
};
