#include "ThumbnailPanel.h"

#include <QApplication>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>

namespace {
constexpr qreal kThumbnailCornerRadius = 7.0;

void drawBookmarkRibbon(QPixmap &pixmap)
{
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const int ribbonWidth = qMax(10, pixmap.width() / 7);
    const int ribbonHeight = qMax(18, pixmap.height() / 5);
    const int x = pixmap.width() - ribbonWidth - 3;
    const QPolygon ribbon({
        QPoint(x, 0),
        QPoint(x + ribbonWidth, 0),
        QPoint(x + ribbonWidth, ribbonHeight),
        QPoint(x + ribbonWidth / 2, ribbonHeight - ribbonWidth / 2),
        QPoint(x, ribbonHeight),
    });
    painter.setPen(QPen(QColor(150, 100, 0), 1));
    painter.setBrush(QColor(255, 193, 7));
    painter.drawPolygon(ribbon);
}

// A stylesheet border-radius alone would round the border decoration but
// not the pixmap content underneath — the square image corner would still
// peek past the rounded border. Drawing the border as part of the same
// pixmap, on top of (i.e. in front of) the image rather than as a separate
// widget-level layer underneath it, keeps the two perfectly aligned: the
// image is clipped to the rounded rect first, then the border is stroked
// along that exact same path.
QPixmap roundedThumbnail(const QPixmap &src, bool current)
{
    QPen pen(current ? QColor(0x30, 0x78, 0xff) : QColor(0x99, 0x99, 0x99));
    pen.setWidthF(current ? 3.0 : 2.0);

    QPixmap result(src.size());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal inset = pen.widthF() / 2.0;
    const QRectF rect(inset, inset, src.width() - pen.widthF(), src.height() - pen.widthF());
    QPainterPath path;
    path.addRoundedRect(rect, kThumbnailCornerRadius, kThumbnailCornerRadius);

    painter.setClipPath(path);
    painter.fillRect(result.rect(), Qt::white);
    painter.drawPixmap(0, 0, src);
    painter.setClipping(false);

    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    return result;
}

const char *kPageMimeType = "application/x-directpdf-page";
} // namespace

ThumbnailItemWidget::ThumbnailItemWidget(int sourcePageIndex, QWidget *parent)
    : QWidget(parent)
    , m_sourcePageIndex(sourcePageIndex)
    , m_displayPosition(sourcePageIndex)
{
    setAcceptDrops(true);
    // Clicking a thumbnail should let the arrow keys step through pages
    // right away, without needing a separate Tab press to focus it first.
    setFocusPolicy(Qt::ClickFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 6);
    layout->setSpacing(4);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet("background: white;");

    // The position number is purely informational now (reordering is done
    // by dragging) — a plain label rather than an editable field, so it
    // doesn't look like something you're meant to type into. It sits as a
    // small badge over the thumbnail's top-left corner, nudged in from the
    // true corner a bit (sharing a grid cell with the image rather than its
    // own row) so the label can go directly under the image without a big
    // gap between the two.
    m_pageNumberLabel = new QLabel(this);
    m_pageNumberLabel->setText(QString::number(m_displayPosition + 1));
    m_pageNumberLabel->setAlignment(Qt::AlignCenter);
    m_pageNumberLabel->setFixedSize(26, 18);
    m_pageNumberLabel->setStyleSheet(
        "QLabel { background: rgba(255, 255, 255, 215); border: 1px solid rgba(0, 0, 0, 90); "
        "border-radius: 3px; font-size: 10px; margin-top: 5px; margin-left: 7px; }");

    auto *overlay = new QGridLayout();
    overlay->setContentsMargins(0, 0, 0, 0);
    overlay->setSpacing(0);
    overlay->addWidget(m_imageLabel, 0, 0);
    overlay->addWidget(m_pageNumberLabel, 0, 0, Qt::AlignTop | Qt::AlignLeft);
    layout->addLayout(overlay);
    layout->setAlignment(overlay, Qt::AlignHCenter);
    m_pageNumberLabel->raise();

    m_labelEdit = new QLineEdit(this);
    m_labelEdit->setAlignment(Qt::AlignCenter);
    m_labelEdit->setFrame(false);
    m_labelEdit->setFixedWidth(96);
    m_labelEdit->setPlaceholderText(tr("Label..."));
    m_labelEdit->setToolTip(tr("Optional name for this page (not written to the PDF)"));
    layout->addWidget(m_labelEdit, 0, Qt::AlignHCenter);

    connect(m_labelEdit, &QLineEdit::editingFinished, this,
            [this]() { emit labelEdited(m_sourcePageIndex, m_labelEdit->text()); });

    updateBorder();
}

