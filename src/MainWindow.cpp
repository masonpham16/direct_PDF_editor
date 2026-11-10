#include "MainWindow.h"
#include "FormatPanel.h"
#include "PdfView.h"
#include "ThumbnailPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUndoGroup>
#include <QUndoStack>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <functional>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("DirectPDF"));
    resize(1100, 850);
    setAcceptDrops(true);

    m_undoGroup = new QUndoGroup(this);

    auto *container = new QWidget(this);
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    m_findBar = new QWidget(container);
    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(6, 4, 6, 4);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(tr("Find in document..."));
    m_findEdit->installEventFilter(this);
    findLayout->addWidget(m_findEdit, 1);
    m_findCountLabel = new QLabel(m_findBar);
    findLayout->addWidget(m_findCountLabel);
    auto *findPrev = new QToolButton(m_findBar);
    findPrev->setText(tr("↑"));
    findPrev->setToolTip(tr("Previous match"));
    findLayout->addWidget(findPrev);
    auto *findNext = new QToolButton(m_findBar);
    findNext->setText(tr("↓"));
    findNext->setToolTip(tr("Next match"));
    findLayout->addWidget(findNext);
    auto *findClose = new QToolButton(m_findBar);
    findClose->setText(tr("✕"));
    findClose->setToolTip(tr("Close"));
    findLayout->addWidget(findClose);
    m_findBar->setVisible(false);

    connect(m_findEdit, &QLineEdit::textChanged, this, &MainWindow::onFindTextChanged);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &MainWindow::onFindNext);
    connect(findNext, &QToolButton::clicked, this, &MainWindow::onFindNext);
    connect(findPrev, &QToolButton::clicked, this, &MainWindow::onFindPrevious);
    connect(findClose, &QToolButton::clicked, this, &MainWindow::onFindBarClosed);

    m_tabWidget = new QTabWidget(container);
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setDocumentMode(true);
    connect(m_tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    containerLayout->addWidget(m_findBar);
    containerLayout->addWidget(m_tabWidget, 1);
    setCentralWidget(container);

    buildActions();
    buildMenus();
    buildToolbar();
    buildStatusBar();
    buildDockPanels();
    restoreWindowLayout();

    refreshForActiveTab(); // zero tabs open yet — sets the "no document" UI state
    updateRecentFilesMenu();

    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::writeRecoverySnapshot);
    m_autosaveTimer->start(120000); // 2 minutes; writeRecoverySnapshot() no-ops with nothing to protect

    checkForRecoverableFiles();
}

MainWindow::DocumentTab *MainWindow::createTab()
{
    auto *tab = new DocumentTab();
    tab->view = new PdfView(m_tabWidget);
    tab->undoStack = new QUndoStack(this);
    m_undoGroup->addStack(tab->undoStack);
    m_tabsByView.insert(tab->view, tab);

    connect(tab->view, &PdfView::sessionContentChanged, this, &MainWindow::onSessionContentChanged);
    connect(tab->view, &PdfView::zoomChanged, this, &MainWindow::onZoomChanged);
    connect(tab->view, &PdfView::currentPageChanged, this, &MainWindow::onCurrentPageChanged);
    connect(tab->view, &PdfView::documentChanged, this, &MainWindow::onDocumentChanged);
    connect(tab->view, &PdfView::rotationChanged, this, &MainWindow::onRotationChanged);
    connect(tab->view, &PdfView::contextMenuRequestedAt, this, &MainWindow::onContextMenuRequested);
    connect(tab->view, &PdfView::markupsChanged, this, &MainWindow::refreshMarkupsPanel);
    connect(tab->view, &PdfView::selectionModeChanged, this, [this](PdfView::SelectionMode mode) {
        // Keep the toolbar buttons honest even when PdfView changes mode on
        // its own (e.g. Add Text/Add Shape auto-reverting after placing one
        // box/shape).
        const QSignalBlocker blockSelectImage(m_selectImageAction);
        const QSignalBlocker blockAddText(m_addTextAction);
        const QSignalBlocker blockRect(m_rectShapeAction);
        const QSignalBlocker blockRoundedRect(m_roundedRectShapeAction);
        const QSignalBlocker blockEllipse(m_ellipseShapeAction);
        const QSignalBlocker blockDiamond(m_diamondShapeAction);
        const QSignalBlocker blockLine(m_lineShapeAction);
        const QSignalBlocker blockArrow(m_arrowShapeAction);
        const QSignalBlocker blockPolygon(m_polygonShapeAction);
        m_selectImageAction->setChecked(mode == PdfView::SelectionMode::Region);
        m_addTextAction->setChecked(mode == PdfView::SelectionMode::AddText);
        // Which specific shape is armed isn't carried by the mode itself —
        // the action that armed it is already checked (it toggled itself),
        // and QActionGroup exclusivity already unchecked the others. Only
        // need to force them all off once placement finishes.
        if (mode != PdfView::SelectionMode::AddShape) {
            m_rectShapeAction->setChecked(false);
            m_roundedRectShapeAction->setChecked(false);
            m_ellipseShapeAction->setChecked(false);
            m_diamondShapeAction->setChecked(false);
            m_lineShapeAction->setChecked(false);
            m_arrowShapeAction->setChecked(false);
            m_polygonShapeAction->setChecked(false);
        }
    });

    const int index = m_tabWidget->addTab(tab->view, tr("Untitled"));
    m_tabWidget->setCurrentIndex(index);
    return tab;
}

void MainWindow::destroyTab(DocumentTab *tab)
{
    if (!tab) {
        return;
    }
    const int index = m_tabWidget->indexOf(tab->view);
    if (index >= 0) {
        m_tabWidget->removeTab(index); // triggers currentChanged for whatever tab (or none) comes next
    }
    m_tabsByView.remove(tab->view);
    m_undoGroup->removeStack(tab->undoStack);
    tab->view->deleteLater();
    delete tab->undoStack;
    delete tab;
}

MainWindow::DocumentTab *MainWindow::currentTab() const
{
    return m_tabsByView.value(qobject_cast<PdfView *>(m_tabWidget->currentWidget()));
}

PdfView *MainWindow::currentView() const
{
    DocumentTab *tab = currentTab();
    return tab ? tab->view : nullptr;
}

void MainWindow::onTabChanged(int index)
{
    Q_UNUSED(index);
    DocumentTab *tab = currentTab();
    m_undoGroup->setActiveStack(tab ? tab->undoStack : nullptr);
    m_formatPanel->setView(tab ? tab->view : nullptr);
    // Reset transient search state rather than trying to preserve it per
    // tab — simpler, and search is usually a short-lived action anyway.
    m_findBar->setVisible(false);
    m_searchMatches.clear();
    m_searchIndex = -1;
    if (tab) {
        tab->view->clearSearchHighlight();
    }
    refreshForActiveTab();
}

void MainWindow::onTabCloseRequested(int index)
{
    PdfView *view = qobject_cast<PdfView *>(m_tabWidget->widget(index));
    DocumentTab *tab = view ? m_tabsByView.value(view) : nullptr;
    if (!tab) {
        return;
    }
    // Switch to this tab first so the save/discard prompt (and any Save As
    // dialog within it) clearly refers to the document being closed, not
    // whatever else might be active.
    m_tabWidget->setCurrentIndex(index);
    if (confirmDiscardUnsavedChanges(tab)) {
        destroyTab(tab);
    }
}

