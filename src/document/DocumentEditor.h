#pragma once
// DocumentEditor: low-level document mutation with structural constraint
// enforcement (architecture section "八、DocumentEditor").
// Knows nothing about undo, build, save, PDF, Qt, or tectonic.

#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"
#include "document/Document.h"

namespace pf {

enum class EditError {
    InvalidTarget,        // node / parent / index does not exist
    ConstraintViolation,  // structural rule violated (e.g. bad table shape)
    NotFound,             // referenced node or asset missing
};

const char* ToString(EditError error);

struct InsertBlockSpec {
    NodeId parent_section;    // Section or Subsection id
    std::optional<size_t> index;  // nullopt = append
};

// Position of a section/subsection/subsubsection within the document.
struct SectionPath {
    size_t section_index = 0;
    bool in_subsection = false;
    size_t subsection_index = 0;
    bool in_subsubsection = false;
    size_t subsubsection_index = 0;
};

class DocumentEditor {
public:
    explicit DocumentEditor(Document& document);

    // Direct mutable access for editors that compose several low-level
    // mutations under one revision bump (used by EditingSystem meta payloads).
    Document& doc() { return document_; }

    // FrontMatter mutations
    Result<void, EditError> SetTitle(const InlineContent& title);
    Result<void, EditError> SetAbstract(const std::optional<InlineContent>& abstract_text);
    Result<void, EditError> SetKeywords(const std::vector<std::string>& keywords);

    Result<AffiliationId, EditError> AddAffiliation(const std::string& name,
                                                    AffiliationId id = AffiliationId{});
    Result<void, EditError> RemoveAffiliation(const AffiliationId& id);
    Result<NodeId, EditError> AddAuthor(Author author, NodeId id = NodeId{});
    Result<void, EditError> RemoveAuthor(size_t index);
    Result<void, EditError> UpdateAuthor(size_t index, const Author& author);

    // Body structure mutations
    // Precondition: sections must remain Block* Subsection* ordered? No --
    // V1 allows blocks and subsections interleaved in the vectors; the
    // rendering order is blocks-then-subsections per Section. We keep
    // append-only convenience plus explicit index control.
    Result<NodeId, EditError> InsertSection(size_t index, InlineContent title,
                                            NodeId id = NodeId{});
    Result<void, EditError> DeleteSection(size_t index);
    Result<void, EditError> MoveSection(size_t from, size_t to);

    Result<NodeId, EditError> InsertSubsection(size_t section_index, size_t index,
                                               InlineContent title, NodeId id = NodeId{});
    // Insert a subsection heading directly after `anchor` in reading order.
    //
    // A section stores its own blocks before its subsections, so a heading can
    // only appear at a position the model can express. "Insert here" therefore
    // means: the blocks that follow the anchor become the new subsection's
    // content, and the heading appears exactly where it was asked for. When
    // the anchor is a subsection (or a block inside one) nothing moves.
    Result<NodeId, EditError> InsertSubsectionAfter(const NodeId& anchor,
                                                    InlineContent title,
                                                    NodeId id = NodeId{});
    Result<void, EditError> DeleteSubsection(size_t section_index, size_t subsection_index);
    Result<void, EditError> MoveSubsection(size_t section_index, size_t from, size_t to);

    // Third heading level. A subsubsection lives inside a subsection.
    Result<NodeId, EditError> InsertSubsubsection(size_t section_index,
                                                  size_t subsection_index, size_t index,
                                                  InlineContent title,
                                                  NodeId id = NodeId{});
    // Insert after `anchor` in reading order; the blocks below the anchor move
    // into the new subsubsection, mirroring InsertSubsectionAfter.
    Result<NodeId, EditError> InsertSubsubsectionAfter(const NodeId& anchor,
                                                       InlineContent title,
                                                       NodeId id = NodeId{});
    Result<void, EditError> DeleteSubsubsection(size_t section_index,
                                                size_t subsection_index,
                                                size_t subsubsection_index);
    Result<void, EditError> MoveSubsubsection(size_t section_index, size_t subsection_index,
                                              size_t from, size_t to);

    Result<void, EditError> RenameSection(const NodeId& id, const InlineContent& title);
    Result<void, EditError> RenameSubsection(const NodeId& id, const InlineContent& title);
    Result<void, EditError> RenameSubsubsection(const NodeId& id, const InlineContent& title);

    // Block mutations
    Result<NodeId, EditError> InsertBlock(const NodeId& parent, std::optional<size_t> index,
                                          Block block, NodeId id = NodeId{});
    Result<void, EditError> DeleteBlock(const NodeId& id);
    Result<void, EditError> MoveBlock(const NodeId& id, const NodeId& new_parent,
                                      std::optional<size_t> new_index);

    Result<void, EditError> SetParagraphContent(const NodeId& id, const InlineContent& content);
    Result<void, EditError> SetCaption(const NodeId& id, const InlineContent& caption);
    Result<void, EditError> SetEquationSource(const NodeId& id, const std::string& source,
                                              std::optional<bool> numbered = std::nullopt);
    Result<void, EditError> SetFigureAsset(const NodeId& id, const AssetId& asset_id,
                                           std::optional<FigureWidth> width = std::nullopt);
    Result<void, EditError> SetFigureAltText(const NodeId& id, const std::string& alt);

    // Table mutations - enforce rectangularity invariant.
    static Result<Table, EditError> MakeTable(std::vector<TableColumn> columns,
                                              size_t rows, bool has_header_row,
                                              NodeId id = NodeId{});
    Result<void, EditError> InsertTableRow(const NodeId& id, size_t index);
    Result<void, EditError> DeleteTableRow(const NodeId& id, size_t index);
    Result<void, EditError> InsertTableColumn(const NodeId& id, size_t index,
                                              ColumnAlignment alignment);
    Result<void, EditError> DeleteTableColumn(const NodeId& id, size_t index);
    Result<void, EditError> SetTableCell(const NodeId& id, size_t row, size_t column,
                                         const InlineContent& content);

    // Lookup helpers (mutable access for editor internals only)
    Section* FindSection(const NodeId& id);
    Subsection* FindSubsection(const NodeId& id);
    Subsubsection* FindSubsubsection(const NodeId& id);
    // The subsection that owns subsubsection `id`, or nullptr.
    Subsection* FindParentSubsubsection(const NodeId& id);
    Block* FindBlock(const NodeId& id);
    // (section_index, subsection_index or npos, subsubsection_index or npos,
    //  block index or npos)
    struct BlockPosition {
        size_t section_index;
        bool in_subsection = false;
        size_t subsection_index = 0;
        bool in_subsubsection = false;
        size_t subsubsection_index = 0;
        size_t block_index;
    };
    std::optional<BlockPosition> FindBlockPosition(const NodeId& id);
    // Container ids for a block, in insertion order (section, subsection,
    // subsubsection). Used by move logic to reject cross-container moves.
    std::vector<NodeId> BlockParents(const NodeId& id);

private:
    [[noreturn]] void Throw(EditError e) const;
    static std::vector<std::vector<TableCell>> MakeCells(size_t rows, size_t cols);
    // The blocks vector of a section/subsection/subsubsection id.
    std::vector<Block>* FindBlockListForParent(const NodeId& parent);
    // The blocks vector that holds node `id`, plus its address.
    std::vector<Block>* FindBlockListForNode(const NodeId& id);

    Document& document_;
};

}  // namespace pf

namespace pf {
template <>
EditError pf::ToStringError<EditError>(const std::string& value);
}  // namespace pf
