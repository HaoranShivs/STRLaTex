#pragma once
// Document Core: semantic document model (architecture sections 3-13).
// Closed schema: FrontMatter / Body(Section->Subsection->Block) / Inline.
// No LaTeX knowledge lives here (architecture rule 52).

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/StrongId.h"
#include "math/MathExpression.h"

namespace pf {

// ---------------- Inline model (section 7) ----------------

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
// Mark composition, so Strong | Emphasis reads the way it is rendered.
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

// Inline math (design §3): a semantic inline object inside a TextBlock, never
// a standalone block. Only the math body is stored; the \(...\) delimiters are
// added by MathGenerator at render time.
struct InlineMath {
    MathExpression expression;
    bool operator==(const InlineMath&) const = default;
};

enum class CitationMode : std::uint8_t {
    Parenthetical,
    Narrative,
};

struct Citation {
    std::vector<std::string> keys;  // one or more citation keys
    CitationMode mode = CitationMode::Parenthetical;
    bool operator==(const Citation&) const = default;
};

struct CrossReference {
    NodeId target;  // semantic NodeId only; numbering is the renderer's job
    bool operator==(const CrossReference&) const = default;
};

using InlineNode = std::variant<TextRun, InlineMath, Citation, CrossReference>;
using InlineContent = std::vector<InlineNode>;

// ---------------- Block model (section 6) ----------------

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

struct Figure {
    NodeId id;
    AssetId asset_id;
    InlineContent caption;
    std::string alt_text;
    FigureWidth width = FigureWidth::Percent100;
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
    // row-major; invariant: cells.size() == rows, each row size == columns.size()
    std::vector<std::vector<TableCell>> cells;

    size_t RowCount() const noexcept { return cells.size(); }
    size_t ColumnCount() const noexcept {
        return columns.size();
    }
    bool IsRectangular() const noexcept {
        for (const auto& row : cells) {
            if (row.size() != columns.size()) return false;
        }
        return true;
    }
};

// EquationBlock (design §4): a standalone display formula. `label` is the
// user-visible LaTeX label ("eq:example"); an empty label falls back to the
// node id so cross references keep resolving.
struct EquationBlock {
    NodeId id;
    MathExpression expression;
    bool numbered = true;
    std::string label;
    bool operator==(const EquationBlock&) const = default;
};

using Block = std::variant<Paragraph, Figure, Table, EquationBlock>;

// ---------------- Section model (section 5) ----------------

// Third heading level. Lives inside a Subsection, owns its own blocks.
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

// ---------------- FrontMatter (section 4) ----------------

struct Author {
    std::string name;
    std::optional<std::string> email;
    std::vector<AffiliationId> affiliations;  // references Affiliation by id
};

struct Affiliation {
    AffiliationId id;
    std::string name;
};

struct FrontMatter {
    InlineContent title;  // may be empty temporarily
    std::vector<Author> authors;
    std::vector<Affiliation> affiliations;
    std::optional<InlineContent> abstract_text;
    std::vector<std::string> keywords;
};

// ---------------- BackMatter ----------------

struct BackMatter {
    // References section is system generated - no user-editable content here.
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

// True for the three heading kinds.
inline bool IsHeadingKind(NodeKind kind) noexcept {
    return kind == NodeKind::Section || kind == NodeKind::Subsection ||
           kind == NodeKind::Subsubsection;
}

// Section = 1, Subsection = 2, Subsubsection = 3; 0 for non-headings.
inline int HeadingDepth(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Section: return 1;
        case NodeKind::Subsection: return 2;
        case NodeKind::Subsubsection: return 3;
        default: return 0;
    }
}

// Defined in DocumentTraversal.cpp. Grants the traversal layer the same
// mutable container access DocumentEditor has: its job is to hand the editor
// the exact blocks vector a node lives in.
class DocumentMutableAccess;

class Document {
public:
    const FrontMatter& front_matter() const noexcept { return front_matter_; }
    const Body& body() const noexcept { return body_; }
    const BackMatter& back_matter() const noexcept { return back_matter_; }
    DocumentVersion version() const noexcept { return version_; }

    // Structural queries
    bool ContainsNode(const NodeId& id) const;
    std::optional<NodeKind> GetNodeKind(const NodeId& id) const;
    // Collect all node ids in document order.
    std::vector<NodeId> CollectNodeIds() const;

private:
    friend class DocumentEditor;
    friend class ProjectSerializer;
    friend class EditingSystem;
    friend class DocumentMutableAccess;

    FrontMatter& front_matter() noexcept { return front_matter_; }
    Body& body() noexcept { return body_; }
    BackMatter& back_matter() noexcept { return back_matter_; }
    void set_version(DocumentVersion v) noexcept { version_ = v; }
    void BumpVersion() noexcept { version_.value += 1; }

    FrontMatter front_matter_;
    Body body_;
    BackMatter back_matter_;
    DocumentVersion version_{0};
};

}  // namespace pf