void MainWindow::buildActions()
{
    m_openAction = new QAction(tr("&Open..."), this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::onOpenTriggered);

    m_closeTabAction = new QAction(tr("&Close Tab"), this);
    m_closeTabAction->setShortcut(QKeySequence::Close);
    connect(m_closeTabAction, &QAction::triggered, this, [this]() {
        if (m_tabWidget->count() > 0) {
            onTabCloseRequested(m_tabWidget->currentIndex());
        }
    });

    m_copyAction = new QAction(tr("&Copy"), this);
    m_copyAction->setShortcut(QKeySequence::Copy);
    // Context-sensitive: Ctrl+C copies a page instead of the normal text/
    // image selection when a thumbnail in the Pages panel currently has
    // keyboard focus — the two features share this shortcut rather than
    // needing a separate, less-discoverable one just for pages.
    connect(m_copyAction, &QAction::triggered, this, [this]() {
        const int focused = m_thumbnailPanel->focusedDisplayPosition();
        if (focused >= 0) {
            copyPageAt(focused);
        } else {
            currentView()->copySelectionToClipboard();
        }
    });

    m_pasteAction = new QAction(tr("&Paste Image"), this);
    m_pasteAction->setShortcut(QKeySequence::Paste);
    m_pasteAction->setToolTip(tr("Paste an image from the clipboard onto the current page"));
    connect(m_pasteAction, &QAction::triggered, this, [this]() {
        const int focused = m_thumbnailPanel->focusedDisplayPosition();
        if (focused >= 0) {
            pastePageAfter(focused);
        } else {
            currentView()->pasteImageFromClipboard();
        }
    });

    m_selectAllAction = new QAction(tr("Select &All"), this);
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    connect(m_selectAllAction, &QAction::triggered, this, [this]() { currentView()->selectAllOnPage(); });

    m_selectImageAction = new QAction(tr("Select Image/Region"), this);
    m_selectImageAction->setCheckable(true);
    m_selectImageAction->setToolTip(tr("Drag a rectangle to select and copy part of the page as an image"));
    connect(m_selectImageAction, &QAction::toggled, this, [this](bool checked) {
        currentView()->setSelectionMode(checked ? PdfView::SelectionMode::Region : PdfView::SelectionMode::Text);
    });

    m_addTextAction = new QAction(tr("Add Text"), this);
    m_addTextAction->setCheckable(true);
    m_addTextAction->setShortcut(tr("Ctrl+T"));
    m_addTextAction->setToolTip(tr("Click on the page to place an editable text box"));
    connect(m_addTextAction, &QAction::toggled, this, [this](bool checked) {
        currentView()->setSelectionMode(checked ? PdfView::SelectionMode::AddText : PdfView::SelectionMode::Text);
    });

    m_rectShapeAction = new QAction(tr("Rectangle"), this);
    m_rectShapeAction->setCheckable(true);
    m_rectShapeAction->setToolTip(tr("Drag on the page to place a rectangle"));
    connect(m_rectShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingShape(PdfView::ShapeKind::Rectangle);
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    m_ellipseShapeAction = new QAction(tr("Ellipse"), this);
    m_ellipseShapeAction->setCheckable(true);
    m_ellipseShapeAction->setToolTip(tr("Drag on the page to place an ellipse/circle"));
    connect(m_ellipseShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingShape(PdfView::ShapeKind::Ellipse);
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    m_diamondShapeAction = new QAction(tr("Diamond"), this);
    m_diamondShapeAction->setCheckable(true);
    m_diamondShapeAction->setToolTip(tr("Drag on the page to place a diamond"));
    connect(m_diamondShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingShape(PdfView::ShapeKind::Diamond);
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    m_roundedRectShapeAction = new QAction(tr("Rounded Rectangle"), this);
    m_roundedRectShapeAction->setCheckable(true);
    m_roundedRectShapeAction->setToolTip(tr("Drag on the page to place a rounded-corner rectangle"));
    connect(m_roundedRectShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingShape(PdfView::ShapeKind::RoundedRectangle);
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    m_lineShapeAction = new QAction(tr("Line"), this);
    m_lineShapeAction->setCheckable(true);
    m_lineShapeAction->setToolTip(tr("Drag on the page to draw a line"));
    connect(m_lineShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingShape(PdfView::ShapeKind::Line);
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    m_arrowShapeAction = new QAction(tr("Arrow"), this);
    m_arrowShapeAction->setCheckable(true);
    m_arrowShapeAction->setToolTip(tr("Drag on the page to draw an arrow (points toward where you release)"));
    connect(m_arrowShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingShape(PdfView::ShapeKind::Arrow);
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    m_polygonShapeAction = new QAction(tr("Polygon"), this);
    m_polygonShapeAction->setCheckable(true);
    m_polygonShapeAction->setToolTip(
        tr("Click each vertex in turn, then double-click or press Enter to close the shape (Esc to cancel)"));
    connect(m_polygonShapeAction, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            currentView()->startPlacingPolygon();
        } else {
            currentView()->setSelectionMode(PdfView::SelectionMode::Text);
        }
    });

    // At most one placement tool active at a time; clicking the active one
    // again turns it off (back to normal text selection).
    auto *toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    toolGroup->addAction(m_selectImageAction);
    toolGroup->addAction(m_addTextAction);
    toolGroup->addAction(m_rectShapeAction);
    toolGroup->addAction(m_roundedRectShapeAction);
    toolGroup->addAction(m_ellipseShapeAction);
    toolGroup->addAction(m_diamondShapeAction);
    toolGroup->addAction(m_lineShapeAction);
    toolGroup->addAction(m_arrowShapeAction);
    toolGroup->addAction(m_polygonShapeAction);

    m_zoomInAction = new QAction(tr("Zoom In"), this);
    m_zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(m_zoomInAction, &QAction::triggered, this, [this]() { currentView()->zoomIn(); });

    m_zoomOutAction = new QAction(tr("Zoom Out"), this);
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(m_zoomOutAction, &QAction::triggered, this, [this]() { currentView()->zoomOut(); });

    m_zoomActualAction = new QAction(tr("Actual Size"), this);
    m_zoomActualAction->setShortcut(tr("Ctrl+0"));
    connect(m_zoomActualAction, &QAction::triggered, this, [this]() { currentView()->zoomToActualSize(); });

    m_zoomFitWidthAction = new QAction(tr("Fit Width"), this);
    m_zoomFitWidthAction->setShortcut(tr("Ctrl+9"));
    connect(m_zoomFitWidthAction, &QAction::triggered, this, [this]() { currentView()->zoomToFitWidth(); });

    m_rotateCwAction = new QAction(tr("Rotate Clockwise"), this);
    m_rotateCwAction->setShortcut(tr("Ctrl+]"));
    connect(m_rotateCwAction, &QAction::triggered, this, [this]() { currentView()->rotateClockwise(); });

    m_rotateCcwAction = new QAction(tr("Rotate Counterclockwise"), this);
    m_rotateCcwAction->setShortcut(tr("Ctrl+["));
    connect(m_rotateCcwAction, &QAction::triggered, this, [this]() { currentView()->rotateCounterclockwise(); });

    m_printAction = new QAction(tr("&Print..."), this);
    m_printAction->setShortcut(QKeySequence::Print);
    connect(m_printAction, &QAction::triggered, this, &MainWindow::onPrintTriggered);

    m_propertiesAction = new QAction(tr("&Properties..."), this);
    connect(m_propertiesAction, &QAction::triggered, this, &MainWindow::onPropertiesTriggered);

    m_revertAction = new QAction(tr("Revert to Saved"), this);
    connect(m_revertAction, &QAction::triggered, this, &MainWindow::onRevertTriggered);

    m_fullScreenAction = new QAction(tr("Full Screen"), this);
    m_fullScreenAction->setCheckable(true);
    m_fullScreenAction->setShortcut(tr("Ctrl+Shift+F"));
    connect(m_fullScreenAction, &QAction::toggled, this, &MainWindow::onFullScreenToggled);

    m_aboutAction = new QAction(tr("&About DirectPDF"), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);

    m_shortcutsAction = new QAction(tr("&Keyboard Shortcuts"), this);
    connect(m_shortcutsAction, &QAction::triggered, this, &MainWindow::onShortcutsTriggered);

    m_userManualAction = new QAction(tr("&User Manual"), this);
    m_userManualAction->setShortcut(QKeySequence::HelpContents);
    connect(m_userManualAction, &QAction::triggered, this, &MainWindow::onUserManualTriggered);

    m_findAction = new QAction(tr("&Find..."), this);
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, &MainWindow::onFindTriggered);

    m_saveAction = new QAction(tr("&Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::onSaveTriggered);

    m_saveAsAction = new QAction(tr("Save &As..."), this);
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::onSaveAsTriggered);

    m_addBookmarkAction = new QAction(tr("&Add Bookmark"), this);
    m_addBookmarkAction->setShortcut(tr("Ctrl+D"));
    m_addBookmarkAction->setToolTip(tr("Bookmark the current page (or selected text) for quick access"));
    connect(m_addBookmarkAction, &QAction::triggered, this, &MainWindow::onAddBookmarkTriggered);

    m_addHighlightAction = new QAction(tr("&Highlight"), this);
    m_addHighlightAction->setShortcut(tr("Ctrl+H"));
    m_addHighlightAction->setToolTip(tr("Highlight the selected text and list it in the Markups panel"));
    connect(m_addHighlightAction, &QAction::triggered, this, &MainWindow::onAddHighlightTriggered);

    // A single master switch for the whole bottom panel group (Pages/
    // Bookmarks/Markups/Tools). Created here (rather than in
    // buildDockPanels(), where its toggled-state wiring to the actual dock
    // widgets happens) so buildStatusBar() — which runs first — has an
    // action to bind its always-visible status bar button to.
    m_toggleBottomPanelsAction = new QAction(tr("▾ Panel"), this);
    m_toggleBottomPanelsAction->setCheckable(true);
    m_toggleBottomPanelsAction->setChecked(true);
    m_toggleBottomPanelsAction->setShortcut(tr("Ctrl+Shift+B"));
    m_toggleBottomPanelsAction->setToolTip(tr("Show/hide the Pages/Bookmarks/Markups/Tools panel"));

    // Page management, always acting on whichever page is currently open
    // in the main view (unlike Ctrl+C/Ctrl+V above, which act on whichever
    // thumbnail has focus) — a menu-reachable way to manage pages that
    // doesn't depend on the Pages panel having focus. No shortcuts of
    // their own, to avoid clashing with Ctrl+C/Ctrl+V's existing meanings;
    // the Pages panel's own right-click menu and Delete/Backspace key
    // remain the quicker path for mouse/keyboard-driven page management.
    m_copyPageAction = new QAction(tr("Copy Page"), this);
    connect(m_copyPageAction, &QAction::triggered, this, [this]() { copyPageAt(currentDisplayPosition()); });

    m_pastePageAction = new QAction(tr("Paste Page"), this);
    connect(m_pastePageAction, &QAction::triggered, this, [this]() { pastePageAfter(currentDisplayPosition()); });

    m_duplicatePageAction = new QAction(tr("Duplicate Page"), this);
    connect(m_duplicatePageAction, &QAction::triggered, this, [this]() {
        const int pos = currentDisplayPosition();
        if (pos < 0) {
            return;
        }
        copyPageAt(pos);
        pastePageAfter(pos);
    });

    m_deletePageAction = new QAction(tr("Delete Page"), this);
    connect(m_deletePageAction, &QAction::triggered, this, [this]() { deletePageAt(currentDisplayPosition()); });

    m_insertPagesFromFileAction = new QAction(tr("Insert Pages from File..."), this);
    connect(m_insertPagesFromFileAction, &QAction::triggered, this,
            [this]() { insertPagesFromFile(currentDisplayPosition()); });
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_openAction);
    m_recentFilesMenu = fileMenu->addMenu(tr("Open &Recent"));
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_saveAction);
    fileMenu->addAction(m_saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_revertAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_printAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_propertiesAction);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction *undoAction = m_undoGroup->createUndoAction(this, tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    QAction *redoAction = m_undoGroup->createRedoAction(this, tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(undoAction);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    editMenu->addAction(m_copyAction);
    editMenu->addAction(m_pasteAction);
    editMenu->addAction(m_selectAllAction);
    editMenu->addSeparator();
    editMenu->addAction(m_findAction);
    editMenu->addSeparator();
    editMenu->addAction(m_selectImageAction);
    editMenu->addAction(m_addTextAction);
    editMenu->addAction(m_rectShapeAction);
    editMenu->addAction(m_roundedRectShapeAction);
    editMenu->addAction(m_ellipseShapeAction);
    editMenu->addAction(m_diamondShapeAction);
    editMenu->addAction(m_lineShapeAction);
    editMenu->addAction(m_arrowShapeAction);
    editMenu->addAction(m_polygonShapeAction);
    editMenu->addSeparator();
    editMenu->addAction(m_addBookmarkAction);
    editMenu->addAction(m_addHighlightAction);
    editMenu->addSeparator();
    editMenu->addAction(m_copyPageAction);
    editMenu->addAction(m_pastePageAction);
    editMenu->addAction(m_duplicatePageAction);
    editMenu->addAction(m_deletePageAction);
    editMenu->addAction(m_insertPagesFromFileAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_zoomInAction);
    viewMenu->addAction(m_zoomOutAction);
    viewMenu->addAction(m_zoomActualAction);
    viewMenu->addAction(m_zoomFitWidthAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_rotateCwAction);
    viewMenu->addAction(m_rotateCcwAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_fullScreenAction);
    viewMenu->addSeparator();
    // Panel toggle actions are added once the docks exist, in buildDockPanels().

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_userManualAction);
    helpMenu->addAction(m_shortcutsAction);
    helpMenu->addSeparator();
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::buildToolbar()
{
    QToolBar *toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->addAction(m_openAction);
    toolbar->addAction(m_saveAction);
    toolbar->addAction(m_saveAsAction);
    toolbar->addAction(m_printAction);
    toolbar->addSeparator();
    toolbar->addAction(m_zoomOutAction);
    toolbar->addAction(m_zoomInAction);
    toolbar->addAction(m_zoomActualAction);
    toolbar->addAction(m_zoomFitWidthAction);
    toolbar->addSeparator();
    toolbar->addAction(m_rotateCcwAction);
    toolbar->addAction(m_rotateCwAction);
    toolbar->addSeparator();
    toolbar->addAction(m_addBookmarkAction);
    toolbar->addAction(m_addHighlightAction);

    // A second toolbar row, directly below the main one, spreading the
    // Format controls out horizontally instead of stacking them in a narrow
    // side panel — much easier to see and adjust everything for whichever
    // text box or shape is currently selected at a glance.
    addToolBarBreak();
    m_formatToolBar = addToolBar(tr("Format"));
    m_formatToolBar->setObjectName("FormatToolBar");
    m_formatToolBar->setMovable(false);
    m_formatPanel = new FormatPanel(nullptr, m_formatToolBar);
    m_formatToolBar->addWidget(m_formatPanel);
}

void MainWindow::buildStatusBar()
{
    // A container with stretches on both sides, added as the status bar's
    // only non-permanent widget with stretch=1, so the page label + go-to
    // box sit centered in the space left of the (right-aligned) zoom label.
    auto *pageGroup = new QWidget(this);
    auto *pageGroupLayout = new QHBoxLayout(pageGroup);
    pageGroupLayout->setContentsMargins(0, 0, 0, 0);
    pageGroupLayout->addStretch(1);

    m_pageLabel = new QLabel(this);
    pageGroupLayout->addWidget(m_pageLabel);

    pageGroupLayout->addSpacing(10);
    pageGroupLayout->addWidget(new QLabel(tr("Go to:"), this));

    m_prevPageButton = new QToolButton(this);
    m_prevPageButton->setText(tr("◀")); // ◀
    m_prevPageButton->setAutoRaise(true);
    m_prevPageButton->setToolTip(tr("Previous Page"));
    m_prevPageButton->setEnabled(false);
    connect(m_prevPageButton, &QToolButton::clicked, this, [this]() { goToAdjacentDisplayPage(-1); });
    pageGroupLayout->addWidget(m_prevPageButton);

    m_pageSpinBox = new QSpinBox(this);
    m_pageSpinBox->setMinimum(1);
    m_pageSpinBox->setMaximum(1);
    // The ◀ / ▶ buttons flanking this box already cover step-by-one
    // navigation, so the spin box's own up/down arrows are redundant.
    m_pageSpinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_pageSpinBox->setEnabled(false);
    connect(m_pageSpinBox, &QSpinBox::editingFinished, this, &MainWindow::onGoToPageRequested);
    pageGroupLayout->addWidget(m_pageSpinBox);

    m_nextPageButton = new QToolButton(this);
    m_nextPageButton->setText(tr("▶")); // ▶
    m_nextPageButton->setAutoRaise(true);
    m_nextPageButton->setToolTip(tr("Next Page"));
    m_nextPageButton->setEnabled(false);
    connect(m_nextPageButton, &QToolButton::clicked, this, [this]() { goToAdjacentDisplayPage(1); });
    pageGroupLayout->addWidget(m_nextPageButton);

    pageGroupLayout->addStretch(1);

    statusBar()->addWidget(pageGroup, 1);

    // Stays in the status bar regardless of whether the bottom panel group
    // itself is currently shown or hidden — a permanent, always-visible
    // reminder that it's just tucked away, not gone, and a one-click way to
    // bring it back.
    auto *bottomPanelButton = new QToolButton(this);
    bottomPanelButton->setDefaultAction(m_toggleBottomPanelsAction);
    bottomPanelButton->setAutoRaise(true);
    statusBar()->addPermanentWidget(bottomPanelButton);

    m_zoomLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_zoomLabel);
}

void MainWindow::buildDockPanels()
{
    // Docked along the bottom edge rather than the left (an experiment —
    // drag any of these tabs' title bar elsewhere if you'd rather have them
    // back on the side; nothing here pins them in place).
    auto *thumbDock = new QDockWidget(tr("Pages"), this);
    thumbDock->setObjectName("ThumbnailDock");
    m_thumbnailPanel = new ThumbnailPanel(thumbDock);
    // Matches the panel's default dock edge (bottom); kept in sync with
    // whatever edge it's actually docked to below.
    m_thumbnailPanel->setOrientation(Qt::Horizontal);
    thumbDock->setWidget(m_thumbnailPanel);
    addDockWidget(Qt::BottomDockWidgetArea, thumbDock);
    connect(m_thumbnailPanel, &ThumbnailPanel::pageActivated, this, &MainWindow::onThumbnailActivated);
    connect(m_thumbnailPanel, &ThumbnailPanel::reorderRequested, this, &MainWindow::onThumbnailReorderRequested);
    connect(m_thumbnailPanel, &ThumbnailPanel::adjacentPageRequested, this,
            [this](int delta) { goToAdjacentDisplayPage(delta); });
    connect(m_thumbnailPanel, &ThumbnailPanel::deleteRequested, this, &MainWindow::onThumbnailDeleteRequested);
    connect(m_thumbnailPanel, &ThumbnailPanel::copyRequested, this, &MainWindow::onThumbnailCopyRequested);
    connect(m_thumbnailPanel, &ThumbnailPanel::pasteRequested, this, &MainWindow::onThumbnailPasteRequested);
    connect(m_thumbnailPanel, &ThumbnailPanel::contextMenuRequested, this, &MainWindow::onThumbnailContextMenu);
    connect(thumbDock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        const bool horizontal = (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea);
        m_thumbnailPanel->setOrientation(horizontal ? Qt::Horizontal : Qt::Vertical);
    });

    auto *outlineDock = new QDockWidget(tr("Bookmarks"), this);
    outlineDock->setObjectName("OutlineDock");
    m_outlineTree = new QTreeWidget(outlineDock);
    m_outlineTree->setHeaderHidden(true);
    // Only F2/context-menu triggers editing, not double-click — double-click
    // already means "jump to this bookmark" and the two would otherwise
    // fire together.
    m_outlineTree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_outlineTree->setContextMenuPolicy(Qt::CustomContextMenu);
    outlineDock->setWidget(m_outlineTree);
    addDockWidget(Qt::BottomDockWidgetArea, outlineDock);
    connect(m_outlineTree, &QTreeWidget::itemActivated, this, &MainWindow::onOutlineItemActivated);
    connect(m_outlineTree, &QTreeWidget::itemChanged, this, &MainWindow::onOutlineItemChanged);
    connect(m_outlineTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onOutlineContextMenu);

    tabifyDockWidget(thumbDock, outlineDock);
    thumbDock->raise();

    // Markups: every placed object (highlight, text box, or shape) in one
    // list — so there's a single place to see everything on the document,
    // jump straight to any of it, and leave yourself a note, the same way
    // Bluebeam Revu's markups list works.
    m_markupsDock = new QDockWidget(tr("Markups"), this);
    m_markupsDock->setObjectName("MarkupsDock");
    m_markupsTable = new QTableWidget(0, 4, m_markupsDock);
    m_markupsTable->setHorizontalHeaderLabels({tr("Type"), tr("Page"), tr("Description"), tr("Note")});
    m_markupsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_markupsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_markupsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_markupsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_markupsTable->verticalHeader()->setVisible(false);
    m_markupsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_markupsTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_markupsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_markupsTable->setToolTip(tr("Click a row to jump to that item; double-click the Note cell to edit it"));
    m_markupsDock->setWidget(m_markupsTable);
    addDockWidget(Qt::BottomDockWidgetArea, m_markupsDock);
    tabifyDockWidget(outlineDock, m_markupsDock);
    thumbDock->raise();
    connect(m_markupsTable, &QTableWidget::cellClicked, this, &MainWindow::onMarkupCellClicked);
    connect(m_markupsTable, &QTableWidget::itemChanged, this, &MainWindow::onMarkupNoteEdited);
    connect(m_markupsTable, &QTableWidget::customContextMenuRequested, this, &MainWindow::onMarkupsContextMenu);

    // Tools: the placement-tool buttons (Select Image/Region, Add Text, the
    // shape tools), tabbed in alongside Pages/Bookmarks/Markups rather than
    // living in their own separate toolbar. Built from the same
    // QAction objects the Edit menu and right-click menu use, via
    // setDefaultAction(), so checked/enabled state stays in sync for free.
    m_toolboxDock = new QDockWidget(tr("Tools"), this);
    m_toolboxDock->setObjectName("ToolboxDock");
    auto *toolboxContent = new QWidget(m_toolboxDock);
    auto *toolboxLayout = new QHBoxLayout(toolboxContent);
    toolboxLayout->setContentsMargins(6, 6, 6, 6);
    toolboxLayout->setSpacing(4);
    const auto addToolboxButton = [toolboxContent, toolboxLayout](QAction *action) {
        auto *button = new QToolButton(toolboxContent);
        button->setDefaultAction(action);
        toolboxLayout->addWidget(button);
    };
    addToolboxButton(m_selectImageAction);
    addToolboxButton(m_addTextAction);
    toolboxLayout->addSpacing(8);
    addToolboxButton(m_rectShapeAction);
    addToolboxButton(m_roundedRectShapeAction);
    addToolboxButton(m_ellipseShapeAction);
    addToolboxButton(m_diamondShapeAction);
    toolboxLayout->addSpacing(8);
    addToolboxButton(m_lineShapeAction);
    addToolboxButton(m_arrowShapeAction);
    addToolboxButton(m_polygonShapeAction);
    toolboxLayout->addStretch(1);

    // Scroll-wrapped so this dock's content can't force the whole tabbed
    // group wider than the Pages tab needs.
    auto *toolboxScroll = new QScrollArea(m_toolboxDock);
    toolboxScroll->setWidget(toolboxContent);
    toolboxScroll->setWidgetResizable(true);
    toolboxScroll->setFrameShape(QFrame::NoFrame);
    toolboxScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolboxDock->setWidget(toolboxScroll);
    addDockWidget(Qt::BottomDockWidgetArea, m_toolboxDock);
    tabifyDockWidget(m_markupsDock, m_toolboxDock);
    thumbDock->raise();

    // Wire up the master switch created in buildActions() now that the
    // actual dock widgets it controls exist. The arrow in its label flips
    // to hint that clicking it again brings the panel back, since its
    // status-bar button (see buildStatusBar()) is the one place this
    // control stays visible even while the panel itself is hidden.
    connect(m_toggleBottomPanelsAction, &QAction::toggled, this, [this, thumbDock, outlineDock](bool checked) {
        thumbDock->setVisible(checked);
        outlineDock->setVisible(checked);
        m_markupsDock->setVisible(checked);
        m_toolboxDock->setVisible(checked);
        m_toggleBottomPanelsAction->setText(checked ? tr("▾ Panel") : tr("▴ Panel"));
    });

    for (QAction *action : menuBar()->actions()) {
        if (action->menu() && action->menu()->title() == tr("&View")) {
            action->menu()->addAction(m_toggleBottomPanelsAction);
            action->menu()->addSeparator();
            action->menu()->addAction(thumbDock->toggleViewAction());
            action->menu()->addAction(outlineDock->toggleViewAction());
            action->menu()->addAction(m_markupsDock->toggleViewAction());
            action->menu()->addAction(m_toolboxDock->toggleViewAction());
            action->menu()->addAction(m_formatToolBar->toggleViewAction());
            break;
        }
    }
}

namespace {
QString firstPdfPath(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls()) {
        return {};
    }
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) {
            return url.toLocalFile();
        }
    }
    return {};
}
} // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!firstPdfPath(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QString path = firstPdfPath(event->mimeData());
    if (!path.isEmpty()) {
        openPathInNewTab(path);
        event->acceptProposedAction();
    }
}

