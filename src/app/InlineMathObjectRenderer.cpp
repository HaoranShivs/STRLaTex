#include "app/InlineMathObjectRenderer.h"

#include <QPainter>
#include <QPixmap>
#include <QTextDocument>

#include "app/Theme.h"

namespace pf::gui {

QSizeF InlineMathObjectRenderer::intrinsicSize(QTextDocument*, int,
                                               const QTextFormat& format) {
    const qreal width =
        qMax<qreal>(1.0, format.property(inline_math_format::kWidthProperty)
                             .toDouble());
    const qreal height =
        qMax<qreal>(1.0, format.property(inline_math_format::kHeightProperty)
                             .toDouble());
    return QSizeF(width, height);
}

void InlineMathObjectRenderer::drawObject(QPainter* painter,
                                          const QRectF& rect, QTextDocument*,
                                          int, const QTextFormat& format) {
    if (!painter || rect.isEmpty()) return;
    const QPixmap pixmap =
        format.property(inline_math_format::kPixmapProperty).value<QPixmap>();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    QColor border(theme::kAccent);
    border.setAlphaF(0.45);
    painter->setPen(QPen(border, 1.0));
    painter->setBrush(QColor(theme::kAccentSoft));
    painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 5.0, 5.0);
    if (pixmap.isNull()) {
        painter->setPen(QColor(theme::kAccent));
        painter->setFont(format.toCharFormat().font());
        painter->drawText(rect, Qt::AlignCenter, QStringLiteral("…"));
    } else {
        const qreal image_width = qMax<qreal>(1.0, format.property(
            inline_math_format::kImageWidthProperty).toDouble());
        const qreal image_height = qMax<qreal>(1.0, format.property(
            inline_math_format::kImageHeightProperty).toDouble());
        const QRectF target(rect.left() + (rect.width() - image_width) / 2.0,
                            rect.top() + (rect.height() - image_height) / 2.0,
                            image_width, image_height);
        painter->drawPixmap(target, pixmap, pixmap.rect());
    }
    painter->restore();
}

}  // namespace pf::gui
