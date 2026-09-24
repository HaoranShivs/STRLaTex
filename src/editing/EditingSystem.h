#pragma once
// EditingSystem：接收 EditCommand，经由 DocumentEditor 修改文档，
// 维护 undo 历史并发出 DocumentChangedEvent。
// 不感知 build/save/PDF/Qt（架构 35-39）。

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
    // ProjectSession 使用的接线方式；通过接受最小接口（getter 与变更回调）
    // 来避免循环依赖。
    struct Host {
        std::function<ProjectId()> project_id;
        std::function<ProjectRevision()> revision;
        std::function<ProjectRevision()> bump_revision; // 返回新的 revision
        std::function<Document&()> document;
        std::function<void(const DocumentChangedEvent&)> on_document_changed;
    };

    EditingSystem() = default;
    explicit EditingSystem(Host host) : host_(std::move(host)) {}

    // 延后接线（当 host lambda 捕获持有者对象时有用）。
    void SetHost(Host host) {
        host_ = std::move(host);
    }

    EditResult Apply(const EditCommand& command);
    EditResult Undo();
    EditResult Redo();

    UndoHistory& history() noexcept {
        return history_;
    }
    DocumentIndex& index() noexcept {
        return index_;
    }
    const DocumentIndex& index() const noexcept {
        return index_;
    }

    // 用于把连续输入合并为一条 undo 记录的 transaction key。
    void BeginTypingTransaction(const NodeId& paragraph);
    void EndTypingTransaction();

  private:
    EditResult ApplyDocumentPayload(const EditCommand& cmd, const EditPayload& payload);
    EditResult ApplyFullPayload(const EditCommand& cmd);

    void Notify(const EditCommand& cmd, ProjectRevision old_rev, ProjectRevision new_rev, std::vector<NodeId> affected,
                std::vector<ChangeKind> kinds);

    Host host_;
    UndoHistory history_;
    AnchorResolver anchor_resolver_;
    DocumentIndex index_;
};

} // namespace pf