void MainWindow::onOpenTriggered()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open PDF"), QString(), tr("PDF Files (*.pdf)"));
    if (!path.isEmpty()) {
        openPathInNewTab(path);
    }
}

MainWindow::DocumentTab *MainWindow::openPathInNewTab(const QString &path)
{
    DocumentTab *tab = createTab();
    if (reloadTabFromPath(tab, path)) {
        return tab;
    }
    destroyTab(tab);
    return nullptr;
}

bool MainWindow::reloadTabFromPath(DocumentTab *tab, const QString &path)
{
    if (!tab) {
        return false;
    }
    // Opening any document into this tab — a freshly-chosen one, or this
    // same file reloading after a successful Save — means whatever the
    // PREVIOUS content's recovery file was protecting is no longer at
    // risk.
    cleanupRecoveryFile(tab);

    QString error;
    if (!tab->view->openFile(path, &error)) {
        QMessageBox::warning(this, tr("Could not open PDF"), error);
        return false;
    }
    tab->currentFilePath = path;
    addToRecentFiles(path);

    tab->pageOrder.clear();
    tab->pendingBookmarks.clear();
    tab->pendingTitle.clear();
    tab->pendingAuthor.clear();
    tab->pendingRenames.clear();
    tab->thumbnailImageCache.clear();
    tab->copiedSourcePage = -1;

    // tab->view->openFile() above already fired documentChanged()
    // synchronously if this is the active tab, before labels for *this*
    // document could be loaded — set them now and refresh once more so the
    // Pages panel picks them up (refreshForActiveTab() is a no-op harmless
    // re-run if this tab isn't actually the active one).
    if (tab == currentTab()) {
        m_thumbnailPanel->setInitialLabels(PdfWriter::readPageLabels(path));
        refreshForActiveTab();
    }

    // Undo history doesn't carry across documents — tab->view->openFile()
    // above already reset every highlight/text box/shape/pasted image, so
    // this new baseline is just the fresh, empty state. captureSessionSnapshot()
    // operates on currentTab() implicitly, which is correct here: both call
    // sites (openPathInNewTab via createTab(), and doSave()/onRevertTriggered,
    // which only ever reload the already-active tab) always have tab ==
    // currentTab() by this point.
    tab->undoStack->clear();
    tab->lastSnapshot = captureSessionSnapshot();
    return true;
}

