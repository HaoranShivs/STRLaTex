#pragma once
// DocumentEditor：低层文档变更，负责强制执行结构约束
// （架构 八、DocumentEditor）。
// 不感知 undo、build、save、PDF、Qt 或 tectonic。

#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"
#include "document/Document.h"

namespace pf {

enum class EditError {
  InvalidTarget,       // node / parent / index 不存在
  ConstraintViolation, // 违反结构规则（例如表格形状不合法）
  NotFound,            // 被引用的 node 或 asset 缺失
};

const char *ToString(EditError error);

struct InsertBlockSpec {
  NodeId parent_section;       // Section 或 Subsection 的 id
  std::optional<size_t> index; // nullopt 表示追加
};

// section/subsection/subsubsection 在文档中的位置。
struct SectionPath {
  size_t section_index = 0;
  bool in_subsection = false;
  size_t subsection_index = 0;
  bool in_subsubsection = false;
  size_t subsubsection_index = 0;
};

class DocumentEditor {
public:
  explicit DocumentEditor(Document &document);

  // 供那些在一次 revision 提升内组合多次低层变更的编辑操作直接进行可变访问
  // （由 EditingSystem 的 meta payload 使用）。
  Document &doc() { return document_; }

  // FrontMatter 变更
  Result<void, EditError> SetTitle(const InlineContent &title);
  Result<void, EditError>
  SetAbstract(const std::optional<InlineContent> &abstract_text);
  Result<void, EditError> SetKeywords(const std::vector<std::string> &keywords);

  Result<AffiliationId, EditError>
  AddAffiliation(const std::string &name, AffiliationId id = AffiliationId{});
  Result<void, EditError> RemoveAffiliation(const AffiliationId &id);
  Result<NodeId, EditError> AddAuthor(Author author, NodeId id = NodeId{});
  Result<void, EditError> RemoveAuthor(size_t index);
  Result<void, EditError> UpdateAuthor(size_t index, const Author &author);

  // 正文结构变更
  // 前提条件：section 必须保持 Block* Subsection* 的顺序吗？不——
  // V1 允许 blocks 与 subsections 在 vector 中交错存放；渲染顺序为
  // 每个 Section 内先 blocks 后 subsections。我们同时保留仅追加的便捷
  // 方式与显式 index 控制。
  Result<NodeId, EditError> InsertSection(size_t index, InlineContent title,
                                          NodeId id = NodeId{});
  Result<void, EditError> DeleteSection(size_t index);
  Result<void, EditError> MoveSection(size_t from, size_t to);

  Result<NodeId, EditError> InsertSubsection(size_t section_index, size_t index,
                                             InlineContent title,
                                             NodeId id = NodeId{});
  // 按阅读顺序在 `anchor` 之后直接插入一个 subsection 标题。
  //
  // 一个 section 会把自己的 blocks 存放在其 subsections 之前，因此标题只能
  // 出现在模型能够表达的位置上。所以「在此处插入」的含义是：anchor 之后的
  // blocks 成为新 subsection 的内容，而标题出现在被要求的确切位置。当
  // anchor 是 subsection（或其中的某个 block）时，不做任何移动。
  Result<NodeId, EditError> InsertSubsectionAfter(const NodeId &anchor,
                                                  InlineContent title,
                                                  NodeId id = NodeId{});
  Result<void, EditError> DeleteSubsection(size_t section_index,
                                           size_t subsection_index);
  Result<void, EditError> MoveSubsection(size_t section_index, size_t from,
                                         size_t to);

  // 第三级标题。subsubsection 位于 subsection 内部。
  Result<NodeId, EditError>
  InsertSubsubsection(size_t section_index, size_t subsection_index,
                      size_t index, InlineContent title, NodeId id = NodeId{});
  // 按阅读顺序在 `anchor` 之后插入；anchor 之后的 blocks 移入新建的
  // subsubsection，与 InsertSubsectionAfter 行为一致。
  Result<NodeId, EditError> InsertSubsubsectionAfter(const NodeId &anchor,
                                                     InlineContent title,
                                                     NodeId id = NodeId{});
  Result<void, EditError> DeleteSubsubsection(size_t section_index,
                                              size_t subsection_index,
                                              size_t subsubsection_index);
  Result<void, EditError> MoveSubsubsection(size_t section_index,
                                            size_t subsection_index,
                                            size_t from, size_t to);

