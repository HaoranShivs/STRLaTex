#pragma once
#include <QColor>
#include <QPixmap>
#include <QString>

namespace pf::gui {

struct MathRenderStyle {
    int font_px = 18;                     // base font pixel size
    QColor color = QColor(20, 22, 26);    // glyph colour
    qreal device_pixel_ratio = 2.0;       // render at 2x for crispness
};

struct MathRenderResult {
    QPixmap pixmap;     // null only for empty input; dpr set to style.device_pixel_ratio
    int width = 0;      // logical pixels
    int height = 0;     // logical pixels
    int baseline = 0;   // logical pixels from the top of the pixmap to the math baseline
    bool exact = true;  // false when unsupported syntax fell back to literal source text
    QString note;       // human-readable reason when exact == false
};

// Render a LaTeX math body. Always returns a usable result for non-empty input.
MathRenderResult RenderMathPreview(const QString& latex, const MathRenderStyle& style);

}  // namespace pf::gui