void MainWindow::onZoomChanged(double zoom)
{
    m_zoomLabel->setText(tr("%1%").arg(qRound(zoom * 100)));
}

void MainWindow::onCurrentPageChanged(int pageIndex)
{
    // "Page N" everywhere in the UI (this label, the Go to box, the arrows,
    // the Pages panel's thumbnail numbers) means *display* position — i.e.
    // whatever order the Pages panel currently shows, after any drag/typed
    // reorder — not the page's original/source index. Otherwise the number
    // shown here would jump around (1, 3, 2, 4...) even though the Pages
    // panel itself numbers things sequentially post-reorder.
    const QVector<int> order = currentPageOrderOrIdentity();
    const int pos = order.indexOf(pageIndex);
    const int displayNumber = pos >= 0 ? pos + 1 : pageIndex + 1;

    m_pageLabel->setText(tr("Page %1 of %2").arg(displayNumber).arg(order.size()));
    if (m_pageSpinBox) {
        const QSignalBlocker blocker(m_pageSpinBox);
        m_pageSpinBox->setMaximum(order.size());
        m_pageSpinBox->setValue(displayNumber);
    }
    if (m_thumbnailPanel) {
        m_thumbnailPanel->setCurrentSourcePage(pageIndex);
    }
    m_prevPageButton->setEnabled(pos > 0);
    m_nextPageButton->setEnabled(pos >= 0 && pos < order.size() - 1);
}

void MainWindow::onDocumentChanged()
{
    // documentChanged() only fires for whichever tab's view is actually
    // interactable, i.e. the active one, so this is always in sync with
    // the active tab — safe to just re-run the same full refresh.
    refreshForActiveTab();
}

void MainWindow::refreshForActiveTab()
{
    DocumentTab *tab = currentTab();
    const bool loaded = tab && tab->view->isDocumentLoaded();

    setWindowTitle(loaded ? tr("%1 - DirectPDF").arg(QFileInfo(tab->currentFilePath).fileName()) : tr("DirectPDF"));
    if (tab) {
        const int index = m_tabWidget->indexOf(tab->view);
        if (index >= 0) {
            const QString name = QFileInfo(tab->currentFilePath).fileName();
            m_tabWidget->setTabText(index, name.isEmpty() ? tr("Untitled") : name);
            m_tabWidget->setTabToolTip(index, tab->currentFilePath);
        }
    }

    m_closeTabAction->setEnabled(tab != nullptr);
    m_copyAction->setEnabled(loaded);
    m_pasteAction->setEnabled(loaded);
    m_selectAllAction->setEnabled(loaded);
    m_zoomInAction->setEnabled(loaded);
    m_zoomOutAction->setEnabled(loaded);
    m_zoomActualAction->setEnabled(loaded);
    m_zoomFitWidthAction->setEnabled(loaded);
    m_selectImageAction->setEnabled(loaded);
    m_addTextAction->setEnabled(loaded);
    m_rectShapeAction->setEnabled(loaded);
    m_roundedRectShapeAction->setEnabled(loaded);
    m_ellipseShapeAction->setEnabled(loaded);
    m_diamondShapeAction->setEnabled(loaded);
    m_lineShapeAction->setEnabled(loaded);
    m_arrowShapeAction->setEnabled(loaded);
    m_polygonShapeAction->setEnabled(loaded);
    m_rotateCwAction->setEnabled(loaded);
    m_rotateCcwAction->setEnabled(loaded);
    m_printAction->setEnabled(loaded);
    m_propertiesAction->setEnabled(loaded);
    m_revertAction->setEnabled(loaded);
    m_findAction->setEnabled(loaded);
    m_saveAction->setEnabled(loaded);
    m_saveAsAction->setEnabled(loaded);
    m_addBookmarkAction->setEnabled(loaded);
    m_addHighlightAction->setEnabled(loaded);
    m_copyPageAction->setEnabled(loaded);
    m_pastePageAction->setEnabled(loaded);
    m_duplicatePageAction->setEnabled(loaded);
    m_deletePageAction->setEnabled(loaded);
    m_insertPagesFromFileAction->setEnabled(loaded);
    m_pageLabel->setText(loaded ? tr("Page %1 of %2").arg(1).arg(effectivePageCount()) : tr("No document"));

    m_findBar->setVisible(false);
    m_searchMatches.clear();
    m_searchIndex = -1;

    m_pageSpinBox->setEnabled(loaded);
    m_pageSpinBox->setMaximum(loaded ? effectivePageCount() : 1);
    m_prevPageButton->setEnabled(false);
    m_nextPageButton->setEnabled(false);
    {
        const QSignalBlocker blocker(m_pageSpinBox);
        m_pageSpinBox->setValue(1);
    }

    if (loaded) {
        refreshThumbnailOrder();
        populateOutline();
    } else {
        m_thumbnailPanel->clear();
        m_outlineTree->clear();
    }
    refreshMarkupsPanel();
    refreshBookmarkIndicators();
}

void MainWindow::refreshBookmarkIndicators()
{
    QSet<int> pages;
    for (auto it = m_bookmarkTargets.constBegin(); it != m_bookmarkTargets.constEnd(); ++it) {
        if (it.value().page >= 0) {
            pages.insert(it.value().page);
        }
    }
    if (DocumentTab *tab = currentTab()) {
        tab->view->setBookmarkedPages(pages);
    }
    m_thumbnailPanel->setBookmarkedPages(pages);
}

void MainWindow::onRotationChanged(int quarterTurns)
{
    Q_UNUSED(quarterTurns);
    // Rotating invalidates cached thumbnails (rendered at the old
    // orientation), so refresh them to match.
    DocumentTab *tab = currentTab();
    if (tab && tab->view->isDocumentLoaded()) {
        tab->thumbnailImageCache.clear();
        refreshThumbnailOrder();
    }
}

void MainWindow::onContextMenuRequested(const QPoint &globalPos)
{
    QMenu menu(this);
    PdfView *view = currentView();

    if (view->hasImageSelection()) {
        QAction *copyAction = menu.addAction(tr("Copy Image"));
        connect(copyAction, &QAction::triggered, view, &PdfView::copySelectionToClipboard);
    } else {
        QAction *copyAction = menu.addAction(tr("Copy"));
        copyAction->setEnabled(view->hasTextSelection());
        connect(copyAction, &QAction::triggered, view, &PdfView::copySelectionToClipboard);
    }
    menu.addAction(m_selectAllAction);
    menu.addSeparator();
    menu.addAction(m_addBookmarkAction);
    m_addHighlightAction->setEnabled(view->hasTextSelection());
    menu.addAction(m_addHighlightAction);
    menu.addSeparator();
    menu.addAction(m_zoomInAction);
    menu.addAction(m_zoomOutAction);
    menu.addAction(m_zoomFitWidthAction);
    menu.addSeparator();
    menu.addAction(m_rotateCwAction);
    menu.addAction(m_rotateCcwAction);

    menu.exec(globalPos);
}