void ThumbnailItemWidget::setImage(const QImage &image)
{
    m_image = image;
    updatePixmap();
}

void ThumbnailItemWidget::setThumbnailSize(int size, Qt::Orientation orientation)
{
    size = qMax(40, size);
    if (size == m_size && orientation == m_orientation) {
        return;
    }
    m_size = size;
    m_orientation = orientation;
    updatePixmap();
}

void ThumbnailItemWidget::updatePixmap()
{
    // Pages are usually portrait; used only as a placeholder guess before
    // the real image arrives, so an exact aspect ratio doesn't matter.
    constexpr qreal kAssumedAspect = 1.3; // height / width

    if (m_image.isNull()) {
        const QSize placeholder = m_orientation == Qt::Horizontal
            ? QSize(qRound(m_size / kAssumedAspect), m_size)
            : QSize(m_size, qRound(m_size * kAssumedAspect));
        m_imageLabel->setFixedSize(placeholder);
        m_imageLabel->setPixmap({});
        updateBorder(); // CSS border, just for this brief pre-thumbnail state
        return;
    }
    QPixmap scaled = m_orientation == Qt::Horizontal
        ? QPixmap::fromImage(m_image).scaledToHeight(m_size, Qt::SmoothTransformation)
        : QPixmap::fromImage(m_image).scaledToWidth(m_size, Qt::SmoothTransformation);
    if (m_bookmarked) {
        drawBookmarkRibbon(scaled);
    }
    // The border is baked into the pixmap itself below (see roundedThumbnail),
    // so clear any leftover CSS border from the placeholder state above.
    m_imageLabel->setStyleSheet(QString());
    m_imageLabel->setPixmap(roundedThumbnail(scaled, m_current));
    m_imageLabel->setFixedSize(scaled.size());
}

void ThumbnailItemWidget::setCurrent(bool current)
{
    if (current == m_current) {
        return;
    }
    m_current = current;
    updatePixmap();
}

void ThumbnailItemWidget::setBookmarked(bool bookmarked)
{
    if (bookmarked == m_bookmarked) {
        return;
    }
    m_bookmarked = bookmarked;
    updatePixmap();
}

void ThumbnailItemWidget::setDisplayPosition(int displayPositionZeroBased)
{
    m_displayPosition = displayPositionZeroBased;
    m_pageNumberLabel->setText(QString::number(m_displayPosition + 1));
}

void ThumbnailItemWidget::setLabel(const QString &label)
{
    if (label != m_labelEdit->text()) {
        m_labelEdit->setText(label);
    }
}

QString ThumbnailItemWidget::label() const
{
    return m_labelEdit->text();
}

void ThumbnailItemWidget::updateBorder()
{
    const QString border = m_current ? "3px solid #3078ff" : "2px solid #999999";
    m_imageLabel->setStyleSheet(
        QString("background: white; border: %1; border-radius: %2px;").arg(border).arg(kThumbnailCornerRadius));
}

void ThumbnailItemWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        // Explicit rather than relying on Qt's automatic click-to-focus —
        // the actual click target under the cursor is the (NoFocus) image
        // label, not this widget itself, so the automatic mechanism isn't
        // reliably triggered by a click anywhere on the thumbnail.
        setFocus();
        emit activated(m_sourcePageIndex);
    }
    QWidget::mousePressEvent(event);
}

