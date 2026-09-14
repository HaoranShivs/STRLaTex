#pragma once
// EditingSystem: accepts EditCommands, mutates the document via
// DocumentEditor, maintains undo history and emits DocumentChangedEvents.
// Does not know about build/save/PDF/Qt (architecture sections 35-39).

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "document/DocumentIndex.h"
#include "editing/AnchorResolver.h"
#include "editing/EditCommand.h"
#include "editing/UndoHistory.h"

namespace pf {

enum class ChangeKind : std::uint8_t {
    MetadataChanged,
    StructureChanged,
    TextChanged,
    ReferenceChanged,
    AssetReferenceChanged,
    TemplateChanged,
};

struct DocumentChangedEvent {
    ProjectId project_id;
    ProjectRevision old_revision;
    ProjectRevision new_revision;
    EditOrigin origin = EditOrigin::User;
    std::vector<NodeId> affected_nodes;
    std::vector<ChangeKind> change_kinds;
};

class EditingSystem {
public:
    // Wiring used by ProjectSession; avoids a circular dependency by taking
    // a minimal interface (getters + mutation callbacks).
    struct Host {
        std::function<ProjectId()> project_id;
        std::function<ProjectRevision()> revision;
        std::function<ProjectRevision()> bump_revision;  // returns new revision
        std::function<Document&()> document;
        std::function<void(const DocumentChangedEvent&)> on_document_changed;
    };

    EditingSystem() = default;
    explicit EditingSystem(Host host) : host_(std::move(host)) {}

    // Late wiring (useful when the host lambdas capture the owner object).
    void SetHost(Host host) { host_ = std::move(host); }

    EditResult Apply(const EditCommand& command);
    EditResult Undo();
    EditResult Redo();

    UndoHistory& history() noexcept { return history_; }
    DocumentIndex& index() noexcept { return index_; }
    const DocumentIndex& index() const noexcept { return index_; }

    // Transaction key for coalescing consecutive typing into one undo entry.
    void BeginTypingTransaction(const NodeId& paragraph);
    void EndTypingTransaction();

private:
    EditResult ApplyDocumentPayload(const EditCommand& cmd, const EditPayload& payload);
    EditResult ApplyFullPayload(const EditCommand& cmd);

    void Notify(const EditCommand& cmd, ProjectRevision old_rev, ProjectRevision new_rev,
                std::vector<NodeId> affected, std::vector<ChangeKind> kinds);

    Host host_;
    UndoHistory history_;
    AnchorResolver anchor_resolver_;
    DocumentIndex index_;
};

}  // namespace pf
