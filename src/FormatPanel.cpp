#include "FormatPanel.h"
#include "PdfView.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSlider>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextList>
#include <QToolButton>

#include <cmath>

FormatPanel::FormatPanel(PdfView *view, QWidget *parent)
    : QWidget(parent)
    , m_view(view)
{
    buildUi();
    if (m_view) {
        connect(m_view, &PdfView::textBoxStateChanged, this, &FormatPanel::refresh);
    }
    refresh();
}

void FormatPanel::setView(PdfView *view)
{
    if (view == m_view) {
        return;
    }
    if (m_view) {
        disconnect(m_view, &PdfView::textBoxStateChanged, this, &FormatPanel::refresh);
    }
    m_view = view;
    if (m_view) {
        connect(m_view, &PdfView::textBoxStateChanged, this, &FormatPanel::refresh);
    }
    refresh();
}

QIcon FormatPanel::colorIcon(const QColor &color)
{
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setPen(QColor(120, 120, 120));
    p.setBrush(color);
    p.drawRect(1, 1, 13, 13);
    return QIcon(pix);
}

void FormatPanel::buildUi()
{
    // Laid out as one horizontal row (meant to live in a top toolbar,
    // spreading every control out where it's all visible at once, rather
    // than the stacked/multi-row layout a narrow side panel would need).
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(4, 2, 4, 2);
    outer->setSpacing(6);

    m_hintLabel = new QLabel(tr("Place a text box or shape, or select one, to format it."), this);
    outer->addWidget(m_hintLabel);

    // --- Text formatting group ---
    m_textGroup = new QWidget(this);
    auto *textLayout = new QHBoxLayout(m_textGroup);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(4);

    m_fontCombo = new QFontComboBox(m_textGroup);
    m_fontCombo->setFixedWidth(130);
    textLayout->addWidget(m_fontCombo);

    m_sizeCombo = new QComboBox(m_textGroup);
    m_sizeCombo->setEditable(true);
    m_sizeCombo->addItems({"8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "32", "36", "48", "72"});
    m_sizeCombo->setFixedWidth(55);
    textLayout->addWidget(m_sizeCombo);

    m_boldButton = new QToolButton(m_textGroup);
    m_boldButton->setText(tr("B"));
    m_boldButton->setCheckable(true);
    QFont boldFont = m_boldButton->font();
    boldFont.setBold(true);
    m_boldButton->setFont(boldFont);
    textLayout->addWidget(m_boldButton);

    m_italicButton = new QToolButton(m_textGroup);
    m_italicButton->setText(tr("I"));
    m_italicButton->setCheckable(true);
    QFont italicFont = m_italicButton->font();
    italicFont.setItalic(true);
    m_italicButton->setFont(italicFont);
    textLayout->addWidget(m_italicButton);

    m_underlineButton = new QToolButton(m_textGroup);
    m_underlineButton->setText(tr("U"));
    m_underlineButton->setCheckable(true);
    QFont underlineFont = m_underlineButton->font();
    underlineFont.setUnderline(true);
    m_underlineButton->setFont(underlineFont);
    textLayout->addWidget(m_underlineButton);

    m_textColorButton = new QToolButton(m_textGroup);
    m_textColorButton->setToolTip(tr("Text Color"));
    m_textColorButton->setIcon(colorIcon(m_currentTextColor));
    textLayout->addWidget(m_textColorButton);

    m_highlightColorButton = new QToolButton(m_textGroup);
    m_highlightColorButton->setToolTip(tr("Highlight Color"));
    m_highlightColorButton->setIcon(colorIcon(m_currentHighlightColor));
    textLayout->addWidget(m_highlightColorButton);

    textLayout->addSpacing(8);

    m_alignLeftButton = new QToolButton(m_textGroup);
    m_alignLeftButton->setText(tr("⟸"));
    m_alignLeftButton->setToolTip(tr("Align Left"));
    m_alignLeftButton->setCheckable(true);
    textLayout->addWidget(m_alignLeftButton);

    m_alignCenterButton = new QToolButton(m_textGroup);
    m_alignCenterButton->setText(tr("≡"));
    m_alignCenterButton->setToolTip(tr("Align Center"));
    m_alignCenterButton->setCheckable(true);
    textLayout->addWidget(m_alignCenterButton);

    m_alignRightButton = new QToolButton(m_textGroup);
    m_alignRightButton->setText(tr("⟹"));
    m_alignRightButton->setToolTip(tr("Align Right"));
    m_alignRightButton->setCheckable(true);
    textLayout->addWidget(m_alignRightButton);

    m_alignJustifyButton = new QToolButton(m_textGroup);
    m_alignJustifyButton->setText(tr("☰"));
    m_alignJustifyButton->setToolTip(tr("Justify"));
    m_alignJustifyButton->setCheckable(true);
    textLayout->addWidget(m_alignJustifyButton);

    textLayout->addSpacing(8);

    m_bulletButton = new QToolButton(m_textGroup);
    m_bulletButton->setText(tr("• List"));
    m_bulletButton->setCheckable(true);
    textLayout->addWidget(m_bulletButton);

    m_numberButton = new QToolButton(m_textGroup);
    m_numberButton->setText(tr("1. List"));
    m_numberButton->setCheckable(true);
    textLayout->addWidget(m_numberButton);

    outer->addWidget(m_textGroup);

    auto *sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    outer->addWidget(sep1);

    // --- Box chrome group --- (title updated in refresh() to say "Text
    // Box" or "Shape" depending on what's selected)
    m_boxGroup = new QGroupBox(tr("Box"), this);
    auto *boxLayout = new QHBoxLayout(m_boxGroup);
    boxLayout->setContentsMargins(6, 2, 6, 2);
    boxLayout->setSpacing(4);

    m_fitBoxButton = new QToolButton(m_boxGroup);
    m_fitBoxButton->setText(tr("Fit Box to Text"));
    m_fitBoxButton->setToolTip(tr("Resize the box height to match its current content"));
    boxLayout->addWidget(m_fitBoxButton);

    m_shrinkTextButton = new QToolButton(m_boxGroup);
    m_shrinkTextButton->setText(tr("Size Text to Box"));
    m_shrinkTextButton->setToolTip(tr("Shrink the text so it fits within the current box size"));
    boxLayout->addWidget(m_shrinkTextButton);

    boxLayout->addSpacing(8);
    boxLayout->addWidget(new QLabel(tr("Border:"), m_boxGroup));
    m_borderColorButton = new QToolButton(m_boxGroup);
    m_borderColorButton->setIcon(colorIcon(m_currentBorderColor));
    boxLayout->addWidget(m_borderColorButton);
    m_borderWidthSpin = new QDoubleSpinBox(m_boxGroup);
    m_borderWidthSpin->setRange(0.0, 20.0);
    m_borderWidthSpin->setSingleStep(0.5);
    m_borderWidthSpin->setSuffix(tr(" pt"));
    m_borderWidthSpin->setFixedWidth(70);
    boxLayout->addWidget(m_borderWidthSpin);

    boxLayout->addSpacing(8);
    boxLayout->addWidget(new QLabel(tr("Fill:"), m_boxGroup));
    m_fillColorButton = new QToolButton(m_boxGroup);
    m_fillColorButton->setIcon(colorIcon(m_currentFillColor));
    boxLayout->addWidget(m_fillColorButton);
    m_fillOpacitySlider = new QSlider(Qt::Horizontal, m_boxGroup);
    m_fillOpacitySlider->setRange(0, 100);
    m_fillOpacitySlider->setFixedWidth(90);
    boxLayout->addWidget(m_fillOpacitySlider);

    outer->addWidget(m_boxGroup);
    outer->addStretch(1);

    // --- Wiring ---
    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
        if (m_updating) {
            return;
        }
        if (auto *editor = m_view->beginFormattingSelectedTextBox()) {
            editor->setFontFamily(font.family());
            m_view->refocusActiveEditor();
        }
    });
    connect(m_sizeCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (m_updating) {
            return;
        }
        bool ok = false;
        const double size = text.toDouble(&ok);
        if (!ok || size <= 0) {
            return;
        }
        if (auto *editor = m_view->beginFormattingSelectedTextBox()) {
            editor->setFontPointSize(size);
            m_view->refocusActiveEditor();
        }
    });
    connect(m_boldButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_updating) {
            return;
        }
        if (auto *editor = m_view->beginFormattingSelectedTextBox()) {
            editor->setFontWeight(checked ? QFont::Bold : QFont::Normal);
            m_view->refocusActiveEditor();
        }
    });
    connect(m_italicButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_updating) {
            return;
        }
        if (auto *editor = m_view->beginFormattingSelectedTextBox()) {
            editor->setFontItalic(checked);
            m_view->refocusActiveEditor();
        }
    });
    connect(m_underlineButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_updating) {
            return;
        }
        if (auto *editor = m_view->beginFormattingSelectedTextBox()) {
            editor->setFontUnderline(checked);
            m_view->refocusActiveEditor();
        }
    });
    connect(m_textColorButton, &QToolButton::clicked, this, [this]() {
        auto *editor = m_view->beginFormattingSelectedTextBox();
        if (!editor) {
            return;
        }
        const QColor chosen = QColorDialog::getColor(m_currentTextColor, this, tr("Text Color"));
        if (chosen.isValid()) {
            editor->setTextColor(chosen);
            m_view->refocusActiveEditor();
        }
    });
    connect(m_highlightColorButton, &QToolButton::clicked, this, [this]() {
        auto *editor = m_view->beginFormattingSelectedTextBox();
        if (!editor) {
            return;
        }
        const QColor chosen = QColorDialog::getColor(m_currentHighlightColor, this, tr("Highlight Color"),
                                                       QColorDialog::ShowAlphaChannel);
        if (chosen.isValid()) {
            editor->setTextBackgroundColor(chosen);
            m_view->refocusActiveEditor();
        }
    });

    const auto wireAlign = [this](QToolButton *button, Qt::Alignment alignment) {
        connect(button, &QToolButton::clicked, this, [this, alignment]() {
            if (auto *editor = m_view->beginFormattingSelectedTextBox()) {
                editor->setAlignment(alignment);
                m_view->refocusActiveEditor();
            }
        });
    };
    wireAlign(m_alignLeftButton, Qt::AlignLeft);
    wireAlign(m_alignCenterButton, Qt::AlignHCenter);
    wireAlign(m_alignRightButton, Qt::AlignRight);
    wireAlign(m_alignJustifyButton, Qt::AlignJustify);

    connect(m_bulletButton, &QToolButton::clicked, this, [this]() { toggleList(QTextListFormat::ListDisc); });
    connect(m_numberButton, &QToolButton::clicked, this, [this]() { toggleList(QTextListFormat::ListDecimal); });

    connect(m_borderColorButton, &QToolButton::clicked, this, [this]() {
        if (!m_view->hasSelectedStyleable()) {
            return;
        }
        const QColor chosen = QColorDialog::getColor(m_currentBorderColor, this, tr("Border Color"));
        if (chosen.isValid()) {
            m_view->setSelectedBorder(chosen, m_borderWidthSpin->value());
        }
    });
    connect(m_borderWidthSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (m_updating || !m_view->hasSelectedStyleable()) {
            return;
        }
        m_view->setSelectedBorder(m_currentBorderColor, value);
    });
    connect(m_fillColorButton, &QToolButton::clicked, this, [this]() {
        if (!m_view->hasSelectedStyleable()) {
            return;
        }
        const QColor chosen = QColorDialog::getColor(m_currentFillColor, this, tr("Fill Color"));
        if (chosen.isValid()) {
            m_view->setSelectedFill(chosen, m_fillOpacitySlider->value() / 100.0);
        }
    });
    connect(m_fillOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updating || !m_view->hasSelectedStyleable()) {
            return;
        }
        m_view->setSelectedFill(m_currentFillColor, value / 100.0);
    });

    connect(m_fitBoxButton, &QToolButton::clicked, this, [this]() {
        m_view->fitSelectedTextBoxToContent();
        m_view->refocusActiveEditor();
    });
    connect(m_shrinkTextButton, &QToolButton::clicked, this, [this]() {
        m_view->shrinkSelectedTextBoxToFit();
        m_view->refocusActiveEditor();
    });
}