void ThumbnailItemWidget::keyPressEvent(QKeyEvent *event)
{
    // Either axis steps through the (one-dimensional) page sequence,
    // regardless of which way the panel happens to be docked right now.
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
        emit adjacentPageRequested(-1);
        event->accept();
        return;
    case Qt::Key_Right:
    case Qt::Key_Down:
        emit adjacentPageRequested(1);
        event->accept();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        emit deleteRequested(m_displayPosition);
        event->accept();
        return;
    default:
        break;
    }
    if (event->matches(QKeySequence::Copy)) {
        emit copyRequested(m_displayPosition);
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        emit pasteRequested(m_displayPosition);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ThumbnailItemWidget::contextMenuEvent(QContextMenuEvent *event)
{
    emit contextMenuRequested(m_displayPosition, event->globalPos());
}

void ThumbnailItemWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton)
        || (event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    auto *drag = new QDrag(this);
    auto *mime = new QMimeData();
    // Display position, not source page index — a duplicated page has two
    // items sharing one source index, and only the display position
    // unambiguously identifies which occurrence is actually being dragged.
    mime->setData(kPageMimeType, QByteArray::number(m_displayPosition));
    drag->setMimeData(mime);

    // A bare QDrag shows no visual at all while dragging — grab a
    // translucent snapshot of just the page image (not the page-number/
    // label fields below it, which would look odd tagging along) so it
    // visibly follows the cursor instead.
    const QPixmap grabbed = m_imageLabel->grab();
    QPixmap ghost(grabbed.size());
    ghost.fill(Qt::transparent);
    QPainter ghostPainter(&ghost);
    ghostPainter.setOpacity(0.7);
    ghostPainter.drawPixmap(0, 0, grabbed);
    ghostPainter.end();
    drag->setPixmap(ghost);
    drag->setHotSpot(m_imageLabel->mapFrom(this, event->pos()));

    drag->exec(Qt::MoveAction);
}

void ThumbnailItemWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(kPageMimeType)) {
        event->acceptProposedAction();
    }
}

void ThumbnailItemWidget::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(kPageMimeType)) {
        return;
    }
    bool ok = false;
    const int sourceDisplayPosition = event->mimeData()->data(kPageMimeType).toInt(&ok);
    if (!ok || sourceDisplayPosition == m_displayPosition) {
        event->acceptProposedAction();
        return;
    }
    const bool after = m_orientation == Qt::Horizontal ? event->position().x() > width() / 2.0
                                                          : event->position().y() > height() / 2.0;
    emit reorderRequested(sourceDisplayPosition, m_displayPosition, after);
    event->acceptProposedAction();
}

ThumbnailPanel::ThumbnailPanel(QWidget *parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_content = new QWidget(this);
    m_layout = new QBoxLayout(QBoxLayout::TopToBottom, m_content);
    m_layout->setContentsMargins(4, 4, 4, 4);
    m_layout->setSpacing(2);
    m_layout->addStretch(1);
    setWidget(m_content);
}