void MainWindow::onGoToPageRequested()
{
    // The spin box's value is a *display* position (see onCurrentPageChanged)
    // — map it back to the corresponding source page index to actually jump.
    const QVector<int> order = currentPageOrderOrIdentity();
    const int pos = qBound(0, m_pageSpinBox->value() - 1, order.size() - 1);
    currentView()->goToPage(order[pos]);
}

void MainWindow::onPrintTriggered()
{
    DocumentTab *tab = currentTab();
    if (!tab || !tab->view->isDocumentLoaded()) {
        return;
    }
    PdfView *view = tab->view;

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(QFileInfo(tab->currentFilePath).fileName());
    printer.setFromTo(1, view->pageCount());

    QPrintDialog dialog(&printer, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const int first = printer.fromPage() > 0 ? printer.fromPage() - 1 : 0;
    const int last = printer.toPage() > 0 ? printer.toPage() - 1 : view->pageCount() - 1;

    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::warning(this, tr("Print"), tr("Could not start printing."));
        return;
    }

    const double dpi = printer.resolution();
    bool firstPage = true;
    for (int p = first; p <= last; ++p) {
        if (!firstPage) {
            printer.newPage();
        }
        firstPage = false;
        const QImage img = view->document().renderPage(p, dpi);
        painter.drawImage(QPoint(0, 0), img);
    }
    painter.end();
}

void MainWindow::onPropertiesTriggered()
{
    DocumentTab *tab = currentTab();
    if (!tab || !tab->view->isDocumentLoaded()) {
        return;
    }
    const PdfDocument::DocumentInfo info = tab->view->document().documentInfo();
    const QFileInfo fileInfo(tab->currentFilePath);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Document Properties"));
    auto *form = new QFormLayout(&dialog);

    auto *titleEdit = new QLineEdit(tab->pendingTitle.isEmpty() ? info.title : tab->pendingTitle, &dialog);
    auto *authorEdit = new QLineEdit(tab->pendingAuthor.isEmpty() ? info.author : tab->pendingAuthor, &dialog);
    form->addRow(tr("Title:"), titleEdit);
    form->addRow(tr("Author:"), authorEdit);

    const auto orNone = [](const QString &s) { return s.isEmpty() ? tr("(none)") : s; };
    form->addRow(tr("Subject:"), new QLabel(orNone(info.subject), &dialog));
    form->addRow(tr("Creator:"), new QLabel(orNone(info.creator), &dialog));
    form->addRow(tr("Producer:"), new QLabel(orNone(info.producer), &dialog));
    form->addRow(tr("Pages:"), new QLabel(QString::number(info.pageCount), &dialog));
    form->addRow(tr("PDF Version:"), new QLabel(tr("%1.%2").arg(info.versionMajor).arg(info.versionMinor), &dialog));
    form->addRow(tr("File Size:"), new QLabel(QLocale().formattedDataSize(fileInfo.size()), &dialog));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        const QString newTitle = titleEdit->text().trimmed();
        const QString newAuthor = authorEdit->text().trimmed();
        if (!newTitle.isEmpty() && newTitle != info.title) {
            tab->pendingTitle = newTitle;
        }
        if (!newAuthor.isEmpty() && newAuthor != info.author) {
            tab->pendingAuthor = newAuthor;
        }
    }
}

void MainWindow::onThumbnailActivated(int sourcePageIndex)
{
    if (sourcePageIndex >= 0) {
        currentView()->goToPage(sourcePageIndex);
    }
}

void MainWindow::onThumbnailReorderRequested(int sourceDisplayPosition, int targetDisplayPosition, bool after)
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    // Positions, not source page indices — a duplicated page has two
    // thumbnails sharing one source index, so position is what
    // unambiguously identifies which occurrence was actually dragged.
    const SessionSnapshot before = captureSessionSnapshot();
    tab->pageOrder = currentPageOrderOrIdentity();
    if (sourceDisplayPosition < 0 || sourceDisplayPosition >= tab->pageOrder.size() || targetDisplayPosition < 0
        || targetDisplayPosition >= tab->pageOrder.size() || sourceDisplayPosition == targetDisplayPosition) {
        return;
    }
    const int sourceValue = tab->pageOrder[sourceDisplayPosition];
    tab->pageOrder.remove(sourceDisplayPosition);
    int insertPos = targetDisplayPosition;
    if (sourceDisplayPosition < targetDisplayPosition) {
        --insertPos; // the target shifted left by one when the source was removed
    }
    if (after) {
        ++insertPos;
    }
    insertPos = qBound(0, insertPos, tab->pageOrder.size());
    tab->pageOrder.insert(insertPos, sourceValue);
    refreshThumbnailOrder();
    onCurrentPageChanged(tab->view->currentPage());
    pushUndoIfChanged(tr("Reorder Pages"), before);
}

void MainWindow::onThumbnailDeleteRequested(int displayPosition)
{
    deletePageAt(displayPosition);
}

void MainWindow::onThumbnailCopyRequested(int displayPosition)
{
    copyPageAt(displayPosition);
}

void MainWindow::onThumbnailPasteRequested(int displayPosition)
{
    pastePageAfter(displayPosition);
}

void MainWindow::onThumbnailContextMenu(int displayPosition, const QPoint &globalPos)
{
    QMenu menu(this);
    QAction *copyAction = menu.addAction(tr("Copy Page"));
    QAction *pasteAction = menu.addAction(tr("Paste Page"));
    DocumentTab *tab = currentTab();
    pasteAction->setEnabled(tab && tab->copiedSourcePage >= 0);
    QAction *duplicateAction = menu.addAction(tr("Duplicate Page"));
    menu.addSeparator();
    QAction *deleteAction = menu.addAction(tr("Delete Page"));
    deleteAction->setEnabled(currentPageOrderOrIdentity().size() > 1);
    menu.addSeparator();
    QAction *insertFromFileAction = menu.addAction(tr("Insert Pages from File..."));

    QAction *chosen = menu.exec(globalPos);
    if (chosen == copyAction) {
        copyPageAt(displayPosition);
    } else if (chosen == pasteAction) {
        pastePageAfter(displayPosition);
    } else if (chosen == duplicateAction) {
        copyPageAt(displayPosition);
        pastePageAfter(displayPosition);
    } else if (chosen == deleteAction) {
        deletePageAt(displayPosition);
    } else if (chosen == insertFromFileAction) {
        insertPagesFromFile(displayPosition);
    }
}

namespace {
QVector<int> pageOrderOrIdentityFor(MainWindow::DocumentTab *tab)
{
    if (!tab) {
        return {};
    }
    if (!tab->pageOrder.isEmpty()) {
        // Guard against a stale order left over from the *previous*
        // document in this same tab: PdfView::openFile() fires
        // documentChanged() synchronously partway through loading a new
        // file, ahead of reloadTabFromPath()'s own pageOrder.clear() a few
        // lines later — so this can transiently be read before it's reset.
        // Falling back to identity whenever any entry is out of range for
        // the page count actually loaded right now is simpler and more
        // robust than trying to win that ordering race.
        bool valid = true;
        for (int sourceIndex : tab->pageOrder) {
            if (sourceIndex < 0 || sourceIndex >= tab->view->pageCount()) {
                valid = false;
                break;
            }
        }
        if (valid) {
            return tab->pageOrder;
        }
    }
    QVector<int> identity(tab->view->pageCount());
    for (int i = 0; i < identity.size(); ++i) {
        identity[i] = i;
    }
    return identity;
}
} // namespace

QVector<int> MainWindow::currentPageOrderOrIdentity() const
{
    return pageOrderOrIdentityFor(currentTab());
}

void MainWindow::goToAdjacentDisplayPage(int delta)
{
    // The main reading pane always shows pages in original source order
    // (see Reordering & Naming Pages in the manual), so stepping by raw
    // source index would ignore a drag/number reorder entirely. Step
    // through the *display* order instead, so Previous/Next follow
    // whatever order the Pages panel currently shows.
    const QVector<int> order = currentPageOrderOrIdentity();
    const int pos = order.indexOf(currentView()->currentPage());
    if (pos < 0) {
        return;
    }
    const int newPos = pos + delta;
    if (newPos < 0 || newPos >= order.size()) {
        return;
    }
    currentView()->goToPage(order[newPos]);
}

void MainWindow::onOutlineItemActivated(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    const auto it = m_bookmarkTargets.constFind(item);
    if (it == m_bookmarkTargets.constEnd() || it.value().page < 0) {
        return;
    }
    const BookmarkTarget &target = it.value();
    if (target.hasTextRange) {
        currentView()->showSearchMatch({target.page, target.start, target.end});
    } else {
        currentView()->goToPage(target.page);
    }
}

void MainWindow::onOutlineItemChanged(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    DocumentTab *tab = currentTab();
    const auto it = m_bookmarkTargets.find(item);
    if (!tab || it == m_bookmarkTargets.end()) {
        return;
    }
    const QString newTitle = item->text(0).trimmed();
    if (newTitle.isEmpty()) {
        return;
    }
    BookmarkTarget &target = it.value();
    if (target.pendingIndex >= 0 && target.pendingIndex < tab->pendingBookmarks.size()) {
        tab->pendingBookmarks[target.pendingIndex].title = newTitle;
    } else if (!target.nativePath.isEmpty()) {
        upsertOutlineRename(target.nativePath, newTitle);
    }
}

void MainWindow::onOutlineContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_outlineTree->itemAt(pos);
    if (!item) {
        return;
    }
    QMenu menu(this);
    QAction *renameAction = menu.addAction(tr("Rename"));
    QAction *chosen = menu.exec(m_outlineTree->viewport()->mapToGlobal(pos));
    if (chosen == renameAction) {
        m_outlineTree->editItem(item, 0);
    }
}

void MainWindow::upsertOutlineRename(const QVector<int> &path, const QString &newTitle)
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    for (PdfWriter::OutlineRename &rename : tab->pendingRenames) {
        if (rename.path == path) {
            rename.newTitle = newTitle;
            return;
        }
    }
    tab->pendingRenames.append({path, newTitle});
}

