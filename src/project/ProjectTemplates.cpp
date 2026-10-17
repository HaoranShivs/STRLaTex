#include "build/TemplateVerifier.h"
#include "project/ProjectSession.h"
#include <algorithm>

namespace pf {
std::shared_ptr<const TemplateDefinition>
ProjectSession::ResolveTemplate(const std::string& id) const {
    if (auto it = imported_templates_.find(id); it != imported_templates_.end())
        return it->second;
    if (const auto* def = TemplateRegistry::Instance().Find(id))
        return std::make_shared<const TemplateDefinition>(*def);
    return {};
}
std::vector<TemplateDefinition> ProjectSession::AvailableTemplates() const {
    auto result = TemplateRegistry::Instance().All();
    for (const auto& [id, definition] : imported_templates_) {
        // UI consumers need metadata, not another copy of all template bytes.
        auto metadata = *definition;
        metadata.resources.clear();
        result.push_back(std::move(metadata));
    }
    return result;
}
void ProjectSession::LoadTemplateCatalog() {
    imported_templates_.clear();
    template_catalog_error_.clear();
    auto catalog = LoadProjectTemplates(paths_.project_dir);
    if (!catalog) {
        template_catalog_error_ = catalog.error();
        return;
    }
    for (auto& def : catalog.value()) {
        auto id = def.id;
        imported_templates_.emplace(
            id, std::make_shared<const TemplateDefinition>(std::move(def)));
    }
    // Older projects had only a built-in ID. Pin without rewriting the file on
    // open.
    if (!state_.template_binding())
        if (auto def = ResolveTemplate(state_.template_selection()))
            state_.SetTemplateBinding(def->binding);
}
Result<void>
ProjectSession::InstallVerifiedTemplate(const VerifiedTemplatePackage& verified,
                                        const std::string& id) {
    NoteOwnerThreadUse();
    if (lifecycle_state_ != LifecycleState::Open)
        return Unexpected("Open a project before importing a template");
    const auto& package = verified.package;
    if (verified.probes.size() != package.profiles.size())
        return Unexpected("All template profiles must pass verification");
    bool selected = false;
    for (const auto& def : package.profiles) {
        if (def.id == id)
            selected = true;
        if (std::none_of(verified.probes.begin(), verified.probes.end(),
                         [&](const auto& probe) {
                             return probe.profile_id == def.id &&
                                    !probe.plan_digest.empty();
                         }))
            return Unexpected("Missing template verification evidence");
    }
    if (!selected)
        return Unexpected("Selected template profile is unavailable");
    auto installed = InstallTemplatePackage(paths_.project_dir, package);
    if (!installed)
        return installed;
    // Publish immutable resources first, then make the binding part of project
    // history/save.
    auto stored = LoadProjectTemplates(paths_.project_dir);
    if (!stored)
        return Unexpected(stored.error());
    for (auto& def : stored.value()) {
        const auto stored_id = def.id;
        imported_templates_[stored_id] =
            std::make_shared<const TemplateDefinition>(std::move(def));
    }
    ChangeTemplate(id);
    return {};
}
} // namespace pf
