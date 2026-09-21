#pragma once

#include <QObject>
#include <QAbstractTextDocumentLayout>
#include <QTextFormat>

namespace pf::gui {

namespace inline_math_format {
inline constexpr int kObjectType = QTextFormat::UserObject + 1;
inline constexpr int kPixmapProperty = QTextFormat::UserProperty + 3;
inline constexpr int kWidthProperty = QTextFormat::UserProperty + 4;
inline constexpr int kHeightProperty = QTextFormat::UserProperty + 5;
inline constexpr int kBaselineProperty = QTextFormat::UserProperty + 6;
}  // namespace inline_math_format

// QTextDocument 把普通图片的基线视为图片底边。该对象则显式保留 TeX 基线，
// 并在不增大行框的前提下，把公式的下沉部分绘制到文本基线下方。
class InlineMathObjectRenderer final : public QObject,
                                       public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit InlineMathObjectRenderer(QObject* parent = nullptr)
        : QObject(parent) {}

    QSizeF intrinsicSize(QTextDocument* document, int positionInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect,
                    QTextDocument* document, int positionInDocument,
                    const QTextFormat& format) override;
};

}  // namespace pf::gui
