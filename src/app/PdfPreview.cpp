#include "app/PdfPreview.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "app/Theme.h"

namespace pf::gui {

namespace {

constexpr double kPointsPerInch = 72.0;
constexpr int kBaselineDpi = 96; // 100% 缩放
constexpr int kMinRenderDpi = 48;
// 整篇论文按页驻留内存，因此光栅分辨率上限比单页查看器所需值更低。
constexpr int kMaxRenderDpi = 300;
constexpr int kRerenderDelayMs = 140;
// 视口之外仍保持驻留的页数（150 DPI 下一页约 8 MB）。
constexpr int kKeepPagesAround = 2;
constexpr int kPageGap = 14; // 两页之间的接缝

QString RunCapture(const QString& program, const QStringList& arguments) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(15000)) {
        process.kill();
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

// 通过 pdfinfo 获取的页数；无法确定时返回 0。
int QueryPageCount(const QString& pdf_path) {
    const QString output = RunCapture("pdfinfo", {pdf_path});
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("Pages:"))) {
            bool ok = false;
            const int count = line.mid(QStringLiteral("Pages:").size()).trimmed().toInt(&ok);
            if (ok && count > 0)
                return count;
        }
    }
    return 0;
}

// 用 pdftoppm 将单页光栅化。失败时返回空 pixmap。
QPixmap RenderPageToPixmap(const QString& pdf_path, int page, int dpi, double* width_pt, double* height_pt) {
    QTemporaryDir dir;
    if (!dir.isValid())
        return {};
    QProcess process;
    process.start("pdftoppm", {"-png", "-r", QString::number(dpi), "-f", QString::number(page), "-l",
                               QString::number(page), pdf_path, dir.path() + "/page"});
    if (!process.waitForFinished(30000)) {
        process.kill();
        return {};
    }
    // pdftoppm 会为页数较多的文档补零页码，因此两种写法都要尝试。
    const QString prefix = dir.path() + "/page";
    QString png;
    for (const char* form : {"-%1.png", "-%01.png", "-%02.png", "-%03.png"}) {
        const QString candidate = prefix + QString::fromLatin1(form).arg(page);
        if (QFileInfo::exists(candidate))
            png = candidate;
    }
    if (png.isEmpty())
        return {};
    QPixmap pixmap(png); // 在临时目录被删除前完成加载
    if (pixmap.isNull())
        return {};
    if (page == 1 && width_pt && height_pt) {
        *width_pt = pixmap.width() / static_cast<double>(dpi) * kPointsPerInch;
        *height_pt = pixmap.height() / static_cast<double>(dpi) * kPointsPerInch;
    }
    return pixmap;
}

QPushButton* ToolButton(const QString& text, const QString& tip, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setStyleSheet(QString("QPushButton { border: none; background: transparent; color: %1;"
                                  " padding: 2px 6px; font-size: 11pt; }"
                                  "QPushButton:hover { background: %2; border-radius: 4px; }")
                              .arg(theme::kSecondaryText, theme::kAccentSoft));
    return button;
}

} // namespace

