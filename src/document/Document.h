#pragma once
// Document Core：语义文档模型（架构 3-13）。
// 封闭 schema：FrontMatter / Body(Section->Subsection->Block) / Inline。
// 此处不包含任何 LaTeX 知识（架构规则 52）。

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/StrongId.h"
#include "math/MathExpression.h"

namespace pf {

// ---------------- Inline 模型（架构 7） ----------------

enum class TextMark : std::uint8_t {
    None = 0,
    Strong = 1 << 0,
    Emphasis = 1 << 1,
};

inline bool HasMark(std::uint8_t marks, TextMark mark) {
    return (marks & static_cast<std::uint8_t>(mark)) != 0;
}
inline void SetMark(std::uint8_t& marks, TextMark mark, bool on) {
    if (on) {
        marks |= static_cast<std::uint8_t>(mark);
    } else {
        marks &= ~static_cast<std::uint8_t>(mark);
    }
}
// Mark 组合，使 Strong | Emphasis 的写法与渲染结果一致。
inline constexpr std::uint8_t operator|(TextMark a, TextMark b) noexcept {
    return static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b);
}
inline constexpr bool operator==(std::uint8_t marks, TextMark mark) noexcept {
    return marks == static_cast<std::uint8_t>(mark);
}
inline constexpr bool operator!=(std::uint8_t marks, TextMark mark) noexcept {
    return !(marks == mark);
}

struct TextRun {
    std::string text;
    std::uint8_t marks = 0;
    bool operator==(const TextRun&) const = default;
};

// 行内数学（设计 §3）：TextBlock 内部的语义行内对象，绝不是独立的 block。
// 只存储数学主体；\(...\) 定界符由 MathGenerator 在渲染时添加。
struct InlineMath {
    MathExpression expression;
    bool operator==(const InlineMath&) const = default;
};

enum class CitationMode : std::uint8_t {
    Parenthetical,
    Narrative,
};

struct Citation {
    std::vector<std::string> keys; // 一个或多个引用 key
    CitationMode mode = CitationMode::Parenthetical;
    bool operator==(const Citation&) const = default;
};

struct CrossReference {
    NodeId target; // 仅语义 NodeId；编号由 renderer 负责
    bool operator==(const CrossReference&) const = default;
};

using InlineNode = std::variant<TextRun, InlineMath, Citation, CrossReference>;
using InlineContent = std::vector<InlineNode>;

// ---------------- Block 模型（架构 6） ----------------

struct Paragraph {
    NodeId id;
    InlineContent content;
};

enum class FigureWidth : std::uint8_t {
    Percent25,
    Percent50,
    Percent75,
    Percent100,
};

// 当模板设置为双栏时，figure 占用文本块的比例
// （设计：单栏与双栏 figure）。
//  SingleColumn - 普通的栏内浮动体。
//  DoubleColumn - 横跨两栏的浮动体（LaTeX \begin{figure*}）。
// 该区分对单栏模板没有意义，因此在单栏模板中按 SingleColumn 渲染；
// 但仍然将它存到 figure 上，意味着只需选择一次，且能经受模板切换。
enum class FigureSpan : std::uint8_t {
    SingleColumn,
    DoubleColumn,
};

struct Figure {
    NodeId id;
    AssetId asset_id;
    InlineContent caption;
    std::string alt_text;
    FigureWidth width = FigureWidth::Percent100;
    FigureSpan span = FigureSpan::SingleColumn;
};

enum class ColumnAlignment : std::uint8_t {
    Left,
    Center,
    Right,
};

struct TableColumn {
    ColumnAlignment alignment = ColumnAlignment::Left;
};

struct TableCell {
    InlineContent content;
};

struct Table {
    NodeId id;
    InlineContent caption;
    bool has_header_row = false;
    std::vector<TableColumn> columns;
    // 行优先；不变式：cells.size() == 行数，每行 size == columns.size()
    std::vector<std::vector<TableCell>> cells;

