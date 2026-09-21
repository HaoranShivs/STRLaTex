#pragma once
// CitationObjectRenderer（引用方案 §1）：把语义化行内对象——引用和
// 交叉引用——绘制为 QTextDocument 中的紧凑 pill。
//
// 设计参照 InlineMathObjectRenderer：文档为每个对象恰好保留一个对象替换
// 字符（U+FFFC）；绘制它所需的一切（kind、payload、显示文本）都保存在
// QTextCharFormat 中。pill 从不参与普通文本编辑，且由于它只是一个字符，
// Backspace/Delete 会删除整个对象。
//
// 显示的 "[1]" / "[1, 3]" / "[?]" 文本只是引用顺序编号策略
// （CitationNumberResolver）的纯投影——文档本身只存储 key，最终编号由
// LaTeX/BibTeX 在 PDF 中生成。

#include <QAbstractTextDocumentLayout>
#include <QObject>
#include <QTextFormat>

namespace pf::gui {

namespace citation_format {
// 向文档布局注册的对象类型。UserObject + 1 属于行内公式；引用和交叉引用
// 共用这一个。
inline constexpr int kObjectType = QTextFormat::UserObject + 2;
// 要绘制的 pill 文本，例如 "[1, 3]"。在插入/加载时解析。
inline constexpr int kDisplayTextProperty = QTextFormat::UserProperty + 7;
}  // namespace citation_format

// 所有语义化行内对象共用的格式属性。InlineEditor 写入它们，
// ContentInRange() 读取它们，pill 渲染器用 kind 决定色调。在此声明以便
// 双方对 id 保持一致。
namespace inline_object_format {
inline constexpr int kKindProperty = QTextFormat::UserProperty + 1;
inline constexpr int kPayloadProperty = QTextFormat::UserProperty + 2;
}  // namespace inline_object_format

class CitationObjectRenderer final : public QObject,
                                     public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit CitationObjectRenderer(QObject* parent = nullptr)
        : QObject(parent) {}

    QSizeF intrinsicSize(QTextDocument* document, int positionInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect,
                    QTextDocument* document, int positionInDocument,
                    const QTextFormat& format) override;
};

}  // namespace pf::gui
