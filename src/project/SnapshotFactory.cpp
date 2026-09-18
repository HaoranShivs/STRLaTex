#include "project/SnapshotFactory.h"

#include "core/IdGenerator.h"
#include "core/ProjectPath.h"
#include "persistence/ProjectPersistence.h"

namespace pf {

BuildSnapshot SnapshotFactory::CreateBuildSnapshot(
    const ProjectState &state, const std::string &bibliography_bibtex) const {
  BuildSnapshot snapshot;
  snapshot.project_id = state.id();
  snapshot.snapshot_id = IdGenerator::NewSnapshotId();
  snapshot.build_id = BuildId(IdGenerator::NewBuildId());
  snapshot.revision = state.revision();
  // Immutable deep copy of the document.
  snapshot.document = std::make_shared<const Document>(state.document());
  snapshot.template_id = state.template_selection();
  snapshot.bibliography_bibtex = bibliography_bibtex;
  // Asset manifest: asset id -> relative path (resolved by the compiler
  // workspace staging). The staged destination keeps the source file name
  // and extension under the package's assets/ directory, which is exactly
  // what the renderer emits in \includegraphics and what pdfLaTeX needs to
  // recognise the graphics format.
  //
  // P0-04 (second check): every stored relative path is re-validated here,
  // immediately before it is turned into a filesystem source. An asset whose
  // path escapes the project (malformed or hand-edited project.paper, or a
  // registry entry mutated in memory) is skipped - the renderer then reports
  // the missing-asset state instead of copying an arbitrary file into the
  // build workspace.
  for (const auto &[id, meta] : assets_->registry().All()) {
    auto safe = ResolveUntrustedProjectPath(assets_->assets_dir(),
                                            meta.relative_path);
    if (!safe.ok())
      continue;
    snapshot.asset_files[id.value()] = meta.relative_path;
    snapshot.asset_sources["assets/" + meta.relative_path] = safe.value();
  }
  return snapshot;
}

SaveSnapshot
SnapshotFactory::CreateSaveSnapshot(const ProjectState &state) const {
  SaveSnapshot data;
  data.project_id = state.id();
  data.revision = state.revision();
  data.serialized.project_id = state.id().value();
  data.serialized.revision = state.revision();
  data.serialized.template_id = state.template_selection();
  data.serialized.document = state.document();
  data.serialized.bibliography_path = state.settings().bibliography_path;
  for (const auto &[id, meta] : assets_->registry().All()) {
    data.serialized.assets.push_back(meta);
  }
  return data;
}

} // namespace pf
