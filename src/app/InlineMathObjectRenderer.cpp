#include "app/InlineMathObjectRenderer.h"

#include <QPainter>
#include <QPixmap>
#include <QTextDocument>

namespace pf::gui {

QSizeF InlineMathObjectRenderer::intrinsicSize(QTextDocument*, int,
                                               const QTextFormat& format) {
    const qreal width =
        qMax<qreal>(1.0, format.property(inline_math_format::kWidthProperty)
                             .toDouble());
    const qreal baseline =
        qMax<qreal>(1.0, format.property(inline_math_format::kBaselineProperty)
                             .toDouble());
    // For an AlignNormal inline object Qt treats its height as ascent.
    // The renderer deliberately paints the (already bounded) descent below
    // rect.bottom(), into the normal text line's descent area.
    return QSizeF(width, baseline);
}

void InlineMathObjectRenderer::drawObject(QPainter* painter,
                                          const QRectF& rect, QTextDocument*,
                                          int, const QTextFormat& format) {
    if (!painter) return;
    const QPixmap pixmap =
        format.property(inline_math_format::kPixmapProperty).value<QPixmap>();
    if (pixmap.isNull()) return;

    const qreal width =
        format.property(inline_math_format::kWidthProperty).toDouble();
    const qreal height =
        format.property(inline_math_format::kHeightProperty).toDouble();
    const qreal baseline =
        format.property(inline_math_format::kBaselineProperty).toDouble();
    if (width <= 0 || height <= 0 || baseline <= 0) return;

    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF target(rect.left(), rect.bottom() - baseline, width, height);
    painter->drawPixmap(target, pixmap, pixmap.rect());
    painter->restore();
}

}  // namespace pf::gui
