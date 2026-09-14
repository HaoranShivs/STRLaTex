#pragma once
// Editing Protocol value types (architecture sections 35-39).

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

// ---------------- Payloads ----------------

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

// Replace the whole affiliation list. Affiliations are referenced by id from
// authors, so a full-list replacement keeps consistency simple in V1.
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
    std::string math_source;
    bool numbered = true;
};

struct DeleteBlockPayload {
    NodeId node;
};

struct MoveBlockPayload {
    NodeId node;
    NodeId new_parent;
    std::optional<size_t> new_index;
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
    std::string math_source;
    std::optional<bool> numbered;
};

struct InsertCitationPayload {
    NodeId paragraph;
    std::vector<std::string> keys;
    CitationMode mode = CitationMode::Parenthetical;
    std::optional<size_t> at_index;  // inline position; nullopt = append
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
// Document-only payload variant (no template change).
using EditPayload = std::variant<
    SetTitlePayload,
    SetAbstractPayload,
    SetKeywordsPayload,
    AddAuthorPayload,
    RemoveAuthorPayload,
    UpdateAuthorPayload,
    SetAffiliationsPayload,
    InsertSectionPayload,
    DeleteSectionPayload,
    MoveSectionPayload,
    RenameSectionPayload,
    RenameSubsectionPayload,
    MoveSubsectionPayload,
    InsertSubsectionPayload,
    DeleteSubsectionPayload,
    InsertParagraphPayload,
    InsertFigurePayload,
    InsertTablePayload,
    InsertEquationPayload,
    DeleteBlockPayload,
    MoveBlockPayload,
    EditParagraphPayload,
    EditCaptionPayload,
    EditEquationPayload,
    InsertCitationPayload,
    InsertCrossReferencePayload
>;




using FullEditPayload = std::variant<
    SetTitlePayload,
    SetAbstractPayload,
    SetKeywordsPayload,
    AddAuthorPayload,
    RemoveAuthorPayload,
    UpdateAuthorPayload,
    SetAffiliationsPayload,
    InsertSectionPayload,
    DeleteSectionPayload,
    MoveSectionPayload,
    RenameSectionPayload,
    RenameSubsectionPayload,
    MoveSubsectionPayload,
    InsertSubsectionPayload,
    DeleteSubsectionPayload,
    InsertParagraphPayload,
    InsertFigurePayload,
    InsertTablePayload,
    InsertEquationPayload,
    DeleteBlockPayload,
    MoveBlockPayload,
    EditParagraphPayload,
    EditCaptionPayload,
    EditEquationPayload,
    InsertCitationPayload,
    InsertCrossReferencePayload,
    ChangeTemplatePayload
>;

// ---------------- Envelope ----------------

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

// History entry: inverse information for undo/redo. Two storage strategies:
//  - SnapshotHistoryAction: full-document before/after snapshots; works for
//    every document operation (including deletions).
//  - TemplateHistoryAction: template id transitions for project-level undo.
struct SnapshotHistoryAction {
    std::shared_ptr<const Document> before;  // restore on undo
    std::shared_ptr<const Document> after;   // restore on redo
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

// Stable anchor (architecture section 十).
enum class AnchorBias : std::uint8_t {
    Before,
    After,
    InsideEnd,
};

struct StableNodeAnchor {
    NodeId reference_node;
    AnchorBias bias = AnchorBias::After;
};

}  // namespace pf