void MainWindow::onRecentFileTriggered()
{
    if (auto *action = qobject_cast<QAction *>(sender())) {
        const QString path = action->data().toString();
        if (!path.isEmpty()) {
            openPathInNewTab(path);
        }
    }
}

bool MainWindow::doSave(DocumentTab *tab, const QString &destPath,
                         const QVector<PdfWriter::ExternalInsertion> &insertions)
{
    if (!tab) {
        return false;
    }
    // Make sure a text box being actively typed into right now gets its
    // latest content flattened too, not whatever was last committed.
    tab->view->commitPendingEdits();

    PdfView *view = tab->view;
    const auto pageHasMarkups = [view](int sourcePage) { return view->pageHasMarkups(sourcePage); };
    const auto paintMarkups = [view](int sourcePage, QPainter &painter) {
        view->paintMarkupsForExport(painter, sourcePage);
    };
    const auto notesForPage = [view](int sourcePage) {
        QVector<PdfWriter::NoteAnchor> notes;
        for (const PdfView::NoteExport &n : view->notesForPage(sourcePage)) {
            notes.append({n.anchorRectPoints, n.text});
        }
        return notes;
    };

    QVector<PdfWriter::PageLabel> pageLabels;
    const QHash<int, QString> labels = m_thumbnailPanel->allLabels();
    for (auto it = labels.constBegin(); it != labels.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            pageLabels.append({it.key(), it.value()});
        }
    }

    QString error;
    const bool ok =
        PdfWriter::save(tab->currentFilePath, destPath, tab->pendingTitle, tab->pendingAuthor, tab->pageOrder,
                         tab->pendingBookmarks, tab->pendingRenames, tab->view->rotationQuarterTurns(), insertions,
                         pageLabels, pageHasMarkups, paintMarkups, notesForPage, &error);
    if (!ok) {
        QMessageBox::warning(this, tr("Save Failed"), error);
        return false;
    }
    // Reopen the just-written file into this same tab (reloadTabFromPath()
    // clears pending title/author/order/bookmark state since it's now
    // committed to disk).
    return reloadTabFromPath(tab, destPath);
}

namespace {

class SessionSnapshotCommand : public QUndoCommand {
public:
    SessionSnapshotCommand(MainWindow *window, MainWindow::SessionSnapshot before,
                            MainWindow::SessionSnapshot after, const QString &text)
        : QUndoCommand(text), m_window(window), m_before(std::move(before)), m_after(std::move(after))
    {
    }
    void undo() override { m_window->restoreSessionSnapshot(m_before); }
    void redo() override { m_window->restoreSessionSnapshot(m_after); }

private:
    MainWindow *m_window;
    MainWindow::SessionSnapshot m_before;
    MainWindow::SessionSnapshot m_after;
};

} // namespace

MainWindow::SessionSnapshot MainWindow::captureSessionSnapshot() const
{
    SessionSnapshot snapshot;
    DocumentTab *tab = currentTab();
    if (!tab) {
        return snapshot;
    }
    snapshot.pageOrder = currentPageOrderOrIdentity();
    snapshot.markups = tab->view->captureMarkupSnapshot();
    return snapshot;
}

void MainWindow::restoreSessionSnapshot(const SessionSnapshot &snapshot)
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    tab->pageOrder = snapshot.pageOrder;
    tab->view->restoreMarkupSnapshot(snapshot.markups); // does not itself emit sessionContentChanged
    tab->lastSnapshot = snapshot;
    refreshThumbnailOrder();
    refreshMarkupsPanel();
    refreshBookmarkIndicators();
}

void MainWindow::pushUndoIfChanged(const QString &text, const SessionSnapshot &before)
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    const SessionSnapshot after = captureSessionSnapshot();
    if (!(before == after)) {
        tab->undoStack->push(new SessionSnapshotCommand(this, before, after, text));
    }
    tab->lastSnapshot = after;
}

void MainWindow::onSessionContentChanged()
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    pushUndoIfChanged(tr("Edit Markup"), tab->lastSnapshot);
}

bool MainWindow::hasUnsavedChanges(DocumentTab *tab) const
{
    if (!tab || !tab->view->isDocumentLoaded()) {
        return false;
    }
    if (!tab->pendingTitle.isEmpty() || !tab->pendingAuthor.isEmpty()) {
        return true;
    }
    if (!tab->pendingBookmarks.isEmpty() || !tab->pendingRenames.isEmpty()) {
        return true;
    }
    const QVector<int> order = pageOrderOrIdentityFor(tab);
    for (int i = 0; i < order.size(); ++i) {
        if (order[i] != i) {
            return true;
        }
    }
    if (tab->view->markupCount() > 0 || tab->view->pastedImageCount() > 0) {
        return true;
    }
    if (tab->view->rotationQuarterTurns() != 0) {
        return true;
    }
    return false;
}

void MainWindow::onSaveTriggered()
{
    DocumentTab *tab = currentTab();
    if (!tab || tab->currentFilePath.isEmpty()) {
        return;
    }
    doSave(tab, tab->currentFilePath);
}

void MainWindow::onSaveAsTriggered()
{
    DocumentTab *tab = currentTab();
    if (!tab || tab->currentFilePath.isEmpty()) {
        return;
    }
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Save PDF As"), tab->currentFilePath, tr("PDF Files (*.pdf)"));
    if (!path.isEmpty()) {
        doSave(tab, path);
    }
}

void MainWindow::onAddBookmarkTriggered()
{
    DocumentTab *tab = currentTab();
    if (!tab || !tab->view->isDocumentLoaded()) {
        return;
    }

    // If there's a text selection, default the title to a snippet of it and
    // remember the exact range so jumping back here can highlight it too.
    const PdfView::SelectionRange selection = tab->view->currentTextSelection();
    const bool hasRange = selection.page >= 0;

    QString defaultTitle;
    if (hasRange) {
        QString snippet = tab->view->selectedText().simplified();
        if (snippet.length() > 60) {
            snippet = snippet.left(57) + QStringLiteral("...");
        }
        defaultTitle = snippet.isEmpty() ? tr("Page %1").arg(selection.page + 1) : snippet;
    } else {
        defaultTitle = tr("Page %1").arg(tab->view->currentPage() + 1);
    }

    bool ok = false;
    const QString title =
        QInputDialog::getText(this, tr("Add Bookmark"), tr("Bookmark title:"), QLineEdit::Normal, defaultTitle, &ok);
    if (!ok || title.trimmed().isEmpty()) {
        return;
    }

    PdfWriter::Bookmark bookmark;
    bookmark.title = title.trimmed();
    bookmark.pageIndexZeroBased = hasRange ? selection.page : tab->view->currentPage();
    tab->pendingBookmarks.append(bookmark);

    // Reflect it immediately in the Bookmarks panel. Marked italic + a
    // tooltip (rather than text appended to the title) so renaming it later
    // doesn't need to strip anything back out.
    const QSignalBlocker blocker(m_outlineTree);
    auto *item = new QTreeWidgetItem(m_outlineTree);
    item->setText(0, bookmark.title);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    QFont font = item->font(0);
    font.setItalic(true);
    item->setFont(0, font);
    item->setToolTip(0, tr("Not yet saved"));

    BookmarkTarget target;
    target.page = bookmark.pageIndexZeroBased;
    target.hasTextRange = hasRange;
    if (hasRange) {
        target.start = selection.start;
        target.end = selection.end;
    }
    target.pendingIndex = tab->pendingBookmarks.size() - 1;
    m_bookmarkTargets.insert(item, target);
    refreshBookmarkIndicators();
}

void MainWindow::onAddHighlightTriggered()
{
    if (!currentView()->addHighlightFromSelection()) {
        statusBar()->showMessage(tr("Select some text first, then Highlight it."), 3000);
    }
}

namespace {
QString markupKindLabel(PdfView::MarkupKind kind)
{
    switch (kind) {
    case PdfView::MarkupKind::Highlight:
        return MainWindow::tr("Highlight");
    case PdfView::MarkupKind::TextBox:
        return MainWindow::tr("Text");
    case PdfView::MarkupKind::Shape:
        return MainWindow::tr("Shape");
    }
    return {};
}
} // namespace

void MainWindow::refreshMarkupsPanel()
{
    PdfView *view = currentView();
    const int count = view ? view->markupCount() : 0;
    const QSignalBlocker blocker(m_markupsTable);
    m_markupsTable->setRowCount(count);
    for (int i = 0; i < count; ++i) {
        const PdfView::MarkupEntry entry = view->markupEntry(i);

        auto *kindItem = new QTableWidgetItem(markupKindLabel(entry.kind));
        kindItem->setFlags(kindItem->flags() & ~Qt::ItemIsEditable);
        m_markupsTable->setItem(i, 0, kindItem);

        auto *pageItem = new QTableWidgetItem(QString::number(entry.page + 1));
        pageItem->setFlags(pageItem->flags() & ~Qt::ItemIsEditable);
        m_markupsTable->setItem(i, 1, pageItem);

        auto *descriptionItem = new QTableWidgetItem(entry.description);
        descriptionItem->setFlags(descriptionItem->flags() & ~Qt::ItemIsEditable);
        m_markupsTable->setItem(i, 2, descriptionItem);

        m_markupsTable->setItem(i, 3, new QTableWidgetItem(entry.note));
    }
}

void MainWindow::onMarkupCellClicked(int row, int column)
{
    Q_UNUSED(column);
    currentView()->jumpToMarkup(row);
}

void MainWindow::onMarkupNoteEdited(QTableWidgetItem *item)
{
    if (!item || item->column() != 3) {
        return;
    }
    currentView()->setMarkupNote(item->row(), item->text());
}

void MainWindow::onMarkupsContextMenu(const QPoint &pos)
{
    const QTableWidgetItem *item = m_markupsTable->itemAt(pos);
    if (!item) {
        return;
    }
    const int row = item->row();
    QMenu menu(this);
    if (currentView()->markupEntry(row).kind == PdfView::MarkupKind::Highlight) {
        QAction *colorAction = menu.addAction(tr("Change Highlight Color..."));
        connect(colorAction, &QAction::triggered, this, [this, row]() {
            const QColor chosen =
                QColorDialog::getColor(currentView()->highlightColor(row), this, tr("Highlight Color"));
            if (chosen.isValid()) {
                currentView()->setHighlightColor(row, chosen);
            }
        });
    }
    QAction *removeAction = menu.addAction(tr("Remove"));
    connect(removeAction, &QAction::triggered, this, [this, row]() { currentView()->removeMarkup(row); });
    menu.exec(m_markupsTable->viewport()->mapToGlobal(pos));
}