PdfPreview::PdfPreview(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName("pdfPreviewHeader");
    header->setStyleSheet(QString("QWidget#pdfPreviewHeader { background: %1;"
                                  " border-bottom: 1px solid %2; }")
                              .arg(theme::kSidePanel, theme::kDivider));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(10, 3, 8, 3);
    header_layout->setSpacing(2);

    auto* title = new QLabel("PREVIEW", header);
    title->setStyleSheet(QString("color: %1; font-size: 8pt;"
                                 " font-weight: 700; letter-spacing: 1px;")
                             .arg(theme::kSecondaryText));
    header_layout->addWidget(title);
    page_indicator_ = new QLabel(QString(), header);
    page_indicator_->setStyleSheet(QString("color: %1; font-size: 8pt; margin-left: 6px;").arg(theme::kDisabledText));
    header_layout->addWidget(page_indicator_);
    header_layout->addStretch(1);

    auto* zoom_out = ToolButton("−", "Zoom out (wheel down)", header);
    auto* zoom_in = ToolButton("+", "Zoom in (wheel up)", header);
    auto* fit_width = ToolButton("⤢", "Fit width", header);
    auto* reset = ToolButton("1:1", "Reset to 100%", header);
    zoom_label_ = new QLabel("100%", header);
    zoom_label_->setMinimumWidth(44);
    zoom_label_->setAlignment(Qt::AlignCenter);
    zoom_label_->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(theme::kSecondaryText));

    header_layout->addWidget(zoom_out);
    header_layout->addWidget(zoom_label_);
    header_layout->addWidget(zoom_in);
    header_layout->addWidget(fit_width);
    header_layout->addWidget(reset);
    layout->addWidget(header);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(false);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    scroll_->setBackgroundRole(QPalette::Window);
    scroll_->setStyleSheet(QString("QScrollArea { background: %1; }").arg(theme::kSidePanel));
    scroll_->viewport()->installEventFilter(this);

    column_ = new QWidget(scroll_);
    column_->setObjectName("pdfPageColumn");
    column_->setStyleSheet(QString("QWidget#pdfPageColumn { background: %1; }").arg(theme::kSidePanel));
    column_layout_ = new QVBoxLayout(column_);
    column_layout_->setContentsMargins(0, 0, 0, 0);
    column_layout_->setSpacing(kPageGap);
    scroll_->setWidget(column_);
    layout->addWidget(scroll_, 1);

    connect(zoom_in, &QPushButton::clicked, this, &PdfPreview::ZoomIn);
    connect(zoom_out, &QPushButton::clicked, this, &PdfPreview::ZoomOut);
    connect(fit_width, &QPushButton::clicked, this, &PdfPreview::FitWidth);
    connect(reset, &QPushButton::clicked, this, &PdfPreview::ResetZoom);
    connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { EnsureVisiblePages(); });

    rerender_timer_ = new QTimer(this);
    rerender_timer_->setSingleShot(true);
    rerender_timer_->setInterval(kRerenderDelayMs);
    connect(rerender_timer_, &QTimer::timeout, this, [this]() { RenderAtCurrentZoom(); });

    ShowMessage(QStringLiteral("No preview yet — press Build."));
}

// ---------------------------------------------------------------- 文档

void PdfPreview::SetDocument(const QString& pdf_path) {
    pdf_path_ = pdf_path;
    ClearPages();
    if (pdf_path.isEmpty()) {
        ShowMessage(QStringLiteral("No preview yet — press Build."));
        return;
    }
    // 首页同时给出页面几何尺寸。
    const QPixmap probe = RenderPageToPixmap(pdf_path_, 1, kBaselineDpi, &page_width_pt_, &page_height_pt_);
    if (probe.isNull()) {
        ShowMessage(QStringLiteral("PDF ready but page rendering unavailable.\n") + pdf_path);
        return;
    }
    page_count_ = QueryPageCount(pdf_path_);
    if (page_count_ <= 0)
        page_count_ = 1;

    for (int i = 0; i < page_count_; ++i) {
        PageSlot slot;
        slot.label = MakePageLabel(i);
        column_layout_->addWidget(slot.label, 0, Qt::AlignHCenter);
        pages_.push_back(std::move(slot));
    }
    if (!pages_.empty()) {
        pages_.front().pixmap = probe;
        pages_.front().pixmap_dpi = kBaselineDpi;
    }
    fit_width_ = true;
    FitWidth();
}

void PdfPreview::ClearPages() {
    for (PageSlot& slot : pages_) {
        if (slot.label) {
            column_layout_->removeWidget(slot.label);
            // 必须先隐藏再延迟删除，否则旧页面会继续绘制在新页面之上。
            slot.label->hide();
            slot.label->deleteLater();
        }
    }
    pages_.clear();
    page_count_ = 0;
    page_indicator_->clear();
    // 占位提示不能残留在取代它的页面之上。
    if (message_label_) {
        column_layout_->removeWidget(message_label_);
        message_label_->hide();
    }
}

