#pragma once
// schema 版本常量与迁移报告类型。
// 不依赖 SerializedProject/Document，使 ProjectPersistence.h 与
// ProjectMigrator.h 都能包含它而不产生循环依赖。

#include <string>
#include <vector>

namespace pf {

// schema 版本。V1 文档没有三级标题；V2 增加了 Subsubsection。
// V3 是数学重新设计的格式：行内数学存储为
// {"type":"inline_math","latex":...}，display 数学存储为
// {"type":"equation","latex":...,"numbered":...,"label":...}。读取器仍
// 接受 V2 的写法（"inlineEquation"/"displayEquation" 配 "math"），
// 因此 V2 文件加载后数据不变。
inline constexpr const char* kSchemaVersionV1 = "1";
inline constexpr const char* kSchemaVersionV2 = "2";
inline constexpr const char* kSchemaVersion = "3";

struct MigrationStep {
    std::string from_version;
    std::string to_version;
    std::string description;
};

struct MigrationResult {
    bool migrated = false;             // 某一步确实改变了文档
    std::string from_version;          // 从磁盘读取时的版本
    std::string to_version;            // 迁移后的版本
    std::vector<MigrationStep> applied;
    std::vector<std::string> warnings;
};

}  // namespace pf
