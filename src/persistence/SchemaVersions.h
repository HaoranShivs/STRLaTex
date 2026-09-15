#pragma once
// Schema version constants and migration reporting types.
// Kept free of SerializedProject/Document so both ProjectPersistence.h and
// ProjectMigrator.h can include it without a cycle.

#include <string>
#include <vector>

namespace pf {

// Schema versions. V1 documents have no third heading level; V2 adds
// Subsubsection and is the current on-disk format.
inline constexpr const char* kSchemaVersionV1 = "1";
inline constexpr const char* kSchemaVersion = "2";

struct MigrationStep {
    std::string from_version;
    std::string to_version;
    std::string description;
};

struct MigrationResult {
    bool migrated = false;             // a step actually changed the document
    std::string from_version;          // version as read from disk
    std::string to_version;            // version after migration
    std::vector<MigrationStep> applied;
    std::vector<std::string> warnings;
};

}  // namespace pf
