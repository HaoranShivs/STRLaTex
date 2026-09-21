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
    // 对于 AlignNormal 的行内对象，Qt 会把它的高度当作 ascent。renderer 特意
    // 把（已做限定的）下沉部分绘制到 rect.bottom() 之下，即普通文本行的下沉
    // 区域中。
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
