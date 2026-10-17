#pragma once
// EditingSystem：接收 EditCommand，经由 DocumentEditor 修改文档，
// 维护 undo 历史并发出 DocumentChangedEvent。
// 不感知 build/save/PDF/Qt（架构 35-39）。

#include <functional>
#include <optional>
#include <string>
#include <vector>

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
        std::function<bool(const ChangeTemplatePayload&)> change_template;
        std::function<std::string()> template_selection;
        std::function<std::optional<TemplateBinding>()> template_binding;
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

  private:
    EditResult ApplyDocumentPayload(const EditCommand& cmd, const EditPayload& payload);
    EditResult ApplyFullPayload(const EditCommand& cmd);
    // Undo 与 Redo 的唯一实现：恢复 snapshot 或重放模板变更。
    EditResult ApplyHistoryEntry(const HistoryEntry& entry, EditOrigin origin);

    void Notify(const EditCommand& cmd, ProjectRevision old_rev, ProjectRevision new_rev, std::vector<NodeId> affected,
                std::vector<ChangeKind> kinds);

    Host host_;
    UndoHistory history_;
};

} // namespace pf
