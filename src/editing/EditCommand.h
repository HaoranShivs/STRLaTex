#pragma once
// Editing Protocol 值类型（架构 35-39）。

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/StrongId.h"
#include "document/Document.h"
#include "document/DocumentEditor.h"

namespace pf {

enum class EditOrigin : std::uint8_t {
    User,
    Undo,
    Redo,
    Recovery,
    System,
};

const char* ToString(EditOrigin origin);

// ---------------- 载荷 ----------------

struct SetTitlePayload {
    InlineContent title;
};

struct SetAbstractPayload {
    std::optional<InlineContent> abstract_text;
};

struct AddAuthorPayload {
    Author author;
};

struct RemoveAuthorPayload {
    size_t index = 0;
};

struct UpdateAuthorPayload {
    size_t index = 0;
    Author author;
};

// 整体替换署名单位列表。作者通过 id 引用署名单位，
// 因此在 V1 中整体替换更易保持一致。
struct SetAffiliationsPayload {
    std::vector<Affiliation> affiliations;
};

struct InsertSectionPayload {
    size_t index = 0;
    InlineContent title;
};

struct DeleteSectionPayload {
    size_t index = 0;
};

struct MoveSectionPayload {
    size_t from = 0;
    size_t to = 0;
};

struct RenameSectionPayload {
    NodeId section;
    InlineContent title;
};

struct InsertSubsectionPayload {
    size_t section_index = 0;
    size_t index = 0;
    InlineContent title;
};

struct RenameSubsectionPayload {
    NodeId subsection;
    InlineContent title;
};

struct MoveSubsectionPayload {
    size_t section_index = 0;
    size_t from = 0;
    size_t to = 0;
};

struct DeleteSubsectionPayload {
    size_t section_index = 0;
    size_t subsection_index = 0;
};

// 第三级标题。subsubsection 位于 subsection 内部。
struct InsertSubsubsectionPayload {
    size_t section_index = 0;
    size_t subsection_index = 0;
    size_t index = 0;
    InlineContent title;
};

struct RenameSubsubsectionPayload {
    NodeId subsubsection;
    InlineContent title;
};

struct MoveSubsubsectionPayload {
    size_t section_index = 0;
    size_t subsection_index = 0;
    size_t from = 0;
    size_t to = 0;
};

struct DeleteSubsubsectionPayload {
    size_t section_index = 0;
    size_t subsection_index = 0;
    size_t subsubsection_index = 0;
};

// 按阅读顺序在 `after` 之后直接插入 subsubsection 标题；
// 锚点之后的块会移入新建的 subsubsection。
struct InsertSubsubsectionAfterPayload {
    NodeId after;
    InlineContent title;
};

struct InsertParagraphPayload {
    NodeId parent;
    std::optional<size_t> index;
    InlineContent content;
};

struct InsertFigurePayload {
    NodeId parent;
    std::optional<size_t> index;
    AssetId asset_id;
    InlineContent caption;
    FigureWidth width = FigureWidth::Percent100;
    FigureSpan span = FigureSpan::SingleColumn;
};

struct InsertTablePayload {
    NodeId parent;
    std::optional<size_t> index;
    std::vector<TableColumn> columns;
    size_t rows = 1;
    bool has_header_row = false;
    InlineContent caption;
};

struct InsertEquationPayload {
    NodeId parent;
    std::optional<size_t> index;
    std::string latex;
    bool numbered = true;
    std::string label;
};

struct DeleteBlockPayload {
    NodeId node;
};

struct MoveBlockPayload {
    NodeId node;
    NodeId new_parent;
    std::optional<size_t> new_index;
};

// 按阅读顺序在 `after` 之后直接插入 subsection 标题；锚点之后的块会移入
// 新建的 subsection（参见 DocumentEditor::InsertSubsectionAfter）。
struct InsertSubsectionAfterPayload {
    NodeId after;
    InlineContent title;
};

struct EditParagraphPayload {
    NodeId paragraph;
    InlineContent content;
};

struct EditCaptionPayload {
    NodeId block;
    InlineContent caption;
};

struct EditEquationPayload {
    NodeId equation;
    std::string latex;
    std::optional<bool> numbered;
    std::optional<std::string> label;
};

// 图片的单栏与双栏。无论当前模板是否为双栏都会记录该值；
// 只有选中双栏模板后，它才会改变浮动体的渲染结果。
struct EditFigureSpanPayload {
    NodeId figure;
    FigureSpan span = FigureSpan::SingleColumn;
};

struct InsertCitationPayload {
    NodeId paragraph;
    std::vector<std::string> keys;
    CitationMode mode = CitationMode::Parenthetical;
    std::optional<size_t> at_index; // 行内位置；nullopt = 追加
};

struct InsertCrossReferencePayload {
    NodeId paragraph;
    NodeId target;
    std::optional<size_t> at_index;
};

struct SetKeywordsPayload {
    std::vector<std::string> keywords;
};

struct ChangeTemplatePayload {
    std::string template_id;
};
// 仅 Document 的载荷变体（不含模板变更）。
using EditPayload = std::variant<
    SetTitlePayload, SetAbstractPayload, SetKeywordsPayload, AddAuthorPayload, RemoveAuthorPayload, UpdateAuthorPayload,
    SetAffiliationsPayload, InsertSectionPayload, DeleteSectionPayload, MoveSectionPayload, RenameSectionPayload,
    RenameSubsectionPayload, MoveSubsectionPayload, InsertSubsectionPayload, InsertSubsectionAfterPayload,
    DeleteSubsectionPayload, InsertSubsubsectionPayload, RenameSubsubsectionPayload, MoveSubsubsectionPayload,
    DeleteSubsubsectionPayload, InsertSubsubsectionAfterPayload, InsertParagraphPayload, InsertFigurePayload,
    InsertTablePayload, InsertEquationPayload, DeleteBlockPayload, MoveBlockPayload, EditParagraphPayload,
    EditCaptionPayload, EditEquationPayload, EditFigureSpanPayload, InsertCitationPayload, InsertCrossReferencePayload>;

using FullEditPayload =
    std::variant<SetTitlePayload, SetAbstractPayload, SetKeywordsPayload, AddAuthorPayload, RemoveAuthorPayload,
                 UpdateAuthorPayload, SetAffiliationsPayload, InsertSectionPayload, DeleteSectionPayload,
                 MoveSectionPayload, RenameSectionPayload, RenameSubsectionPayload, MoveSubsectionPayload,
                 InsertSubsectionPayload, InsertSubsectionAfterPayload, DeleteSubsectionPayload,
                 InsertSubsubsectionPayload, RenameSubsubsectionPayload, MoveSubsubsectionPayload,
                 DeleteSubsubsectionPayload, InsertSubsubsectionAfterPayload, InsertParagraphPayload,
                 InsertFigurePayload, InsertTablePayload, InsertEquationPayload, DeleteBlockPayload, MoveBlockPayload,
                 EditParagraphPayload, EditCaptionPayload, EditEquationPayload, EditFigureSpanPayload,
                 InsertCitationPayload, InsertCrossReferencePayload, ChangeTemplatePayload>;

// ---------------- 信封 ----------------

enum class EditStatus : std::uint8_t {
    Applied,
    Rejected,
};

enum class FailureReason : std::uint8_t {
    None,
    InvalidTarget,
    ConstraintViolation,
    StaleOperation,
    WrongProject,
    UnknownPayload,
};

struct EditResult {
    EditStatus status = EditStatus::Rejected;
    ProjectRevision new_revision;
    NodeId created_node;
    FailureReason failure = FailureReason::None;
    std::string detail;

