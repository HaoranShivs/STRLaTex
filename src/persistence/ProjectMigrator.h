#pragma once
// ProjectMigrator: brings an old project file up to the current schema.
//
// Load flow (plan §15):
//   Load project -> detect schema version -> migrate -> current Document
//
// Rules:
//   * an old project must open,
//   * saving writes the current version,
//   * migration is unit tested,
//   * the on-disk file is never rewritten as a side effect of loading.

#include <string>

#include "persistence/ProjectPersistence.h"
#include "persistence/SchemaVersions.h"

namespace pf {

class ProjectMigrator {
public:
    // Schema version this build writes and understands.
    static const char* CurrentVersion();

    // True when `version` is understood by this build (possibly after
    // migration).
    static bool IsKnownVersion(const std::string& version);

    // Migrate a deserialized project to the current schema. Idempotent: a
    // project already at the current version is returned unchanged with
    // `migrated == false`.
    static MigrationResult MigrateToCurrent(SerializedProject* project);
};

}  // namespace pf
