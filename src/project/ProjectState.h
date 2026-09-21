#pragma once
// ProjectState：持有可变的项目聚合根（架构 十四）。
// ProjectRevision 的唯一所有者。

#include <string>

#include "asset/AssetManager.h"
#include "document/Document.h"
#include "template/TemplateRegistry.h"

namespace pf {

struct ProjectSettings {
    std::string name = "Untitled Paper";
    std::string bibliography_path = "references.bib";
};

class ProjectState {
public:
    ProjectId id() const noexcept { return id_; }
    ProjectRevision revision() const noexcept { return revision_; }
    const Document& document() const noexcept { return document_; }
    const TemplateSelection& template_selection() const noexcept { return template_; }
    const ProjectSettings& settings() const noexcept { return settings_; }
    std::uint64_t document_version() const noexcept { return document_.version().value; }

    Document& mutable_document() noexcept { return document_; }
    TemplateSelection& mutable_template() noexcept { return template_; }
    ProjectSettings& mutable_settings() noexcept { return settings_; }

    // revision 管理——仅由 ProjectSession／EditingSystem 调用。
    ProjectRevision BumpRevision() noexcept {
        revision_.value += 1;
        return revision_;
    }
    void SetRevision(ProjectRevision rev) noexcept { revision_ = rev; }
    void SetId(ProjectId id) noexcept { id_ = std::move(id); }

    void Reset() {
        document_ = Document{};
        template_ = "generic-article";
        revision_ = ProjectRevision{0};
    }

private:
    ProjectId id_;
    ProjectRevision revision_{0};
    Document document_;
    TemplateSelection template_ = "generic-article";
    ProjectSettings settings_;
};

}  // namespace pf