void MainWindow::onRevertTriggered()
{
    DocumentTab *tab = currentTab();
    if (!tab || tab->currentFilePath.isEmpty()) {
        return;
    }
    const auto reply = QMessageBox::question(
        this, tr("Revert to Saved"),
        tr("Discard all changes and reload \"%1\" from disk?").arg(QFileInfo(tab->currentFilePath).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        reloadTabFromPath(tab, tab->currentFilePath);
    }
}

void MainWindow::onFullScreenToggled(bool checked)
{
    if (checked) {
        showFullScreen();
    } else {
        showNormal();
    }
}

void MainWindow::onAboutTriggered()
{
    QMessageBox::about(this, tr("About DirectPDF"),
                        tr("<b>DirectPDF</b> version %1"
                           "<p>A lightweight, open-source PDF reader and markup tool.</p>"
                           "<p>Built with Qt and Poppler.</p>")
                            .arg(QCoreApplication::applicationVersion()));
}

void MainWindow::onShortcutsTriggered()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Keyboard Shortcuts"));

    auto *layout = new QVBoxLayout(&dialog);
    auto *tree = new QTreeWidget(&dialog);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Action"), tr("Shortcut")});
    tree->setRootIsDecorated(false);

    std::function<void(QMenu *)> collect = [&](QMenu *menu) {
        const auto actions = menu->actions();
        for (QAction *action : actions) {
            if (action->isSeparator()) {
                continue;
            }
            if (QMenu *sub = action->menu()) {
                collect(sub);
                continue;
            }
            if (action->shortcut().isEmpty()) {
                continue;
            }
            QString text = action->text();
            text.remove(QLatin1Char('&'));
            auto *item = new QTreeWidgetItem(tree);
            item->setText(0, text);
            item->setText(1, action->shortcut().toString(QKeySequence::NativeText));
        }
    };
    const auto topActions = menuBar()->actions();
    for (QAction *topAction : topActions) {
        if (QMenu *menu = topAction->menu()) {
            collect(menu);
        }
    }
    tree->resizeColumnToContents(0);
    layout->addWidget(tree);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(420, 480);
    dialog.exec();
}

void MainWindow::onUserManualTriggered()
{
    QFile file(QStringLiteral(":/USER_MANUAL.md"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("User Manual"), tr("Could not load the user manual."));
        return;
    }
    const QString markdown = QString::fromUtf8(file.readAll());

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("DirectPDF User Manual"));

    auto *layout = new QVBoxLayout(dialog);
    auto *browser = new QTextBrowser(dialog);
    browser->setOpenExternalLinks(true);
    browser->setMarkdown(markdown);
    layout->addWidget(browser);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(buttons);

    dialog->resize(680, 720);
    dialog->show();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && m_fullScreenAction) {
        const QSignalBlocker blocker(m_fullScreenAction);
        m_fullScreenAction->setChecked(isFullScreen());
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Snapshot the tabs first — confirming one may close it (Discard, or a
    // successful Save both call reloadTabFromPath()/destroyTab()-adjacent
    // paths), which would shift live QTabWidget indices out from under a
    // live loop.
    QVector<DocumentTab *> tabs;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto *view = qobject_cast<PdfView *>(m_tabWidget->widget(i))) {
            tabs.append(m_tabsByView.value(view));
        }
    }
    for (DocumentTab *tab : tabs) {
        // Switch to it first so any save/discard prompt clearly refers to
        // the document being asked about.
        m_tabWidget->setCurrentWidget(tab->view);
        if (!confirmDiscardUnsavedChanges(tab)) {
            event->ignore();
            return;
        }
    }
    saveWindowLayout();
    event->accept();
}

void MainWindow::restoreWindowLayout()
{
    QSettings settings;
    if (settings.contains("mainWindow/geometry")) {
        restoreGeometry(settings.value("mainWindow/geometry").toByteArray());
    }
    if (settings.contains("mainWindow/state")) {
        restoreState(settings.value("mainWindow/state").toByteArray());
    }
}

void MainWindow::saveWindowLayout()
{
    QSettings settings;
    settings.setValue("mainWindow/geometry", saveGeometry());
    settings.setValue("mainWindow/state", saveState());
}

bool MainWindow::confirmDiscardUnsavedChanges(DocumentTab *tab)
{
    if (!hasUnsavedChanges(tab)) {
        return true;
    }

    const auto reply = QMessageBox::question(
        this, tr("Save Changes?"),
        tr("Do you want to save changes to \"%1\"?").arg(QFileInfo(tab->currentFilePath).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (reply == QMessageBox::Cancel) {
        return false;
    }
    if (reply == QMessageBox::Discard) {
        cleanupRecoveryFile(tab);
        return true;
    }

    QString destPath = tab->currentFilePath;
    if (destPath.isEmpty()) {
        destPath = QFileDialog::getSaveFileName(this, tr("Save PDF As"), QString(), tr("PDF Files (*.pdf)"));
        if (destPath.isEmpty()) {
            return false;
        }
    }
    return doSave(tab, destPath);
}

QString MainWindow::recoveryDirPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/recovery");
}

void MainWindow::writeRecoverySnapshot()
{
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        auto *view = qobject_cast<PdfView *>(m_tabWidget->widget(i));
        if (DocumentTab *tab = view ? m_tabsByView.value(view) : nullptr) {
            writeRecoverySnapshotFor(tab);
        }
    }
}

void MainWindow::writeRecoverySnapshotFor(DocumentTab *tab)
{
    if (!hasUnsavedChanges(tab)) {
        return;
    }
    if (tab->recoveryFilePath.isEmpty()) {
        QDir().mkpath(recoveryDirPath());
        const QString base = QFileInfo(tab->currentFilePath).completeBaseName();
        const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        tab->recoveryFilePath = recoveryDirPath() + QStringLiteral("/%1-%2.pdf").arg(base, uid);
    }

    // Deliberately does NOT call tab->view->commitPendingEdits() — autosave
    // firing mid-keystroke shouldn't yank focus out of a text box the user
    // is actively typing into. Worst case, that one box's snapshot is a
    // save behind; everything else (and that box's own previously-
    // committed content) is still protected.
    PdfView *view = tab->view;
    const auto pageHasMarkups = [view](int page) { return view->pageHasMarkups(page); };
    const auto paintMarkups = [view](int page, QPainter &painter) { view->paintMarkupsForExport(painter, page); };
    const auto notesForPage = [view](int page) {
        QVector<PdfWriter::NoteAnchor> notes;
        for (const PdfView::NoteExport &n : view->notesForPage(page)) {
            notes.append({n.anchorRectPoints, n.text});
        }
        return notes;
    };
    // Page labels live in the single shared Pages panel widget rather than
    // per-tab, so they can only be read reliably for the active tab — a
    // background tab's recovery snapshot may miss a label typed while it
    // was last active but not yet saved. Markups, page order, bookmarks,
    // and rotation are unaffected (all live directly on the tab) and
    // always recover correctly regardless of which tab is active.
    QVector<PdfWriter::PageLabel> pageLabels;
    if (tab == currentTab()) {
        const QHash<int, QString> labels = m_thumbnailPanel->allLabels();
        for (auto it = labels.constBegin(); it != labels.constEnd(); ++it) {
            if (!it.value().isEmpty()) {
                pageLabels.append({it.key(), it.value()});
            }
        }
    }

    QString error;
    const bool ok = PdfWriter::save(tab->currentFilePath, tab->recoveryFilePath, tab->pendingTitle,
                                     tab->pendingAuthor, tab->pageOrder, tab->pendingBookmarks, tab->pendingRenames,
                                     tab->view->rotationQuarterTurns(), {}, pageLabels, pageHasMarkups, paintMarkups,
                                     notesForPage, &error);
    if (!ok) {
        return; // silent — a failed autosave shouldn't interrupt the user; the next tick tries again
    }

    QSettings settings;
    QVariantMap map = settings.value("recovery/map").toMap();
    map[tab->recoveryFilePath] = tab->currentFilePath;
    settings.setValue("recovery/map", map);
}

void MainWindow::cleanupRecoveryFile(DocumentTab *tab)
{
    if (!tab || tab->recoveryFilePath.isEmpty()) {
        return;
    }
    QFile::remove(tab->recoveryFilePath);
    QSettings settings;
    QVariantMap map = settings.value("recovery/map").toMap();
    map.remove(tab->recoveryFilePath);
    settings.setValue("recovery/map", map);
    tab->recoveryFilePath.clear();
}

