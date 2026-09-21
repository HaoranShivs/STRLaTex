#pragma once
// ProjectMigrator：把旧项目文件升级到当前 schema。
//
// 加载流程（方案 §15）：
//   加载 project -> 检测 schema version -> migrate -> 当前 Document
//
// 规则：
//   * 旧 project 必须能打开，
//   * 保存时写入当前版本，
//   * migration 需有单元测试，
//   * 加载过程绝不以副作用形式重写磁盘文件。

#include <string>

#include "persistence/ProjectPersistence.h"
#include "persistence/SchemaVersions.h"

namespace pf {

class ProjectMigrator {
public:
    // 本 build 写入并理解的 schema 版本。
    static const char* CurrentVersion();

    // 当本 build 能理解 `version` 时返回 true（可能需经 migration）。
    static bool IsKnownVersion(const std::string& version);

    // 将反序列化后的 project 迁移到当前 schema。幂等：已处于当前版本的
    // project 原样返回，且 `migrated == false`。
    static MigrationResult MigrateToCurrent(SerializedProject* project);
};

}  // namespace pf