    size_t RowCount() const noexcept {
        return cells.size();
    }
    size_t ColumnCount() const noexcept {
        return columns.size();
    }
    bool IsRectangular() const noexcept {
        for (const auto& row : cells) {
            if (row.size() != columns.size())
                return false;
        }
        return true;
    }
};

// EquationBlock（设计 §4）：独立的行间公式。`label` 是用户可见的 LaTeX
// label（"eq:example"）；label 为空时回退到 node id，使交叉引用仍能解析。
struct EquationBlock {
    NodeId id;
    MathExpression expression;
    bool numbered = true;
    std::string label;
    bool operator==(const EquationBlock&) const = default;
};

using Block = std::variant<Paragraph, Figure, Table, EquationBlock>;

// ---------------- Section 模型（架构 5） ----------------

// 第三级标题。位于 Subsection 内部，拥有自己的 blocks。
struct Subsubsection {
    NodeId id;
    InlineContent title;
    std::vector<Block> blocks;
};

struct Subsection {
    NodeId id;
    InlineContent title;
    std::vector<Block> blocks;
    std::vector<Subsubsection> subsubsections;
};

struct Section {
    NodeId id;
    InlineContent title;
    std::vector<Block> blocks;
    std::vector<Subsection> subsections;
};

struct Body {
    std::vector<Section> sections;
};

// ---------------- FrontMatter（架构 4） ----------------

struct Author {
    std::string name;
    std::optional<std::string> email;
    std::vector<AffiliationId> affiliations; // 按 id 引用 Affiliation
};

struct Affiliation {
    AffiliationId id;
    std::string name;
};

struct FrontMatter {
    InlineContent title; // 可能暂时为空
    std::vector<Author> authors;
    std::vector<Affiliation> affiliations;
    std::optional<InlineContent> abstract_text;
    std::vector<std::string> keywords;
};

// ---------------- BackMatter ----------------

struct BackMatter {
    // References 章节由系统生成——此处没有用户可编辑的内容。
    bool bibliography_enabled = true;
};

// ---------------- Document ----------------

enum class NodeKind : std::uint8_t {
    Section,
    Subsection,
    Subsubsection,
    Paragraph,
    Figure,
    Table,
    Equation,
};

const char* ToString(NodeKind kind);

// 三种 heading 类型返回 true。
inline bool IsHeadingKind(NodeKind kind) noexcept {
    return kind == NodeKind::Section || kind == NodeKind::Subsection || kind == NodeKind::Subsubsection;
}

// Section = 1，Subsection = 2，Subsubsection = 3；非 heading 为 0。
inline int HeadingDepth(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::Section:
        return 1;
    case NodeKind::Subsection:
        return 2;
    case NodeKind::Subsubsection:
        return 3;
    default:
        return 0;
    }
}

// 定义在 DocumentTraversal.cpp。赋予遍历层与 DocumentEditor 相同的可变容器
// 访问权限：它的职责是把节点所在的精确 blocks vector 交给 editor。
class DocumentMutableAccess;

class Document {
  public:
    const FrontMatter& front_matter() const noexcept {
        return front_matter_;
    }
    const Body& body() const noexcept {
        return body_;
    }
    const BackMatter& back_matter() const noexcept {
        return back_matter_;
    }
    DocumentVersion version() const noexcept {
        return version_;
    }

    // 结构查询
    bool ContainsNode(const NodeId& id) const;
    std::optional<NodeKind> GetNodeKind(const NodeId& id) const;
    // 按文档顺序收集所有 node id。
    std::vector<NodeId> CollectNodeIds() const;

  private:
    friend class DocumentEditor;
    friend class ProjectSerializer;
    friend class EditingSystem;
    friend class DocumentMutableAccess;

    FrontMatter& front_matter() noexcept {
        return front_matter_;
    }
    Body& body() noexcept {
        return body_;
    }
    BackMatter& back_matter() noexcept {
        return back_matter_;
    }
    void set_version(DocumentVersion v) noexcept {
        version_ = v;
    }
    void BumpVersion() noexcept {
        version_.value += 1;
    }

    FrontMatter front_matter_;
    Body body_;
    BackMatter back_matter_;
    DocumentVersion version_{0};
};

} // namespace pf
