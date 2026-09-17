#include "app/CitationObjectRenderer.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QTextCharFormat>
#include <QTextDocument>

#include "app/Theme.h"

namespace pf::gui {

namespace {
// Pill geometry. The horizontal padding keeps the brackets from touching the
// rounded corners; the vertical numbers are expressed relative to the text
// baseline so the pill hugs the line instead of enlarging it.
constexpr qreal kPadX = 5.0;
constexpr qreal kAboveAscent = 1.5;   // extra space above the cap height
constexpr qreal kBelowBaseline = 2.5; // descent the pill dips to (inside the
                                      // normal line's own descent area)
constexpr qreal kRadius = 4.0;

QFont PillFont(const QTextCharFormat& format, const QTextDocument* document) {
    // The insertion site sets the format's font to the document font; fall
    // back to the document default when a format was built elsewhere.
    QFont font = format.hasProperty(QTextFormat::FontFamily)
                     ? format.font()
                     : (document ? document->defaultFont() : QFont());
    font.setWeight(QFont::DemiBold);
    return font;
}

QString DisplayOf(const QTextCharFormat& format) {
    return format.property(citation_format::kDisplayTextProperty).toString();
}
}  // namespace

QSizeF CitationObjectRenderer::intrinsicSize(QTextDocument* document, int,
                                             const QTextFormat& base) {
    const QTextCharFormat format = base.toCharFormat();
    const QFont font = PillFont(format, document);
    const QFontMetricsF metrics(font);
    const qreal text_width = metrics.horizontalAdvance(DisplayOf(format));
    const qreal width = text_width + 2 * kPadX;
    // AlignNormal makes the returned height the ascent Qt reserves above the
    // baseline. One line ascent is enough for the pill body; the small dip
    // below the baseline is painted into the line's normal descent area - the
    // same trick the math renderer uses, so no line box ever grows.
    return QSizeF(width, metrics.ascent() + kAboveAscent);
}

void CitationObjectRenderer::drawObject(QPainter* painter, const QRectF& rect,
                                        QTextDocument* document, int,
                                        const QTextFormat& base) {
    if (!painter) return;
    const QTextCharFormat format = base.toCharFormat();
    const QString display = DisplayOf(format);
    if (display.isEmpty()) return;

    const QFont font = PillFont(format, document);

    // For AlignNormal the reserved box runs from (baseline - ascent - pad)
    // down to rect.bottom() == baseline. Extend it by the small dip below the
    // baseline to get the full pill.
    const QRectF pill(rect.left(), rect.top(), rect.width(),
                      rect.height() + kBelowBaseline);

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
    // Draw on the line's own baseline so the pill reads as part of the text.
    painter->drawText(QPointF(pill.left() + kPadX, rect.bottom() - 0.5),
                      display);
    painter->restore();
}

}  // namespace pf::gui