void PdfPreview::ShowMessage(const QString& text) {
    ClearPages();
    if (message_label_ == nullptr) {
        message_label_ = new QLabel(column_);
        message_label_->setAlignment(Qt::AlignCenter);
        message_label_->setWordWrap(true);
        message_label_->setStyleSheet(
            QString("background: transparent; color: %1; padding: 24px;").arg(theme::kSecondaryText));
    }
    message_label_->setText(text);
    message_label_->setFixedWidth(std::max(200, scroll_->viewport()->width()));
    column_layout_->addWidget(message_label_, 0, Qt::AlignHCenter);
    UpdateColumnSize();
}

void PdfPreview::Clear() {
    pdf_path_.clear();
    ClearPages();
    ShowMessage(QStringLiteral("No preview yet — press Build."));
}

void PdfPreview::SetMessage(const QString& text) {
    ShowMessage(text);
}

QLabel* PdfPreview::MakePageLabel(int index) {
    auto* label = new QLabel(column_);
    label->setObjectName("pdfPageSheet");
    label->setAlignment(Qt::AlignCenter);
    // 白色纸张配细边框，让两页清晰可辨。
    label->setStyleSheet(QString("background: #FFFFFF; color: %1;"
                                 " border: 1px solid %2;")
                             .arg(theme::kDisabledText, theme::kDivider));
    label->setText(QStringLiteral("Page %1").arg(index + 1));
    const QSize size = PageSizeAtZoom();
    label->setFixedSize(size);
    return label;
}

QSize PdfPreview::PageSizeAtZoom() const {
    const int width =
        std::max(1, static_cast<int>(std::lround(page_width_pt_ / kPointsPerInch * kBaselineDpi * zoom_)));
    const int height =
        std::max(1, static_cast<int>(std::lround(page_height_pt_ / kPointsPerInch * kBaselineDpi * zoom_)));
    return QSize(width, height);
}

// ------------------------------------------------------------------ 渲染

int PdfPreview::RenderDpiFor(double zoom) const {
    const int dpi = static_cast<int>(std::lround(kBaselineDpi * zoom * qApp->devicePixelRatio()));
    return std::clamp(dpi, kMinRenderDpi, kMaxRenderDpi);
}