void ThumbnailPanel::setOrientation(Qt::Orientation orientation)
{
    if (orientation == m_orientation) {
        return;
    }
    m_orientation = orientation;
    m_layout->setDirection(orientation == Qt::Horizontal ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    setHorizontalScrollBarPolicy(orientation == Qt::Horizontal ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(orientation == Qt::Horizontal ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    relayoutWidths();
}

void ThumbnailPanel::clear()
{
    for (ThumbnailItemWidget *item : m_items) {
        item->deleteLater();
    }
    m_items.clear();
    m_currentSourcePage = -1;
    m_labelsBySourcePage.clear();
    m_bookmarkedSourcePages.clear();
}

ThumbnailItemWidget *ThumbnailPanel::createItem(int sourcePageIndex)
{
    auto *item = new ThumbnailItemWidget(sourcePageIndex, m_content);
    connect(item, &ThumbnailItemWidget::activated, this, &ThumbnailPanel::pageActivated);
    connect(item, &ThumbnailItemWidget::reorderRequested, this, &ThumbnailPanel::reorderRequested);
    connect(item, &ThumbnailItemWidget::labelEdited, this, [this](int sourcePage, const QString &label) {
        m_labelsBySourcePage.insert(sourcePage, label);
        emit labelEdited(sourcePage, label);
    });
    connect(item, &ThumbnailItemWidget::adjacentPageRequested, this, &ThumbnailPanel::adjacentPageRequested);
    connect(item, &ThumbnailItemWidget::deleteRequested, this, &ThumbnailPanel::deleteRequested);
    connect(item, &ThumbnailItemWidget::copyRequested, this, &ThumbnailPanel::copyRequested);
    connect(item, &ThumbnailItemWidget::pasteRequested, this, &ThumbnailPanel::pasteRequested);
    connect(item, &ThumbnailItemWidget::contextMenuRequested, this, &ThumbnailPanel::contextMenuRequested);
    return item;
}

void ThumbnailPanel::rebuildFromOrder(const QVector<int> &sourcePageIndices)
{
    // A lightweight teardown (unlike clear()) — this is "the same document,
    // just a different arrangement of its pages," so the per-source-page
    // caches (labels, bookmark flags, which page is current) deliberately
    // survive and get reapplied to the freshly-created items below.
    for (ThumbnailItemWidget *item : m_items) {
        item->deleteLater();
    }
    m_items.clear();
    m_items.reserve(sourcePageIndices.size());
    for (int displayPos = 0; displayPos < sourcePageIndices.size(); ++displayPos) {
        const int sourceIndex = sourcePageIndices[displayPos];
        ThumbnailItemWidget *item = createItem(sourceIndex);
        item->setDisplayPosition(displayPos);
        item->setLabel(m_labelsBySourcePage.value(sourceIndex));
        item->setBookmarked(m_bookmarkedSourcePages.contains(sourceIndex));
        item->setCurrent(sourceIndex == m_currentSourcePage);
        m_layout->insertWidget(m_layout->count() - 1, item);
        m_items.append(item);
    }
    relayoutWidths();
}

void ThumbnailPanel::setThumbnail(int sourcePageIndex, const QImage &image)
{
    for (ThumbnailItemWidget *item : m_items) {
        if (item->sourcePageIndex() == sourcePageIndex) {
            item->setImage(image);
        }
    }
}

void ThumbnailPanel::setCurrentSourcePage(int sourcePageIndex)
{
    m_currentSourcePage = sourcePageIndex;
    for (ThumbnailItemWidget *item : m_items) {
        item->setCurrent(item->sourcePageIndex() == sourcePageIndex);
    }
}

void ThumbnailPanel::setBookmarkedPages(const QSet<int> &sourcePageIndices)
{
    m_bookmarkedSourcePages = sourcePageIndices;
    for (ThumbnailItemWidget *item : m_items) {
        item->setBookmarked(m_bookmarkedSourcePages.contains(item->sourcePageIndex()));
    }
}

int ThumbnailPanel::focusedDisplayPosition() const
{
    for (ThumbnailItemWidget *item : m_items) {
        if (item->hasFocus()) {
            return item->displayPosition();
        }
    }
    return -1;
}

void ThumbnailPanel::relayoutWidths()
{
    // Vertical: fill the available width, scroll vertically (each item's
    // height follows from its own aspect ratio). Horizontal: fill the
    // available height minus room for the label row below each image (the
    // page number is an overlay badge, not its own row), scroll sideways
    // instead.
    const int size = m_orientation == Qt::Horizontal ? qMax(40, viewport()->height() - 56)
                                                       : qMax(40, viewport()->width() - 24);
    for (ThumbnailItemWidget *item : m_items) {
        item->setThumbnailSize(size, m_orientation);
    }
}

void ThumbnailPanel::resizeEvent(QResizeEvent *event)
{
    QScrollArea::resizeEvent(event);
    relayoutWidths();
}
