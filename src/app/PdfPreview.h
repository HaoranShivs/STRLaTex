#pragma once
// PdfPreview: the build-output preview pane.
//
// Shows every page of the rendered PDF as a continuous column, the way a
// reader expects a paper to scroll: one page after the next with a thin seam
// between them. The design keeps four things in mind:
//   * the point under the cursor stays under the cursor while zooming, so the
//     text you are reading does not run away;
//   * pages are re-rendered at the resolution the current zoom needs instead
//     of only magnifying a bitmap, so zoomed-in text stays crisp;
//   * only the pages near the viewport keep a raster, because a 15 page paper
//     at 200 DPI would otherwise cost hundreds of megabytes;
//   * once zoomed past the viewport, the page can be dragged with the mouse.

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

    // Point the preview at a built PDF (empty clears the pane).
    void SetDocument(const QString& pdf_path);
    void Clear();
    void SetMessage(const QString& text);

    // Zoom in display units: 1.0 shows the page at 100%.
    void SetZoom(double zoom);
    double zoom() const { return zoom_; }
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();
    void FitWidth();
    void FitPage();
    // Scroll to a fraction of the whole document (0..1).
    void ScrollTo(double fraction_x, double fraction_y);

    int pageCount() const { return page_count_; }
    // Page whose top is closest to the top of the viewport (1-based, 0 when
    // there is nothing to show).
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
        QLabel* label = nullptr;  // shows the raster or a placeholder
        QPixmap pixmap;           // cached raster
        int pixmap_dpi = 0;       // resolution the raster was rendered at
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
    double page_width_pt_ = 595.0;  // A4 until the first page is measured
    double page_height_pt_ = 842.0;
    double zoom_ = 1.0;
    bool fit_width_ = true;  // follow the pane width until the user zooms
    bool panning_ = false;
    QPoint pan_origin_;

    QTimer* rerender_timer_ = nullptr;
};

}  // namespace pf::gui
