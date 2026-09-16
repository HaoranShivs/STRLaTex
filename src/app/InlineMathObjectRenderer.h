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

// QTextDocument treats ordinary images as having their baseline at the image
// bottom. This object keeps the TeX baseline explicit and paints the formula's
// descent below the text baseline without increasing the line box.
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
