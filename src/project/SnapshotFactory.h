#pragma once
// SnapshotFactory: immutable snapshot creation (architecture section 十六).
// Snapshots are the isolation wall between the application thread and
// background work: once captured, a worker only ever sees values.

#include <memory>

#include "build/BuildCoordinator.h"
#include "persistence/ProjectPersistence.h"
#include "project/ProjectState.h"

namespace pf {

// A self-contained save job payload. `serialized` owns a deep copy of the
// document, so the save worker never observes later edits.
struct SaveSnapshot {
    ProjectId project_id;
    ProjectRevision revision;
    std::string schema_version = kSchemaVersion;
    SerializedProject serialized;  // ready for persistence
};

class SnapshotFactory {
public:
    explicit SnapshotFactory(AssetManager* assets) : assets_(assets) {}

    // Capture an immutable build snapshot (document deep-copied, build id
    // minted here so the application thread knows the identity of the ask).
    BuildSnapshot CreateBuildSnapshot(const ProjectState& state,
                                      const std::string& bibliography_bibtex) const;

    // Capture a project snapshot for save/autosave/recovery.
    SaveSnapshot CreateSaveSnapshot(const ProjectState& state) const;

private:
    AssetManager* assets_;
};

}  // namespace pf
