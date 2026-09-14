#pragma once
// SnapshotFactory: immutable snapshot creation (architecture section 十六).
// Snapshots are the isolation wall between UI thread and background work.

#include <memory>

#include "build/BuildCoordinator.h"
#include "persistence/ProjectPersistence.h"
#include "project/ProjectState.h"

namespace pf {

struct ProjectSnapshotData {
    ProjectId project_id;
    ProjectRevision revision;
    std::string schema_version = "1";
    SerializedProject serialized;  // ready for persistence
};

class SnapshotFactory {
public:
    explicit SnapshotFactory(AssetManager* assets) : assets_(assets) {}

    // Capture an immutable build snapshot (document deep-copied).
    BuildSnapshot CreateBuildSnapshot(const ProjectState& state,
                                      const std::string& bibliography_bibtex) const;

    // Capture a project snapshot for save/autosave/recovery.
    ProjectSnapshotData CreateProjectSnapshot(const ProjectState& state) const;

private:
    AssetManager* assets_;
};

}  // namespace pf
