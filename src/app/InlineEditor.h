#pragma once
// InlineEditor：直接编辑 InlineContent（方案 §4.1）。
//
// 旧的段落行是基于 InlineToPlainText() 的 QPlainTextEdit，它把整个段落强行
// 塞进 "[cite:key]" 字符串编码，并完全抹平了粗体/斜体。InlineEditor 则维护一个
// QTextEdit，其字符格式本身就是标记状态，其中的语义节点（citation、cross
// reference、inline math）作为只读对象内嵌渲染在文本中。提交时生成 document
// 所存储的 InlineContent。
//
// Token 不变量（方案 §5）：
//   * token 内部的标识符不可由用户编辑，
//   * Backspace/Delete 会删除整个 token，
//   * 点击会选中它，
//   * citation/reference token 可以重新挑选，
//   * 提交、undo 与 redo 都保持语义结构不变。
//
// Inline math（math-input 设计 §3）：token 是一个行内预览对象。双击它（或
// 工具栏的 Inline Math 动作）会打开 math 编辑器，其中显示 LaTeX body 与实时
// 预览；document 存储的值始终是裸 body——\(...\) 定界符稍后生成。
//
// Citations 与 cross references（citation 方案 §1）遵循同一架构：每个都是真正
// 的行内对象（object replacement character + CitationObjectRenderer），显示为
// 一个紧凑的 pill——citation 显示 "[1]"、"[1, 3]"、"[?]"，reference 显示目标
// 的 label。pill 只是 document 所存储 key 集合的可视投影；编号来自窗口注入的
// CitationNumberResolver，绝不来自用户文本。

#include <QTextEdit>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "app/CitationObjectRenderer.h"
#include "document/Document.h"
#include "numbering/CitationNumberResolver.h"

class QAction;
class QMimeData;
class QTextCursor;

namespace pf::gui {

class InlineMathObjectRenderer;
class CitationObjectRenderer;
class InlineEditor : public QTextEdit {
    Q_OBJECT

public:
    explicit InlineEditor(QWidget* parent = nullptr);
    ~InlineEditor() override;

    // 务必在 SetContent 之前建立该行的字体环境（UI 方案 §6）：inline math 对象
    // 在插入时测量 document()->defaultFont()，因此加载内容时正文字体与行高必须
    // 已经就位，否则 pill 与 math 会沿用旧的度量。
    void SetBodyTypography(const QFont& font, int line_height_percent);

    // 从 document 加载。绝不把该行标记为 dirty。
    void SetContent(const InlineContent& content);
    // 本次编辑后 document 应当存储的内容。
    InlineContent Content() const;

    // 外围 UI 在 tooltip/outline 中显示的纯文本。
    QString PlainText() const;

    bool IsDirty() const { return dirty_; }
    bool IsMathEditorOpen() const { return math_editor_open_; }
    void MarkClean() { dirty_ = false; }
    // 加载但不触碰 dirty 标志（供程序化 restyle 使用）。
    void SetContentClean(const InlineContent& content);

    // P0-07：应用异步渲染完成的公式。由 math render 服务在 GUI 线程调用。通过
    // payload 中的 formula id 定位对象；当对象已不存在（被删除、被 undo、被
    // 重新加载）时，丢弃该回复。
    void ApplyMathRender(const QString& formula_id, const QString& latex,
                         const QImage& image, int width, int height,
                         int baseline, qreal device_pixel_ratio,
                         int render_font_px);

    // 让控件重新适配其内容；宽度变化后调用。
    void ResizeToContent();
    // 使用一个尚未应用到控件上的宽度重新适配。
    void ResizeToWidth(int width);

    enum class TokenKind : int { Citation = 1, CrossReference = 2, Math = 3 };

    // 供 citation / cross-reference 选择器使用的引用项。
    // label、detail、payload = citation key 或 node id。
    struct ReferenceItem {
        QString label;
        QString detail;
        QString payload;
    };
    void SetReferenceItems(std::vector<ReferenceItem> items);

    // pill 的 citation 顺序编号（citation 方案 §3）。以 shared_ptr 持有，使每一
    // 行都能基于同一份 document 级映射解析显示编号；重新加载该行会重绘 pill。
    void SetCitationNumbers(std::shared_ptr<const CitationNumberResolver> numbers);
    // cross-reference pill 的 node id -> 显示 label 映射（"@" 列表）。
    void SetCrossReferenceLabels(std::map<QString, QString> labels);

    // 格式化当前选区（若选区已折叠，则切换输入时的开/关状态）——即 [B] / [I]
    // 工具栏以及 Ctrl+B / Ctrl+I 入口。
    void ToggleBold();
    void ToggleItalic();
    bool IsBoldActive() const;
    bool IsItalicActive() const;

    // 在光标处插入语义行内对象。每个都构建真正的可渲染对象（citation 方案
    // §2）：不存在共享的 "InsertToken(kind, payload, label)" 伪接口。
    void InsertCitationObject(const QStringList& keys);
    void InsertCrossReferenceObject(const QString& target_node);
    // 从 LaTeX body 插入一个 inline math 对象（不含定界符）。
    void InsertInlineMath(const QString& latex);
    // 工具栏入口：向用户索取 math body，然后插入。
    void BeginInlineMath();
    // 为 `position` 处的对象打开 math 编辑器，并在用户确认后替换它。
    void EditMathAt(int position);

