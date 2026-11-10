#pragma once

#include <QColor>
#include <QIcon>
#include <QWidget>

class PdfView;
class QFontComboBox;
class QComboBox;
class QToolButton;
class QDoubleSpinBox;
class QSlider;
class QLabel;
class QGroupBox;

// Side-panel formatting toolbar for the "Add Text" tool's text boxes: font
// family/size, bold/italic/underline, text/highlight color, alignment,
// bullet/numbered lists, and the text box's own border/fill/transparency.
// Acts directly on PdfView's active rich-text editor (QTextEdit already has
// public slots for all of this — setFontBold, setTextColor, etc. — so this
// panel is mostly just UI wiring, not a reimplementation) and on the
// selected TextBoxObject's style fields for the box chrome.
class FormatPanel : public QWidget {
    Q_OBJECT

public:
    explicit FormatPanel(PdfView *view, QWidget *parent = nullptr);
    // Rebinds this panel to a different PdfView (e.g. the newly active tab)
    // — disconnects from the old one, connects to the new one, and
    // refreshes immediately so the controls match the new view's state.
    void setView(PdfView *view);

public slots:
    // Call whenever PdfView::textBoxStateChanged fires: re-reads the active
    // editor's current formatting (or the selected box's style) and updates
    // every control to match, enabling/disabling groups as appropriate.
    void refresh();

private:
    void buildUi();
    static QIcon colorIcon(const QColor &color);
    void toggleList(int style);

    PdfView *m_view;
    bool m_updating = false; // guards against refresh()'s own setChecked etc. re-triggering handlers

    QFontComboBox *m_fontCombo = nullptr;
    QComboBox *m_sizeCombo = nullptr;
    QToolButton *m_boldButton = nullptr;
    QToolButton *m_italicButton = nullptr;
    QToolButton *m_underlineButton = nullptr;
    QToolButton *m_textColorButton = nullptr;
    QToolButton *m_highlightColorButton = nullptr;
    QToolButton *m_alignLeftButton = nullptr;
    QToolButton *m_alignCenterButton = nullptr;
    QToolButton *m_alignRightButton = nullptr;
    QToolButton *m_alignJustifyButton = nullptr;
    QToolButton *m_bulletButton = nullptr;
    QToolButton *m_numberButton = nullptr;
    QToolButton *m_fitBoxButton = nullptr;
    QToolButton *m_shrinkTextButton = nullptr;

    QToolButton *m_borderColorButton = nullptr;
    QDoubleSpinBox *m_borderWidthSpin = nullptr;
    QToolButton *m_fillColorButton = nullptr;
    QSlider *m_fillOpacitySlider = nullptr;

    QWidget *m_textGroup = nullptr;
    QGroupBox *m_boxGroup = nullptr;
    QLabel *m_hintLabel = nullptr;

    QColor m_currentTextColor = Qt::black;
    QColor m_currentHighlightColor = Qt::yellow;
    QColor m_currentBorderColor = Qt::black;
    QColor m_currentFillColor = Qt::white;
};
