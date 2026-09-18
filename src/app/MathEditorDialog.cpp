#include "app/MathEditorDialog.h"

#include <atomic>

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "app/MathPreviewRenderer.h"
#include "app/Theme.h"
#include "app/math/MathRenderService.h"
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
    // P0-07: the preview renders on the shared worker thread. Closing the
    // dialog destroys this object, and the service's QPointer guard then
    // discards any reply that was still in flight.
    static std::atomic<std::uint64_t> next_dialog_id{0};
    const QString dialog_id =
        QStringLiteral("math-dialog-%1").arg(next_dialog_id.fetch_add(1));
    setObjectName(dialog_id);
    MathRenderService* service = MathRenderService::Shared();
    service->RegisterClient(dialog_id, this);
    connect(service, &MathRenderService::mathRendered, this,
            [this](const MathRenderResponse& response) {
                if (response.editor_id != objectName())
                    return;
                // The dialog re-validates on its own; only apply the pixels.
                ApplyRenderedPreview(
                    response.latex, response.result.image,
                    response.result.width, response.result.height,
                    response.result.baseline,
                    response.result.device_pixel_ratio, response.result.note,
                    response.result.exact);
            });
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
    // Invalid/incomplete source is kept visible through the bounded fallback;
    // only a valid body is sent to the real TeX renderer.
    if (!validation.valid()) {
        style.backend = MathRenderBackend::ApproximateOnly;
    }
    // P0-07: enqueue instead of blocking the GUI thread on TeX. The debounce
    // above only limits how many requests are issued; it no longer runs TeX.
    preview_->setText(QStringLiteral("…"));
    preview_->setPixmap(QPixmap());
    ++preview_generation_;
    MathRenderService::Shared()->Request(objectName(),
                                         QStringLiteral("preview"), body,
                                         style);
}

void MathEditorDialog::ApplyRenderedPreview(const QString& latex,
                                            const QImage& image, int width,
                                            int height, int baseline,
                                            qreal device_pixel_ratio,
                                            const QString& note, bool exact) {
    if (!preview_ || !status_ || image.isNull())
        return;
    // The dialog scales the pixmap through the label, so the logical metrics
    // are only needed to reject an empty render.
    if (width <= 0 || height <= 0 || baseline < 0)
        return;
    // A reply for a stale body must not replace the current one.
    if (source_ && latex != source_->toPlainText())
        return;
    MathRenderResult partial;
    partial.image = image;
    partial.device_pixel_ratio = device_pixel_ratio;
    const QPixmap pixmap = PixmapFromMathResult(partial);
    if (pixmap.isNull()) {
        preview_->setText(QStringLiteral("?"));
        return;
    }
    preview_->setText(QString());
    preview_->setPixmap(pixmap);
    if (!exact && !note.isEmpty()) {
        status_->setText(status_->text() + QStringLiteral("  (%1)").arg(note));
    }
}

}  // namespace pf::gui
