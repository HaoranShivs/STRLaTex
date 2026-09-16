#include "app/MathEditorDialog.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "app/MathPreviewRenderer.h"
#include "app/Theme.h"
#include "math/MathValidator.h"

namespace pf::gui {

namespace {

constexpr int kPreviewFontPx = 22;
constexpr int kDebounceMs = 140;

}  // namespace

MathEditorDialog::MathEditorDialog(const QString& latex, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Inline Math"));
    setModal(true);
    resize(520, 380);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* source_label = new QLabel(QStringLiteral("LaTeX source"), this);
    source_label->setStyleSheet(
        QString("color: %1; font-weight: 600;").arg(theme::kSecondaryText));
    layout->addWidget(source_label);

    source_ = new QPlainTextEdit(this);
    source_->setPlainText(latex);
    source_->setPlaceholderText(QStringLiteral("\\frac{a}{b}"));
    source_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    source_->setFixedHeight(96);
    source_->setTabChangesFocus(false);
    layout->addWidget(source_);

    auto* preview_label = new QLabel(QStringLiteral("Preview"), this);
    preview_label->setStyleSheet(
        QString("color: %1; font-weight: 600;").arg(theme::kSecondaryText));
    layout->addWidget(preview_label);

    preview_ = new QLabel(this);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumHeight(96);
    preview_->setStyleSheet(
        QString("background: %1; border: 1px solid %2; border-radius: 6px;")
            .arg(theme::kEditorBackground, theme::kDivider));
    layout->addWidget(preview_, 1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Insert"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);
    connect(debounce_, &QTimer::timeout, this,
            [this]() { RefreshPreview(); });
    // Live (debounced) preview: source changed -> validation -> render.
    connect(source_, &QPlainTextEdit::textChanged, this,
            [this]() { debounce_->start(); });

    RefreshPreviewNow();
    source_->setFocus();
    source_->selectAll();
}

QString MathEditorDialog::latex() const {
    return source_ ? source_->toPlainText() : QString();
}

void MathEditorDialog::SetSourceForTest(const QString& latex) {
    if (source_) source_->setPlainText(latex);
    RefreshPreviewNow();
}

QString MathEditorDialog::StateText() const {
    return status_ ? status_->text() : QString();
}

void MathEditorDialog::RefreshPreviewNow() {
    if (debounce_) debounce_->stop();
    RefreshPreview();
}

void MathEditorDialog::RefreshPreview() {
    if (!source_ || !preview_ || !status_) return;
    const QString body = source_->toPlainText();
    const pf::MathValidation validation =
        pf::ValidateMath(body.toStdString(), pf::MathFlavor::Inline);

    if (validation.invalid()) {
        status_->setText(QStringLiteral("Invalid — %1")
                             .arg(QString::fromStdString(validation.error)));
        status_->setStyleSheet(QString("color: %1;").arg(theme::kError));
    } else if (validation.pending()) {
        status_->setText(QStringLiteral("Type a math body, e.g. \\frac{a}{b}"));
        status_->setStyleSheet(
            QString("color: %1;").arg(theme::kSecondaryText));
    } else {
        status_->setText(QStringLiteral("Valid"));
        status_->setStyleSheet(QString("color: %1;").arg(theme::kAccent));
    }

    MathRenderStyle style;
    style.font_px = kPreviewFontPx;
    const MathRenderResult rendered = RenderMathPreview(body, style);
    if (rendered.pixmap.isNull()) {
        preview_->setPixmap(QPixmap());
        preview_->setText(validation.pending() ? QStringLiteral("—")
                                               : QStringLiteral("?"));
        return;
    }
    preview_->setText(QString());
    preview_->setPixmap(rendered.pixmap);
    if (!rendered.exact && !rendered.note.isEmpty()) {
        status_->setText(status_->text() +
                         QStringLiteral("  (%1)").arg(rendered.note));
    }
}

}  // namespace pf::gui