void FormatPanel::toggleList(int style)
{
    auto *editor = m_view->beginFormattingSelectedTextBox();
    if (!editor) {
        return;
    }
    QTextCursor cursor = editor->textCursor();
    cursor.beginEditBlock();
    QTextList *list = cursor.currentList();
    const auto wantedStyle = static_cast<QTextListFormat::Style>(style);
    if (list && list->format().style() == wantedStyle) {
        QTextBlockFormat blockFormat = cursor.blockFormat();
        blockFormat.setIndent(0);
        cursor.setBlockFormat(blockFormat);
        list->remove(cursor.block());
    } else {
        QTextListFormat format;
        format.setStyle(wantedStyle);
        cursor.createList(format);
    }
    cursor.endEditBlock();
    m_view->refocusActiveEditor();
}

void FormatPanel::refresh()
{
    m_updating = true;

    // No tab open at all (e.g. the last one was just closed) — nothing to
    // show controls for.
    if (!m_view) {
        m_hintLabel->setVisible(true);
        m_textGroup->setEnabled(false);
        m_boxGroup->setEnabled(false);
        m_fitBoxButton->setEnabled(false);
        m_shrinkTextButton->setEnabled(false);
        m_fillColorButton->setEnabled(false);
        m_fillOpacitySlider->setEnabled(false);
        m_updating = false;
        return;
    }

    // Deliberately reads via activeTextEditor() (which may be null) rather
    // than beginFormattingSelectedTextBox() — refresh() just displays
    // current state and must not have the side effect of starting an edit
    // session merely because the panel repainted.
    auto *editor = m_view->activeTextEditor();
    const bool hasTextBox = m_view->hasSelectedTextBox();
    const bool hasStyleable = m_view->hasSelectedStyleable();

    m_hintLabel->setVisible(!editor && !hasStyleable);
    // Enabled whenever a text box is selected, even without an open editor
    // — that's what lets you select a box and change its whole-block font/
    // size/color without double-clicking in first.
    m_textGroup->setEnabled(editor != nullptr || hasTextBox);
    m_boxGroup->setEnabled(hasStyleable);
    m_boxGroup->setTitle(m_view->hasSelectedShape() ? tr("Shape") : tr("Text Box"));
    // Fit/Shrink act on text content, so they're meaningless for a shape
    // even though the rest of the Box group (border/fill) applies to both.
    m_fitBoxButton->setEnabled(hasTextBox);
    m_shrinkTextButton->setEnabled(hasTextBox);
    // Lines/arrows have no interior to fill.
    const bool fillApplicable = hasStyleable && !m_view->hasSelectedLineLikeShape();
    m_fillColorButton->setEnabled(fillApplicable);
    m_fillOpacitySlider->setEnabled(fillApplicable);

    if (editor) {
        m_fontCombo->setCurrentFont(QFont(editor->fontFamily()));
        m_sizeCombo->setCurrentText(QString::number(editor->fontPointSize() > 0 ? editor->fontPointSize() : 14));
        m_boldButton->setChecked(editor->fontWeight() > QFont::Normal);
        m_italicButton->setChecked(editor->fontItalic());
        m_underlineButton->setChecked(editor->fontUnderline());
        m_currentTextColor = editor->textColor();
        m_textColorButton->setIcon(colorIcon(m_currentTextColor));
        m_currentHighlightColor = editor->textBackgroundColor();
        m_highlightColorButton->setIcon(colorIcon(m_currentHighlightColor));

        const Qt::Alignment align = editor->alignment();
        m_alignLeftButton->setChecked(align & Qt::AlignLeft);
        m_alignCenterButton->setChecked(align & Qt::AlignHCenter);
        m_alignRightButton->setChecked(align & Qt::AlignRight);
        m_alignJustifyButton->setChecked(align & Qt::AlignJustify);

        QTextList *list = editor->textCursor().currentList();
        const bool isBullet = list && list->format().style() == QTextListFormat::ListDisc;
        const bool isNumbered = list && list->format().style() == QTextListFormat::ListDecimal;
        m_bulletButton->setChecked(isBullet);
        m_numberButton->setChecked(isNumbered);
    } else if (hasTextBox) {
        // Selected but not being edited — peek at its stored formatting
        // without starting an edit session just to display it.
        const PdfView::TextFormatSnapshot snap = m_view->selectedTextBoxFormatSnapshot();
        m_fontCombo->setCurrentFont(QFont(snap.fontFamily));
        m_sizeCombo->setCurrentText(QString::number(snap.fontPointSize));
        m_boldButton->setChecked(snap.bold);
        m_italicButton->setChecked(snap.italic);
        m_underlineButton->setChecked(snap.underline);
        m_currentTextColor = snap.textColor;
        m_textColorButton->setIcon(colorIcon(m_currentTextColor));
        if (snap.highlightColor.alpha() > 0) {
            m_currentHighlightColor = snap.highlightColor;
        }
        m_highlightColorButton->setIcon(colorIcon(m_currentHighlightColor));

        m_alignLeftButton->setChecked(snap.alignment & Qt::AlignLeft);
        m_alignCenterButton->setChecked(snap.alignment & Qt::AlignHCenter);
        m_alignRightButton->setChecked(snap.alignment & Qt::AlignRight);
        m_alignJustifyButton->setChecked(snap.alignment & Qt::AlignJustify);

        // Bullet/numbered-list state isn't tracked in the snapshot (would
        // need a live QTextCursor) — leave the two buttons unchecked rather
        // than guessing.
        m_bulletButton->setChecked(false);
        m_numberButton->setChecked(false);
    }

    if (hasStyleable) {
        m_currentBorderColor = m_view->selectedBorderColor();
        m_borderColorButton->setIcon(colorIcon(m_currentBorderColor));
        m_borderWidthSpin->setValue(m_view->selectedBorderWidthPts());
        m_currentFillColor = m_view->selectedFillColor();
        m_fillColorButton->setIcon(colorIcon(m_currentFillColor));
        m_fillOpacitySlider->setValue(int(std::round(m_view->selectedFillOpacity() * 100.0)));
    }

    m_updating = false;
}
