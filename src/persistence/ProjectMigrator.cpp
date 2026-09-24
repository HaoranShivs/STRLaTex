#include "persistence/ProjectMigrator.h"

#include <algorithm>

namespace pf {

const char* ProjectMigrator::CurrentVersion() {
    return kSchemaVersion;
}

bool ProjectMigrator::IsKnownVersion(const std::string& version) {
    return version == kSchemaVersionV1 || version == kSchemaVersionV2 || version == kSchemaVersion;
}

MigrationResult ProjectMigrator::MigrateToCurrent(SerializedProject* project) {
    MigrationResult result;
    if (!project)
        return result;
    result.from_version = project->schema_version;

    if (project->schema_version.empty()) {
        // 非常旧或手写的文件：视为 V1 并记录一条警告。
        project->schema_version = kSchemaVersionV1;
        result.from_version = kSchemaVersionV1;
        result.warnings.push_back("project file has no schemaVersion; assumed " + std::string(kSchemaVersionV1));
    }

    if (!IsKnownVersion(project->schema_version)) {
        result.warnings.push_back("unknown schemaVersion " + project->schema_version + " - loaded as-is");
        project->schema_version = kSchemaVersion;
        result.to_version = kSchemaVersion;
        return result;
    }

    // V1 -> V2：引入了第三级标题。V1 文档完全没有 subsubsection 字段，
    // 因此内存中的表示本就正确（每个 Subsection 都以空的 subsubsections
    // vector 开始）；本次迁移只是打一个版本戳，外加一条该文档按新 schema
    // 解释的说明。没有任何内容被丢弃或重命名，因此该步骤在构造上就是无损的。
    if (project->schema_version == kSchemaVersionV1) {
        MigrationStep step;
        step.from_version = kSchemaVersionV1;
        step.to_version = kSchemaVersionV2;
        step.description = "subsubsection support (no data change)";
        result.applied.push_back(step);
        result.migrated = true;
        project->schema_version = kSchemaVersionV2;
    }

    // V2 -> V3：math 迁移到 MathExpression。读取器一直同时理解旧写法
    // （"inlineEquation"/"displayEquation" + "math"）和新写法
    // （"inline_math"/"equation" + "latex"），因此该步骤只是一个版本戳：
    // 不丢弃任何内容，存储的源码原样保留。
    if (project->schema_version == kSchemaVersionV2) {
        MigrationStep step;
        step.from_version = kSchemaVersionV2;
        step.to_version = kSchemaVersion;
        step.description = "math expression format: MathExpression with a bare LaTeX body "
                           "(no data change)";
        result.applied.push_back(step);
        result.migrated = true;
        project->schema_version = kSchemaVersion;
    }

    result.to_version = project->schema_version;
    return result;
}

} // namespace pf