void MainWindow::checkForRecoverableFiles()
{
    QSettings settings;
    QVariantMap map = settings.value("recovery/map").toMap();
    if (map.isEmpty()) {
        return;
    }

    // Unlike before multi-tab, every orphan can be recovered — each gets
    // its own new tab, not just the first.
    for (auto it = map.begin(); it != map.end();) {
        const QString recoveryPath = it.key();
        const QString originalPath = it.value().toString();

        if (!QFile::exists(recoveryPath)) {
            it = map.erase(it);
            continue;
        }

        const auto reply = QMessageBox::question(
            this, tr("Recover Unsaved Changes?"),
            tr("DirectPDF didn't close properly last time. A recovered version of \"%1\" with unsaved changes is "
               "available. Open it?")
                .arg(QFileInfo(originalPath).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (reply == QMessageBox::Yes) {
            if (DocumentTab *tab = openPathInNewTab(recoveryPath)) {
                // openPathInNewTab() just cleaned up this (brand new)
                // tab's own empty recovery bookkeeping — now adopt the
                // recovered file's own entry instead, so autosave keeps
                // targeting the same file rather than starting a new one.
                tab->recoveryFilePath = recoveryPath;
                QMessageBox::information(
                    this, tr("Recovered"),
                    tr("This is a recovered copy of \"%1\" with unsaved changes restored. Use Save As to write it "
                       "back over the original file, or Save to keep this as a new copy.")
                        .arg(QFileInfo(originalPath).fileName()));
            }
            ++it; // keep registered — still a live tab's recovery file
        } else {
            QFile::remove(recoveryPath);
            it = map.erase(it);
        }
    }
    settings.setValue("recovery/map", map);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_findEdit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            onFindBarClosed();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return && (keyEvent->modifiers() & Qt::ShiftModifier)) {
            onFindPrevious();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onFindTriggered()
{
    if (!currentView() || !currentView()->isDocumentLoaded()) {
        return;
    }
    m_findBar->setVisible(true);
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void MainWindow::onFindTextChanged(const QString &text)
{
    PdfView *view = currentView();
    if (!view) {
        return;
    }
    m_searchMatches = view->findAll(text);
    m_searchIndex = m_searchMatches.isEmpty() ? -1 : 0;
    if (m_searchIndex >= 0) {
        view->showSearchMatch(m_searchMatches[m_searchIndex]);
    } else {
        view->clearSearchHighlight();
    }
    updateFindStatus();
}

void MainWindow::onFindNext()
{
    if (m_searchMatches.isEmpty()) {
        return;
    }
    m_searchIndex = (m_searchIndex + 1) % m_searchMatches.size();
    currentView()->showSearchMatch(m_searchMatches[m_searchIndex]);
    updateFindStatus();
}

void MainWindow::onFindPrevious()
{
    if (m_searchMatches.isEmpty()) {
        return;
    }
    m_searchIndex = (m_searchIndex - 1 + m_searchMatches.size()) % m_searchMatches.size();
    currentView()->showSearchMatch(m_searchMatches[m_searchIndex]);
    updateFindStatus();
}

void MainWindow::onFindBarClosed()
{
    m_findBar->setVisible(false);
    if (PdfView *view = currentView()) {
        view->clearSearchHighlight();
        view->setFocus();
    }
}

void MainWindow::updateFindStatus()
{
    if (m_findEdit->text().isEmpty()) {
        m_findCountLabel->setText(QString());
    } else if (m_searchMatches.isEmpty()) {
        m_findCountLabel->setText(tr("No matches"));
    } else {
        m_findCountLabel->setText(tr("%1 of %2").arg(m_searchIndex + 1).arg(m_searchMatches.size()));
    }
}

void MainWindow::refreshThumbnailOrder()
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    const QVector<int> order = currentPageOrderOrIdentity();
    tab->view->setPageOrder(order);
    m_thumbnailPanel->rebuildFromOrder(order);
    constexpr qreal kThumbDpi = 60.0;
    QSet<int> rendered;
    for (int sourceIndex : order) {
        if (rendered.contains(sourceIndex)) {
            continue; // a duplicated page needs rendering only once
        }
        rendered.insert(sourceIndex);
        QImage img = tab->thumbnailImageCache.value(sourceIndex);
        if (img.isNull()) {
            img = tab->view->document().renderPage(sourceIndex, kThumbDpi, tab->view->rotationQuarterTurns());
            tab->thumbnailImageCache.insert(sourceIndex, img);
        }
        m_thumbnailPanel->setThumbnail(sourceIndex, img);
    }
    m_thumbnailPanel->setCurrentSourcePage(tab->view->currentPage());
}

int MainWindow::effectivePageCount() const
{
    return currentPageOrderOrIdentity().size();
}

int MainWindow::currentDisplayPosition() const
{
    PdfView *view = currentView();
    return view ? currentPageOrderOrIdentity().indexOf(view->currentPage()) : -1;
}

void MainWindow::copyPageAt(int displayPosition)
{
    DocumentTab *tab = currentTab();
    const QVector<int> order = currentPageOrderOrIdentity();
    if (!tab || displayPosition < 0 || displayPosition >= order.size()) {
        return;
    }
    tab->copiedSourcePage = order[displayPosition];
    statusBar()->showMessage(tr("Copied page %1.").arg(displayPosition + 1), 2000);
}

void MainWindow::pastePageAfter(int displayPosition)
{
    DocumentTab *tab = currentTab();
    if (!tab || tab->copiedSourcePage < 0) {
        statusBar()->showMessage(tr("Copy a page first, then paste it."), 3000);
        return;
    }
    const SessionSnapshot before = captureSessionSnapshot();
    tab->pageOrder = currentPageOrderOrIdentity();
    if (displayPosition < 0 || displayPosition >= tab->pageOrder.size()) {
        return;
    }
    tab->pageOrder.insert(displayPosition + 1, tab->copiedSourcePage);
    refreshThumbnailOrder();
    onCurrentPageChanged(tab->view->currentPage());
    pushUndoIfChanged(tr("Paste Page"), before);
}

void MainWindow::deletePageAt(int displayPosition)
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    const SessionSnapshot before = captureSessionSnapshot();
    tab->pageOrder = currentPageOrderOrIdentity();
    if (tab->pageOrder.size() <= 1) {
        statusBar()->showMessage(tr("Can't delete the only remaining page."), 3000);
        return;
    }
    if (displayPosition < 0 || displayPosition >= tab->pageOrder.size()) {
        return;
    }
    tab->pageOrder.remove(displayPosition);
    refreshThumbnailOrder();
    onCurrentPageChanged(tab->view->currentPage());
    pushUndoIfChanged(tr("Delete Page"), before);
}

namespace {

// Parses a 1-based page range string like "1-3,5,7-9" into 0-based page
// indices in the order given, clamped to [0, pageCount). Unparseable or
// out-of-range segments are silently skipped rather than rejecting the
// whole string, so one typo doesn't throw away an otherwise-valid list.
QVector<int> parsePageRange(const QString &text, int pageCount)
{
    QVector<int> result;
    for (const QString &rawPart : text.split(',', Qt::SkipEmptyParts)) {
        const QString part = rawPart.trimmed();
        const int dash = part.indexOf('-');
        int first = 0;
        int last = 0;
        bool ok1 = false;
        bool ok2 = false;
        if (dash > 0) {
            first = part.left(dash).trimmed().toInt(&ok1);
            last = part.mid(dash + 1).trimmed().toInt(&ok2);
        } else {
            first = part.toInt(&ok1);
            last = first;
            ok2 = ok1;
        }
        if (!ok1 || !ok2) {
            continue;
        }
        if (first > last) {
            std::swap(first, last);
        }
        for (int p = first; p <= last; ++p) {
            if (p >= 1 && p <= pageCount) {
                result.append(p - 1);
            }
        }
    }
    return result;
}

} // namespace

void MainWindow::insertPagesFromFile(int afterDisplayPosition)
{
    DocumentTab *tab = currentTab();
    if (!tab || !tab->view->isDocumentLoaded()) {
        return;
    }
    const QString sourcePath =
        QFileDialog::getOpenFileName(this, tr("Insert Pages From"), QString(), tr("PDF Files (*.pdf)"));
    if (sourcePath.isEmpty()) {
        return;
    }

    PdfDocument externalDoc;
    QString loadError;
    if (!externalDoc.load(sourcePath, &loadError)) {
        QMessageBox::warning(this, tr("Could Not Open PDF"), loadError);
        return;
    }
    const int externalPageCount = externalDoc.pageCount();
    if (externalPageCount <= 0) {
        QMessageBox::warning(this, tr("Could Not Open PDF"), tr("That file has no pages."));
        return;
    }

    bool ok = false;
    const QString rangeText = QInputDialog::getText(
        this, tr("Insert Pages"),
        tr("Which pages to insert from \"%1\"? (e.g. 1-3,5) This file has %2 page(s).")
            .arg(QFileInfo(sourcePath).fileName())
            .arg(externalPageCount),
        QLineEdit::Normal, tr("1-%1").arg(externalPageCount), &ok);
    if (!ok || rangeText.trimmed().isEmpty()) {
        return;
    }

    const QVector<int> pageIndices = parsePageRange(rangeText, externalPageCount);
    if (pageIndices.isEmpty()) {
        QMessageBox::warning(this, tr("Insert Pages"), tr("No valid page numbers were entered."));
        return;
    }

    PdfWriter::ExternalInsertion insertion;
    insertion.sourcePath = sourcePath;
    insertion.pageIndices = pageIndices;
    insertion.afterDisplayPosition = afterDisplayPosition;

    // This immediately performs a real Save (like clicking Save) rather
    // than staying pending — see the declaration comment for why.
    if (doSave(tab, tab->currentFilePath, {insertion})) {
        statusBar()->showMessage(
            tr("Inserted %1 page(s) from \"%2\".").arg(pageIndices.size()).arg(QFileInfo(sourcePath).fileName()),
            3000);
    }
}

namespace {
void addOutlineItems(QTreeWidgetItem *parent, QTreeWidget *tree, const QVector<PdfDocument::OutlineEntry> &entries,
                      const QVector<int> &pathPrefix, QHash<QTreeWidgetItem *, MainWindow::BookmarkTarget> &targets)
{
    for (int i = 0; i < entries.size(); ++i) {
        const PdfDocument::OutlineEntry &entry = entries[i];
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setText(0, entry.title);
        item->setFlags(item->flags() | Qt::ItemIsEditable);

        QVector<int> path = pathPrefix;
        path.append(i);

        MainWindow::BookmarkTarget target;
        target.page = entry.pageIndex;
        target.nativePath = path;
        targets.insert(item, target);

        if (!entry.children.isEmpty()) {
            addOutlineItems(item, tree, entry.children, path, targets);
        }
    }
}
} // namespace

void MainWindow::populateOutline()
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        return;
    }
    const QSignalBlocker blocker(m_outlineTree);
    m_outlineTree->clear();
    m_bookmarkTargets.clear();

    const QVector<PdfDocument::OutlineEntry> entries = tab->view->document().outline();
    addOutlineItems(nullptr, m_outlineTree, entries, {}, m_bookmarkTargets);
    m_outlineTree->expandAll();
}

void MainWindow::updateRecentFilesMenu()
{
    m_recentFilesMenu->clear();
    QSettings settings;
    const QStringList files = settings.value("recentFiles").toStringList();

    if (files.isEmpty()) {
        QAction *empty = m_recentFilesMenu->addAction(tr("(No Recent Files)"));
        empty->setEnabled(false);
        return;
    }

    for (const QString &path : files) {
        QAction *action = m_recentFilesMenu->addAction(QFileInfo(path).fileName());
        action->setData(path);
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, &MainWindow::onRecentFileTriggered);
    }
    m_recentFilesMenu->addSeparator();
    QAction *clearAction = m_recentFilesMenu->addAction(tr("Clear Recent Files"));
    connect(clearAction, &QAction::triggered, this, [this]() {
        QSettings settings;
        settings.remove("recentFiles");
        updateRecentFilesMenu();
    });
}

void MainWindow::addToRecentFiles(const QString &path)
{
    QSettings settings;
    QStringList files = settings.value("recentFiles").toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 8) {
        files.removeLast();
    }
    settings.setValue("recentFiles", files);
    updateRecentFilesMenu();
}