    // 某个选择器（citation / reference 弹窗）即将获取焦点并向该行插入对象。
    // 在它打开期间，focusOut 不得被误判为「用户已结束编辑 body」（citation
    // 方案 §4）：弹窗的焦点往返不得提交编辑到一半的状态。
    void BeginProtectedInsert() { ++protected_inserts_; }
    void EndProtectedInsert() { protected_inserts_ = qMax(0, protected_inserts_ - 1); }
    bool IsProtectedInsertOpen() const { return protected_inserts_ > 0; }

    // 测试接缝，用于暴露私有剪贴板类型：它在 copy/paste 中携带标记、token 与
    // math 对象。
    QMimeData* MimeDataForSelection() const {
        return createMimeDataFromSelection();
    }
    void InsertMimeDataForTest(const QMimeData* data) {
        insertFromMimeData(data);
    }

    signals:
        // 用户编辑了内容（在 focus-out / Ctrl+Enter 时提交）。
        void Committed();
    // 用户要求在该行之后插入一个 block（Ctrl+Enter）。
    void NewBlockAfter();
    // 某个 citation token 被激活以重新挑选（payload = keys）。
    void CitationTokenActivated(const QString& keys);
    // 某个 cross-reference token 被激活以重新挑选。
    void CrossReferenceTokenActivated(const QString& target);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    // 在宽度变化以及该行首次可见时重新适配。否则固定高度会保留布局赋予控件真实
    // 宽度之前算出的值，表现为一大片空白区域；此后输入（或 Delete）会重新测量并
    // 「恢复」它。
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    QMimeData* createMimeDataFromSelection() const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    // ---- 语义行内对象 ----
    // 在 QTextDocument 中，每个行内对象恰好占一个字符——object replacement
    // character——并通过专用 renderer 绘制（math：InlineMathObjectRenderer；
    // citation / cross reference：CitationObjectRenderer）。其身份（kind +
    // payload）保存在字符格式中，因此 Backspace/Delete 会删除整个对象，文本编辑
    // 永远不会触及它的内部。kTokenChar 是旧 pill 编码使用的私用区字符；它仍可能
    // 通过过期的粘贴进入，会被剥离。
    static constexpr QChar kTokenChar{0xE000};
    static constexpr QChar kObjectChar{0xFFFC};
    static constexpr int kTokenKindProperty =
        inline_object_format::kKindProperty;
    static constexpr int kTokenPayloadProperty =
        inline_object_format::kPayloadProperty;
    // P0-07：标识该行内某一个已渲染的公式对象，使异步回复能够找到（或安全地
    // 找不到）其目标。
    static constexpr int kMathFormulaIdProperty = QTextFormat::UserProperty + 20;
    // 保留 math 对象、标记与语义 token 的剪贴板类型。
    static const char* InlineMimeType();

    // 插入一个为 `payload` 渲染 `display` 的 citation/reference pill。
    void InsertPillObject(QTextCursor& cursor, TokenKind kind,
                          const QString& payload, const QString& display);
    // 插入一个已渲染的 math 对象，其 payload 为 LaTeX body。
    void InsertMathObject(QTextCursor& cursor, const QString& latex);
    void RequestMathRender(const QString& formula_id, const QString& latex);
    void UpdateMathGeometry(QTextCharFormat* format, int available_width) const;
    void RefreshMathGeometry(int available_width, bool rerender);
    // 基于当前编号 / label 映射解析出的 pill 文本。
    QString CitationDisplayText(const QStringList& keys) const;
    QString CrossReferenceDisplayText(const QString& target) const;
    // 重绘该行的 pill，但不触碰 dirty 状态或光标（用于编号映射或 label 集合
    // 发生变化时）。
    void RefreshObjectDisplays();
    // 在光标处插入已经结构化的内容。
    void InsertContent(const InlineContent& content);
    // 光标下的 token（如果有；光标必须位于其*内部*）。
    struct TokenHit {
        TokenKind kind;
        QString payload;
        int position;  // token 字符的位置
    };
    std::optional<TokenHit> TokenAt(int position) const;
    // 把存储的比例行高重新应用到每个 block（在 document 重新加载之后、于
    // loading_ 保护下调用）。
    void ApplyLineHeight();
    void OpenMathEditor(const std::optional<TokenHit>& hit);
    // 删除从 `position` 开始的整个 token。
    void RemoveTokenAt(int position);
    // 编辑器文本中不得留有丢失格式的游离 token 字符（例如从外部来源粘贴之后）。
    void SanitizeTokens();
    // 从 document 的 [begin, end) 区间中提取结构化内容。
    InlineContent ContentInRange(int begin, int end) const;
    std::vector<ReferenceItem> reference_items_;
    bool dirty_ = false;
    bool loading_ = false;
    bool refreshing_displays_ = false;
    bool updating_math_geometry_ = false;
    bool math_editor_open_ = false;
    int protected_inserts_ = 0;
    // P0-07：该行渲染过的每个公式都会获得一个稳定 id，而每次渲染请求都会递增该
    // 行的 generation，从而丢弃针对旧公式的迟到回复。editor id 是创建该行时赋予
    // 的 objectName。
    std::uint64_t math_generation_ = 0;
    std::uint64_t next_formula_id_ = 0;
    // 正文排版（UI 方案 §5）：每次重新加载都重新应用比例行高，使 150% 在
    // SetContent/clear 之后依然保留。
    int line_height_percent_ = 0;
    std::unique_ptr<InlineMathObjectRenderer> math_object_renderer_;
    std::unique_ptr<CitationObjectRenderer> citation_object_renderer_;
    std::shared_ptr<const CitationNumberResolver> citation_numbers_;
    std::map<QString, QString> xref_labels_;
};

}  // namespace pf::gui