    static EditResult Ok(ProjectRevision rev, NodeId node = NodeId{}) {
        EditResult r;
        r.status = EditStatus::Applied;
        r.new_revision = rev;
        r.created_node = std::move(node);
        return r;
    }
    static EditResult Fail(FailureReason reason, std::string detail = {}) {
        EditResult r;
        r.failure = reason;
        r.detail = std::move(detail);
        return r;
    }
};

struct EditCommand {
    OperationId operation_id;
    ProjectId project_id;
    ProjectRevision base_revision;
    EditOrigin origin = EditOrigin::User;
    FullEditPayload payload;

    const char* PayloadName() const;
};

// 历史记录项：用于 undo/redo 的逆向信息。两种存储策略：
//  - SnapshotHistoryAction：整篇 Document 的前后 snapshot；适用于
//    所有文档操作（包括删除）。
//  - TemplateHistoryAction：用于项目级 undo 的模板 id 变更。
struct SnapshotHistoryAction {
    std::shared_ptr<const Document> before; // undo 时恢复
    std::shared_ptr<const Document> after;  // redo 时恢复
};

struct TemplateHistoryAction {
    std::string old_template;
    std::string new_template;
};

using HistoryAction = std::variant<SnapshotHistoryAction, TemplateHistoryAction>;

struct HistoryEntry {
    HistoryAction action;
    OperationId operation_id;
    ProjectRevision resulting_revision;
};

// 稳定锚点（架构 十）。
enum class AnchorBias : std::uint8_t {
    Before,
    After,
    InsideEnd,
};

struct StableNodeAnchor {
    NodeId reference_node;
    AnchorBias bias = AnchorBias::After;
};

} // namespace pf
