#include "app/CitationObjectRenderer.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QTextCharFormat>
#include <QTextDocument>

#include "app/Theme.h"

namespace pf::gui {

namespace {
// pill 的几何参数。水平内边距让方括号不接触圆角；垂直方向的数值以文本
// 基线为参照，使 pill 贴合行高而不会撑大行高。
constexpr qreal kPadX = 5.0;
constexpr qreal kAboveAscent = 1.5;   // 大写字母高度之上的额外空间
constexpr qreal kBelowBaseline = 2.5; // pill 下探的下降量（位于普通行自身
                                      // 的下降区域内）
constexpr qreal kRadius = 4.0;

QFont PillFont(const QTextCharFormat& format, const QTextDocument* document) {
    // 插入点会把格式的字体设为文档字体；当格式在别处构建时，回退到文档
    // 默认字体。
    QFont font =
        format.hasProperty(QTextFormat::FontFamily) ? format.font() : (document ? document->defaultFont() : QFont());
    font.setWeight(QFont::DemiBold);
    return font;
}

QString DisplayOf(const QTextCharFormat& format) {
    return format.property(citation_format::kDisplayTextProperty).toString();
}
} // namespace

QSizeF CitationObjectRenderer::intrinsicSize(QTextDocument* document, int, const QTextFormat& base) {
    const QTextCharFormat format = base.toCharFormat();
    const QFont font = PillFont(format, document);
    const QFontMetricsF metrics(font);
    const qreal text_width = metrics.horizontalAdvance(DisplayOf(format));
    const qreal width = text_width + 2 * kPadX;
    // AlignNormal 使返回的高度成为 Qt 在基线上方保留的 ascent。单行
    // ascent 足以容纳 pill 本体；基线下方的少量下探绘制到该行正常的下降
    // 区域内——这与数学渲染器使用相同的技巧，因此行框永远不会变大。
    return QSizeF(width, metrics.ascent() + kAboveAscent);
}

void CitationObjectRenderer::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* document, int,
                                        const QTextFormat& base) {
    if (!painter)
        return;
    const QTextCharFormat format = base.toCharFormat();
    const QString display = DisplayOf(format);
    if (display.isEmpty())
        return;

    const QFont font = PillFont(format, document);

    // 对于 AlignNormal，保留框从 (baseline - ascent - pad) 延伸到
    // rect.bottom() == baseline。将其向下扩展基线下方的少量下探，得到完整
    // 的 pill。
    const QRectF pill(rect.left(), rect.top(), rect.width(), rect.height() + kBelowBaseline);

    QColor fill(theme::kAccentSoft);
    QColor border(theme::kAccent);
    border.setAlphaF(0.45);
    QColor text_color(theme::kAccent);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(border, 1.0));
    painter->setBrush(fill);
    painter->drawRoundedRect(pill, kRadius, kRadius);

    painter->setFont(font);
    painter->setPen(text_color);
    // 绘制在该行自身的基线上，使 pill 看起来是文本的一部分。
    painter->drawText(QPointF(pill.left() + kPadX, rect.bottom() - 0.5), display);
    painter->restore();
}

} // namespace pf::gui