  Result<void, EditError> RenameSection(const NodeId &id,
                                        const InlineContent &title);
  Result<void, EditError> RenameSubsection(const NodeId &id,
                                           const InlineContent &title);
  Result<void, EditError> RenameSubsubsection(const NodeId &id,
                                              const InlineContent &title);

  // Block 变更
  Result<NodeId, EditError> InsertBlock(const NodeId &parent,
                                        std::optional<size_t> index,
                                        Block block, NodeId id = NodeId{});
  Result<void, EditError> DeleteBlock(const NodeId &id);
  Result<void, EditError> MoveBlock(const NodeId &id, const NodeId &new_parent,
                                    std::optional<size_t> new_index);

  Result<void, EditError> SetParagraphContent(const NodeId &id,
                                              const InlineContent &content);
  Result<void, EditError> SetCaption(const NodeId &id,
                                     const InlineContent &caption);
  Result<void, EditError>
  SetEquationSource(const NodeId &id, const std::string &source,
                    std::optional<bool> numbered = std::nullopt,
                    std::optional<std::string> label = std::nullopt);
  Result<void, EditError>
  SetFigureAsset(const NodeId &id, const AssetId &asset_id,
                 std::optional<FigureWidth> width = std::nullopt);
  Result<void, EditError> SetFigureAltText(const NodeId &id,
                                           const std::string &alt);
  // 单栏与双栏 figure。该值会被无条件存储，仅在双栏模板中才会产生
  // 可见效果（对于 DoubleColumn，renderer 会输出带星号的 float）。
  Result<void, EditError> SetFigureSpan(const NodeId &id, FigureSpan span);

  // Table 变更——强制维持矩形不变量。
  static Result<Table, EditError> MakeTable(std::vector<TableColumn> columns,
                                            size_t rows, bool has_header_row,
                                            NodeId id = NodeId{});
  Result<void, EditError> InsertTableRow(const NodeId &id, size_t index);
  Result<void, EditError> DeleteTableRow(const NodeId &id, size_t index);
  Result<void, EditError> InsertTableColumn(const NodeId &id, size_t index,
                                            ColumnAlignment alignment);
  Result<void, EditError> DeleteTableColumn(const NodeId &id, size_t index);
  Result<void, EditError> SetTableCell(const NodeId &id, size_t row,
                                       size_t column,
                                       const InlineContent &content);

  // 查找辅助函数（仅限编辑器内部使用的可变访问）
  Section *FindSection(const NodeId &id);
  Subsection *FindSubsection(const NodeId &id);
  Subsubsection *FindSubsubsection(const NodeId &id);
  // 拥有 subsubsection `id` 的 subsection，若无则返回 nullptr。
  Subsection *FindParentSubsubsection(const NodeId &id);
  Block *FindBlock(const NodeId &id);
  // （section_index、subsection_index 或 npos、subsubsection_index 或 npos、
  //  block index 或 npos）
  struct BlockPosition {
    size_t section_index;
    bool in_subsection = false;
    size_t subsection_index = 0;
    bool in_subsubsection = false;
    size_t subsubsection_index = 0;
    size_t block_index;
  };
  std::optional<BlockPosition> FindBlockPosition(const NodeId &id);
  // 某个 block 的各级容器 id，按插入顺序排列（section、subsection、
  // subsubsection）。move 逻辑用它来拒绝跨容器移动。
  std::vector<NodeId> BlockParents(const NodeId &id);

private:
  [[noreturn]] void Throw(EditError e) const;
  static std::vector<std::vector<TableCell>> MakeCells(size_t rows,
                                                       size_t cols);
  // section/subsection/subsubsection id 对应的 blocks vector。
  std::vector<Block> *FindBlockListForParent(const NodeId &parent);
  // 持有 node `id` 的 blocks vector 及其地址。
  std::vector<Block> *FindBlockListForNode(const NodeId &id);

  Document &document_;
};

} // namespace pf

namespace pf {
template <> EditError pf::ToStringError<EditError>(const std::string &value);
} // namespace pf
