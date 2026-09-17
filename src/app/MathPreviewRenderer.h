#pragma once
#include <QColor>
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
    QPixmap pixmap;     // null only for empty input; dpr set to style.device_pixel_ratio
    int width = 0;      // logical pixels
    int height = 0;     // logical pixels
    int baseline = 0;   // logical pixels from the top of the pixmap to the math baseline
    bool exact = true;  // true for real TeX, or exact ApproximateOnly output
    bool used_tex = false;
    QString note;       // human-readable renderer/fallback state
};

// Render a LaTeX math body. Always returns a usable result for non-empty input.
MathRenderResult RenderMathPreview(const QString& latex, const MathRenderStyle& style);

}  // namespace pf::gui
