#pragma once
// Schema version constants and migration reporting types.
// Kept free of SerializedProject/Document so both ProjectPersistence.h and
// ProjectMigrator.h can include it without a cycle.

#include <string>
#include <vector>

namespace pf {

// Schema versions. V1 documents have no third heading level; V2 adds
// Subsubsection. V3 is the math-redesign format: inline math is stored as
// {"type":"inline_math","latex":...} and display math as
// {"type":"equation","latex":...,"numbered":...,"label":...}. The reader still
// accepts the V2 spellings ("inlineEquation"/"displayEquation" with "math"),
// so a V2 file loads with no data change.
inline constexpr const char* kSchemaVersionV1 = "1";
inline constexpr const char* kSchemaVersionV2 = "2";
inline constexpr const char* kSchemaVersion = "3";

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