void PdfPreview::RenderPage(int index, int dpi) {
    if (index < 0 || index >= static_cast<int>(pages_.size()))
        return;
    PageSlot& slot = pages_[index];
    const QPixmap fresh = RenderPageToPixmap(pdf_path_, index + 1, dpi, nullptr, nullptr);
    if (fresh.isNull())
        return;
    slot.pixmap = fresh;
    slot.pixmap_dpi = dpi;
    if (slot.label) {
        slot.label->setText(QString());
        slot.label->setPixmap(fresh.scaled(PageSizeAtZoom(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void PdfPreview::ReleaseFarPages(int first, int last) {
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        if (i >= first && i <= last)
            continue;
        PageSlot& slot = pages_[i];
        if (slot.pixmap.isNull())
            continue;
        slot.pixmap = QPixmap();
        slot.pixmap_dpi = 0;
        if (slot.label) {
            slot.label->setPixmap(QPixmap());
            slot.label->setText(QStringLiteral("Page %1").arg(i + 1));
        }
    }
}

// 渲染视口附近的页并释放远处页面的光栅，使长篇论文的内存占用有上界。
void PdfPreview::EnsureVisiblePages() {
    if (pages_.empty())
        return;
    const int page_height = PageSizeAtZoom().height();
    const int stride = page_height + kPageGap;
    const int top = scroll_->verticalScrollBar()->value();
    const int viewport = std::max(1, scroll_->viewport()->height());
    const int first = std::clamp(top / std::max(1, stride), 0, page_count_ - 1);
    const int last = std::clamp((top + viewport) / std::max(1, stride), 0, page_count_ - 1);

    const int dpi = RenderDpiFor(zoom_);
    for (int i = first; i <= last; ++i) {
        PageSlot& slot = pages_[i];
        if (slot.pixmap.isNull() || slot.pixmap_dpi != dpi) {
            RenderPage(i, dpi);
        }
    }
    ReleaseFarPages(first - kKeepPagesAround, last + kKeepPagesAround);
    page_indicator_->setText(QStringLiteral("p. %1 / %2").arg(first + 1).arg(page_count_));
}

void PdfPreview::RenderAtCurrentZoom() {
    if (pdf_path_.isEmpty() || pages_.empty())
        return;
    const int dpi = RenderDpiFor(zoom_);
    // 先丢弃分辨率不符的光栅，再由窗口逻辑重建实际可见的页面。
    for (PageSlot& slot : pages_) {
        if (!slot.pixmap.isNull() && slot.pixmap_dpi != dpi) {
            slot.pixmap = QPixmap();
            slot.pixmap_dpi = 0;
        }
    }
    EnsureVisiblePages();
}

void PdfPreview::ScheduleRerender() {
    rerender_timer_->start();
}

// 可滚动列依据已知的页面几何尺寸确定大小。
// 此处不能用 QWidget::adjustSize()：它会在激活布局前读取布局缓存的
// sizeHint，而该缓存在页面刚被（重新）创建后已经过期，会把列高度压为 0。
void PdfPreview::UpdateColumnSize() {
    const int viewport_width = std::max(1, scroll_->viewport()->width());
    column_layout_->activate();
    if (pages_.empty()) {
        const int height = message_label_ && message_label_->isVisible() ? message_label_->sizeHint().height() : 0;
        column_->resize(viewport_width, height);
        return;
    }
    const QSize page = PageSizeAtZoom();
    const int count = static_cast<int>(pages_.size());
    column_->resize(std::max(page.width(), viewport_width), count * page.height() + (count - 1) * kPageGap);
}

// -------------------------------------------------------------------- 缩放

void PdfPreview::ApplyZoom(bool rerender_now) {
    const QSize size = PageSizeAtZoom();
    for (PageSlot& slot : pages_) {
        if (slot.label)
            slot.label->setFixedSize(size);
        if (!slot.pixmap.isNull() && slot.label) {
            // 立即给出略模糊的反馈；随后由防抖重渲染替换为正确分辨率的光栅。
            slot.label->setText(QString());
            slot.label->setPixmap(slot.pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    UpdateColumnSize();
    UpdateZoomLabel();
    emit zoomChanged(zoom_);
    if (rerender_now) {
        RenderAtCurrentZoom();
    } else {
        ScheduleRerender();
    }
    EnsureVisiblePages();
}

void PdfPreview::UpdateZoomLabel() {
    zoom_label_->setText(QString::number(static_cast<int>(std::lround(zoom_ * 100))) + "%");
}

void PdfPreview::SetZoom(double zoom) {
    const double clamped = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::abs(clamped - zoom_) < 0.001)
        return;
    zoom_ = clamped;
    fit_width_ = false;
    ApplyZoom(false);
}

void PdfPreview::ZoomIn() {
    SetZoom(zoom_ * 1.25);
}
void PdfPreview::ZoomOut() {
    SetZoom(zoom_ / 1.25);
}

void PdfPreview::ResetZoom() {
    SetZoom(1.0);
    ApplyZoom(true);
}

namespace {
double FitZoomFor(const QSize& viewport, double page_w_pt, double page_h_pt, bool whole_page) {
    const double base_w = page_w_pt / kPointsPerInch * kBaselineDpi;
    const double base_h = page_h_pt / kPointsPerInch * kBaselineDpi;
    if (base_w <= 0.0 || base_h <= 0.0)
        return 1.0;
    const double by_width = viewport.width() / base_w;
    if (!whole_page)
        return by_width;
    return std::min(by_width, viewport.height() / base_h);
}
} // namespace

void PdfPreview::FitWidth() {
    const double fit = FitZoomFor(scroll_->viewport()->size(), page_width_pt_, page_height_pt_, false);
    zoom_ = std::clamp(fit, kMinZoom, kMaxZoom);
    fit_width_ = true;
    ApplyZoom(true);
}

void PdfPreview::FitPage() {
    const double fit = FitZoomFor(scroll_->viewport()->size(), page_width_pt_, page_height_pt_, true);
    zoom_ = std::clamp(fit, kMinZoom, kMaxZoom);
    fit_width_ = false;
    ApplyZoom(true);
}

void PdfPreview::ScrollTo(double fraction_x, double fraction_y) {
    UpdateColumnSize();
    QScrollBar* horizontal = scroll_->horizontalScrollBar();
    QScrollBar* vertical = scroll_->verticalScrollBar();
    horizontal->setValue(static_cast<int>(std::lround(horizontal->maximum() * std::clamp(fraction_x, 0.0, 1.0))));
    vertical->setValue(static_cast<int>(std::lround(vertical->maximum() * std::clamp(fraction_y, 0.0, 1.0))));
    EnsureVisiblePages();
}

int PdfPreview::visiblePage() const {
    if (pages_.empty())
        return 0;
    const int stride = PageSizeAtZoom().height() + kPageGap;
    const int index = scroll_->verticalScrollBar()->value() / std::max(1, stride);
    return std::clamp(index, 0, page_count_ - 1) + 1;
}

// 缩放时保持光标下的文档位置不动。
void PdfPreview::ZoomAtPoint(double new_zoom, const QPoint& viewport_pos) {
    const double clamped = std::clamp(new_zoom, kMinZoom, kMaxZoom);
    if (std::abs(clamped - zoom_) < 0.001)
        return;

    QScrollBar* horizontal = scroll_->horizontalScrollBar();
    QScrollBar* vertical = scroll_->verticalScrollBar();
    const double old_w = std::max(1, column_->width());
    const double old_h = std::max(1, column_->height());
    const double fx = (horizontal->value() + viewport_pos.x()) / old_w;
    const double fy = (vertical->value() + viewport_pos.y()) / old_h;

    zoom_ = clamped;
    fit_width_ = false;
    ApplyZoom(false);

    const double new_w = std::max(1, column_->width());
    const double new_h = std::max(1, column_->height());
    horizontal->setValue(static_cast<int>(std::lround(fx * new_w - viewport_pos.x())));
    vertical->setValue(static_cast<int>(std::lround(fy * new_h - viewport_pos.y())));
    EnsureVisiblePages();
}

// -------------------------------------------------------------- 交互

bool PdfPreview::eventFilter(QObject* watched, QEvent* event) {
    if (watched != scroll_->viewport()) {
        return QWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const int delta = wheel->angleDelta().y();
        if (delta == 0)
            break;
        // 滚轮用于缩放（按需求）；按住 Shift 则改为水平平移，这是页面
        // 宽于面板时的常用应对方式。
        if (wheel->modifiers() & Qt::ShiftModifier) {
            scroll_->horizontalScrollBar()->setValue(scroll_->horizontalScrollBar()->value() - delta / 2);
            return true;
        }
        const double factor = delta > 0 ? 1.15 : 1.0 / 1.15;
        ZoomAtPoint(zoom_ * factor, wheel->position().toPoint());
        return true;
    }
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton || mouse->button() == Qt::LeftButton) {
            panning_ = true;
            pan_origin_ = mouse->pos();
            scroll_->viewport()->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        if (!panning_)
            break;
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint delta = mouse->pos() - pan_origin_;
        pan_origin_ = mouse->pos();
        scroll_->horizontalScrollBar()->setValue(scroll_->horizontalScrollBar()->value() - delta.x());
        scroll_->verticalScrollBar()->setValue(scroll_->verticalScrollBar()->value() - delta.y());
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (panning_) {
            panning_ = false;
            scroll_->viewport()->unsetCursor();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void PdfPreview::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (message_label_) {
        message_label_->setFixedWidth(std::max(200, scroll_->viewport()->width()));
    }
    UpdateColumnSize();
    // 处于「适应宽度」状态时页面跟随面板，因此调整窗口大小仍能看到整页。
    if (fit_width_ && !pages_.empty()) {
        const double fit = FitZoomFor(scroll_->viewport()->size(), page_width_pt_, page_height_pt_, false);
        const double clamped = std::clamp(fit, kMinZoom, kMaxZoom);
        if (std::abs(clamped - zoom_) > 0.001) {
            zoom_ = clamped;
            ApplyZoom(false);
        }
    }
    EnsureVisiblePages();
}

} // namespace pf::gui
