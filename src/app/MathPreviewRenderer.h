#pragma once
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QString>

namespace pf::gui {

enum class MathRenderBackend {
    // Compile the expression with a real TeX engine. The bounded QPainter
    // renderer is used only when the runtime is unavailable or TeX rejects
    // the source.
    RealTexPreferred,
    // Fast deterministic renderer used by stress tests and as the fallback.
    ApproximateOnly,
};

struct MathRenderStyle {
    int font_px = 18;                     // base font pixel size
    QColor color = QColor(20, 22, 26);    // glyph colour
    qreal device_pixel_ratio = 2.0;       // render at 2x for crispness
    QString font_family;                  // cache/style identity
    QString template_id;                  // cache/template identity
    MathRenderBackend backend = MathRenderBackend::RealTexPreferred;
};

struct MathRenderResult {
    // P0-07: the pixels travel as a QImage so the render can run on a worker
    // thread (QPixmap may only be created on the GUI thread). The GUI-side
    // entry point converts to `pixmap`; worker-side callers use `image`.
    QImage image;
    QPixmap pixmap;     // null only for empty input; dpr set to style.device_pixel_ratio
    int width = 0;      // logical pixels
    int height = 0;     // logical pixels
    int baseline = 0;   // logical pixels from the top of the pixmap to the math baseline
    bool exact = true;  // true for real TeX, or exact ApproximateOnly output
    bool used_tex = false;
    QString note;       // human-readable renderer/fallback state
    // Device pixel ratio the image was rasterised at (worker side: the
    // QPixmap conversion applies it on the GUI thread).
    qreal device_pixel_ratio = 1.0;

    // True when there is something to draw.
    bool HasPixels() const { return !pixmap.isNull() || !image.isNull(); }
};

// Render a LaTeX math body. Always returns a usable result for non-empty input.
// GUI thread only: the result carries a QPixmap.
MathRenderResult RenderMathPreview(const QString& latex, const MathRenderStyle& style);

// P0-07 worker-thread entry point: identical rendering, but the result carries
// a QImage and no QPixmap, so it is safe to call off the GUI thread. The
// caller converts to a QPixmap on the GUI thread when it applies the result.
MathRenderResult RenderMathPreviewImage(const QString& latex,
                                        const MathRenderStyle& style);

}  // namespace pf::gui
