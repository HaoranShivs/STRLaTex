#pragma once
// PdfPreview：build 输出的预览窗格。
//
// 把渲染后 PDF 的每一页排成连续的一列，符合读者对纸张滚动的预期：一页接着
// 一页，页与页之间只有一道细缝。设计时考虑了四点：
//   * 缩放时光标下方的位置保持不动，正在阅读的文字不会跑偏；
//   * 页面按当前缩放所需的清晰度重新渲染，而不是只放大位图，
//     因此放大后的文字依然锐利；
//   * 只有视口附近的页面保留栅格位图，否则一份 200 DPI 的 15 页文档
//     会占用数百 MB 内存；
//   * 一旦缩放超出视口，就可以用鼠标拖拽页面。

#include <QString>
#include <QWidget>
#include <QPixmap>

#include <vector>

class QLabel;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class QWheelEvent;
class QMouseEvent;
class QResizeEvent;

namespace pf::gui {

class PdfPreview : public QWidget {
    Q_OBJECT

public:
    explicit PdfPreview(QWidget* parent = nullptr);

    // 让预览指向已构建的 PDF（传空则清空窗格）。
    void SetDocument(const QString& pdf_path);
    void Clear();
    void SetMessage(const QString& text);

    // 以显示单位表示的缩放：1.0 表示页面按 100% 显示。
    void SetZoom(double zoom);
    double zoom() const { return zoom_; }
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();
    void FitWidth();
    void FitPage();
    // 滚动到整个文档的指定比例处（0..1）。
    void ScrollTo(double fraction_x, double fraction_y);

    int pageCount() const { return page_count_; }
    // 页顶最接近视口顶部的页面（从 1 开始计数，没有内容可显示时为 0）。
    int visiblePage() const;

    static constexpr double kMinZoom = 0.25;
    static constexpr double kMaxZoom = 8.0;

signals:
    void zoomChanged(double zoom);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct PageSlot {
        QLabel* label = nullptr;  // 显示栅格位图或占位符
        QPixmap pixmap;           // 缓存的栅格位图
        int pixmap_dpi = 0;       // 栅格位图渲染时所用的分辨率
    };

    QLabel* MakePageLabel(int index);
    void ClearPages();
    void ShowMessage(const QString& text);
    void ApplyZoom(bool rerender_now);
    void ScheduleRerender();
    void RenderAtCurrentZoom();
    void RenderPage(int index, int dpi);
    void ReleaseFarPages(int first, int last);
    int RenderDpiFor(double zoom) const;
    void ZoomAtPoint(double new_zoom, const QPoint& viewport_pos);
    void UpdateZoomLabel();
    void RebuildPageWidgets();
    void UpdatePageSizes();
    void UpdateViewportExtent();
    void EnsureVisiblePages();
    void UpdateColumnSize();
    QSize PageSizeAtZoom() const;

    QLabel* zoom_label_ = nullptr;
    QLabel* page_indicator_ = nullptr;
    QLabel* message_label_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* column_ = nullptr;
    QVBoxLayout* column_layout_ = nullptr;

    QString pdf_path_;
    std::vector<PageSlot> pages_;
    int page_count_ = 0;
    double page_width_pt_ = 595.0;  // 测量首页之前暂按 A4 处理
    double page_height_pt_ = 842.0;
    double zoom_ = 1.0;
    bool fit_width_ = true;  // 在用户缩放之前跟随窗格宽度
    bool panning_ = false;
    QPoint pan_origin_;

    QTimer* rerender_timer_ = nullptr;
};

}  // namespace pf::gui
