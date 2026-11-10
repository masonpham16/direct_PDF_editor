#pragma once

#include <QHash>
#include <QImage>
#include <QScrollArea>
#include <QSet>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QBoxLayout;
class QDragEnterEvent;
class QDropEvent;
class QKeyEvent;
class QContextMenuEvent;

// A single thumbnail: the page image (bordered, highlighted when current)
// with a small, read-only page-number badge over its top-left corner
// showing the page's current *display position* (1-based), and a separate
// editable label field below for a free-text name of your own choosing
// (e.g. "Chapter 1"), independent of the position number. Clicking the
// image jumps the main view to this page; dragging it onto another
// thumbnail reorders pages. Since a page can be duplicated, more than one
// item can share the same sourcePageIndex() — displayPosition() is what
// uniquely identifies *this* occurrence.
class ThumbnailItemWidget : public QWidget {
    Q_OBJECT

public:
    explicit ThumbnailItemWidget(int sourcePageIndex, QWidget *parent = nullptr);

    void setImage(const QImage &image);
    // size is a target width (Qt::Vertical panel orientation, thumbnails
    // stacked top-to-bottom) or target height (Qt::Horizontal, stacked
    // left-to-right) — whichever dimension the panel's scroll direction
    // isn't filling.
    void setThumbnailSize(int size, Qt::Orientation orientation);
    void setCurrent(bool current);
    void setBookmarked(bool bookmarked);
    void setDisplayPosition(int displayPositionZeroBased);
    void setLabel(const QString &label);
    QString label() const;
    int sourcePageIndex() const { return m_sourcePageIndex; }
    int displayPosition() const { return m_displayPosition; }

signals:
    void activated(int sourcePageIndex);
    // Drag-and-drop reorder: this item (at displayPosition()) was dropped
    // onto the item at targetDisplayPosition; `after` says whether it
    // should land just after (true) or just before (false) the target.
    // Carries *display* positions rather than source page indices so a
    // drag involving a duplicated page (two items sharing one source
    // index) is unambiguous.
    void reorderRequested(int sourceDisplayPosition, int targetDisplayPosition, bool after);
    void labelEdited(int sourcePageIndex, const QString &label);
    // Left/Right (or Up/Down, matching the panel's orientation) pressed
    // while this thumbnail has focus: -1 for previous, +1 for next, in
    // display order.
    void adjacentPageRequested(int delta);
    void deleteRequested(int displayPosition);
    void copyRequested(int displayPosition);
    void pasteRequested(int displayPosition);
    void contextMenuRequested(int displayPosition, const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void updatePixmap();
    void updateBorder();

    int m_sourcePageIndex;
    int m_displayPosition;
    QImage m_image;
    int m_size = 120;
    Qt::Orientation m_orientation = Qt::Vertical;
    bool m_current = false;
    bool m_bookmarked = false;
    QPoint m_dragStartPos;

    QLabel *m_imageLabel = nullptr;
    QLabel *m_pageNumberLabel = nullptr;
    QLineEdit *m_labelEdit = nullptr;
};

// Scrollable list of thumbnails that rescale to fill the available
// cross-axis space whenever the panel is resized. Flows top-to-bottom
// (scrolling vertically) or left-to-right (scrolling horizontally)
// depending on setOrientation() — intended to match whichever edge of the
// window the panel is currently docked to.
//
// The displayed sequence (rebuildFromOrder()) is always rebuilt wholesale
// from scratch rather than incrementally patched — once pages can be
// deleted or duplicated, "one widget per original page, repositioned" no
// longer holds (a page can have zero or several on-screen items), so a
// full rebuild is simpler and safer than trying to diff two arbitrary
// sequences. Per-source-page state that would otherwise be lost when its
// widget is destroyed (its label, whether it's bookmarked/current) is
// cached here and reapplied to whatever new widget(s) represent that page.
class ThumbnailPanel : public QScrollArea {
    Q_OBJECT

public:
    explicit ThumbnailPanel(QWidget *parent = nullptr);

    // Rebuilds the displayed thumbnails to exactly match sourcePageIndices,
    // in order — one item per entry, so an index can be omitted (deleted)
    // or repeated (duplicated). Existing items are destroyed and recreated;
    // call setThumbnail() afterward to (re-)populate each one's image.
    void rebuildFromOrder(const QVector<int> &sourcePageIndices);
    // Pushes image to every current item showing sourcePageIndex (there
    // may be more than one, if that page is duplicated).
    void setThumbnail(int sourcePageIndex, const QImage &image);
    void setCurrentSourcePage(int sourcePageIndex);
    void clear();
    void setOrientation(Qt::Orientation orientation);
    // Marks every item whose page is in sourcePageIndices as bookmarked
    // (clearing the marker from any item not in the set).
    void setBookmarkedPages(const QSet<int> &sourcePageIndices);
    // The source page index of whichever thumbnail currently has keyboard
    // focus, or -1 if none does — lets MainWindow route Ctrl+C/Ctrl+V to
    // page copy/paste instead of the normal text/image clipboard when the
    // Pages panel is what the user's actually interacting with.
    int focusedDisplayPosition() const;
    // Every page's free-text label, by source page index — used when
    // saving (see PdfWriter::PageLabel / Save's "What gets saved" in the
    // manual).
    QHash<int, QString> allLabels() const { return m_labelsBySourcePage; }
    // Populates labels read back from the file's own /PageLabels (see
    // PdfWriter::readPageLabels) — called once after a document loads,
    // before the first rebuildFromOrder().
    void setInitialLabels(const QHash<int, QString> &labels) { m_labelsBySourcePage = labels; }

signals:
    void pageActivated(int sourcePageIndex);
    void reorderRequested(int sourceDisplayPosition, int targetDisplayPosition, bool after);
    void labelEdited(int sourcePageIndex, const QString &label);
    void adjacentPageRequested(int delta);
    void deleteRequested(int displayPosition);
    void copyRequested(int displayPosition);
    void pasteRequested(int displayPosition);
    void contextMenuRequested(int displayPosition, const QPoint &globalPos);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void relayoutWidths();
    ThumbnailItemWidget *createItem(int sourcePageIndex);

    QWidget *m_content = nullptr;
    QBoxLayout *m_layout = nullptr;
    QVector<ThumbnailItemWidget *> m_items; // in display order; sourcePageIndex() may repeat
    int m_currentSourcePage = -1;
    Qt::Orientation m_orientation = Qt::Vertical;

    // Survives rebuildFromOrder() even though the widgets themselves don't.
    QHash<int, QString> m_labelsBySourcePage;
    QSet<int> m_bookmarkedSourcePages;
};
