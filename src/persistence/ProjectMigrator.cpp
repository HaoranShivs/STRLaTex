#include "persistence/ProjectMigrator.h"

#include <algorithm>

namespace pf {

const char* ProjectMigrator::CurrentVersion() { return kSchemaVersion; }

bool ProjectMigrator::IsKnownVersion(const std::string& version) {
    return version == kSchemaVersionV1 || version == kSchemaVersion;
}

MigrationResult ProjectMigrator::MigrateToCurrent(SerializedProject* project) {
    MigrationResult result;
    if (!project) return result;
    result.from_version = project->schema_version;

    if (project->schema_version.empty()) {
        // Very old or hand-written files: treat as V1 and record a warning.
        project->schema_version = kSchemaVersionV1;
        result.from_version = kSchemaVersionV1;
        result.warnings.push_back(
            "project file has no schemaVersion; assumed " +
            std::string(kSchemaVersionV1));
    }

    if (!IsKnownVersion(project->schema_version)) {
        result.warnings.push_back("unknown schemaVersion " +
                                  project->schema_version +
                                  " - loaded as-is");
        project->schema_version = kSchemaVersion;
        result.to_version = kSchemaVersion;
        return result;
    }

    if (project->schema_version == kSchemaVersion) {
        result.to_version = kSchemaVersion;
        return result;
    }

    // V1 -> V2: the third heading level was introduced. A V1 document has no
    // subsubsection field at all, so the in-memory representation is already
    // correct (every Subsection starts with an empty subsubsections vector);
    // the migration is a version stamp plus a note that the document was
    // interpreted under the new schema. Nothing is dropped or renamed, which
    // is why the step is lossless by construction.
    if (project->schema_version == kSchemaVersionV1) {
        MigrationStep step;
        step.from_version = kSchemaVersionV1;
        step.to_version = kSchemaVersion;
        step.description = "subsubsection support (no data change)";
        result.applied.push_back(step);
        result.migrated = true;
        project->schema_version = kSchemaVersion;
    }

    result.to_version = project->schema_version;
    return result;
}

}  // namespace pf
