#pragma once
// 项目序列化：project.paper（JSON）读写（架构 15、16、43、44）。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "asset/AssetManager.h"
#include "core/Diagnostic.h"
#include "core/Json.h"
#include "core/ProjectPath.h"
#include "core/Result.h"
#include "document/Document.h"
#include "persistence/SchemaVersions.h"
namespace pf {

struct SerializedProject {
    std::string project_id;
    std::string schema_version = kSchemaVersion;
    std::string template_id;
    Document document;
    std::vector<AssetMetadata> assets;
    std::string bibliography_path = "references.bib"; // 相对路径
    ProjectRevision revision;                         // 带类型的 revision（唯一事实来源）
};

class ProjectSerializer {
  public:
    // 序列化为 JSON 文本。
    static std::string Serialize(const SerializedProject& project);

    // 解析 JSON 文本。失败时返回错误字符串。
    // 输入会被完整校验（P0-02）：解析上限、schema 结构、
    // id/table/path 不变量。任何不可信的 .paper 载荷要么产出
    // 有效 project，要么返回结构化错误字符串——绝不抛出异常、
    // abort 或崩溃。
    static Result<SerializedProject, std::string> Deserialize(const std::string& json_text);
};

struct SaveRequest {
    std::string save_id;
    ProjectId project_id;
    ProjectRevision revision;
    std::filesystem::path destination; // project.paper 的路径
    SerializedProject snapshot;
};

struct SaveResult {
    // Queued：不可变 snapshot 已交给 save worker，但写入尚未被观察到。
    // 完成情况会以应用事件的形式到达（见 project/ApplicationEvent.h）。
    enum class Status { Ok, Queued, IoError, SerializeError };
    std::string save_id;
    ProjectRevision saved_revision;
    Status status = Status::Ok;
    std::string detail;
    std::optional<Diagnostic> diagnostic;
};

struct LoadRequest {
    std::filesystem::path project_file; // project.paper 路径
};

struct LoadResult {
    enum class Status { Ok, FileMissing, ParseError, SchemaError, TooLarge, IoError };
    Status status = Status::Ok;
    std::string detail;
    std::optional<SerializedProject> project;
    std::vector<Diagnostic> diagnostics;
    // 当文件由旧 schema 写入并在内存中升级到最新版本时，此字段非空。
    // 加载时，persistence 绝不重写文件。
    MigrationResult migration;
};

class ProjectPersistence {
  public:
    // 超过此大小的文件在读取前即被拒绝（P0-02）：损坏或恶意的 .paper
    // 文件无法让应用分配无界内存。
    static constexpr std::uintmax_t kMaxProjectFileBytes = 32 * 1024 * 1024;

    // 原子保存：序列化 -> 临时文件 -> 替换。
    static SaveResult Save(const SaveRequest& request);
    static LoadResult Load(const LoadRequest& request);
};

} // namespace pf
