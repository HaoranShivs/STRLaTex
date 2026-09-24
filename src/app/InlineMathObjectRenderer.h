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
inline constexpr int kSourceWidthProperty = QTextFormat::UserProperty + 21;
inline constexpr int kSourceHeightProperty = QTextFormat::UserProperty + 22;
inline constexpr int kRenderFontPxProperty = QTextFormat::UserProperty + 23;
inline constexpr int kImageWidthProperty = QTextFormat::UserProperty + 24;
inline constexpr int kImageHeightProperty = QTextFormat::UserProperty + 25;
inline constexpr int kSourceBaselineProperty = QTextFormat::UserProperty + 26;
}  // namespace inline_math_format

// 完整胶囊尺寸参与 QTextDocument 排版；位图始终绘制在胶囊内部，
// 不越过对象矩形，也不会依赖正文极小的下沉空间。
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
