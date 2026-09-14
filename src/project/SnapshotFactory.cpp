#include "project/SnapshotFactory.h"

#include "core/IdGenerator.h"
#include "persistence/ProjectPersistence.h"

namespace pf {

BuildSnapshot SnapshotFactory::CreateBuildSnapshot(
    const ProjectState& state, const std::string& bibliography_bibtex) const {
    BuildSnapshot snapshot;
    snapshot.project_id = state.id();
    snapshot.snapshot_id = IdGenerator::NewSnapshotId();
    snapshot.revision = state.revision();
    // Immutable deep copy of the document.
    snapshot.document = std::make_shared<const Document>(state.document());
    snapshot.template_id = state.template_selection();
    snapshot.bibliography_bibtex = bibliography_bibtex;
    // Asset manifest: asset id -> relative path (resolved by the compiler
    // workspace staging).
    for (const auto& [id, meta] : assets_->registry().All()) {
        snapshot.asset_files[id.value()] = meta.relative_path;
    }
    return snapshot;
}

ProjectSnapshotData SnapshotFactory::CreateProjectSnapshot(
    const ProjectState& state) const {
    ProjectSnapshotData data;
    data.project_id = state.id();
    data.revision = state.revision();
    data.serialized.project_id = state.id().value();
    data.serialized.revision = state.revision();
    data.serialized.template_id = state.template_selection();
    data.serialized.document = state.document();
    data.serialized.bibliography_path = state.settings().bibliography_path;
    for (const auto& [id, meta] : assets_->registry().All()) {
        data.serialized.assets.push_back(meta);
    }
    return data;
}

}  // namespace pf
