#pragma once
// 资源管理（架构 17）：导入 -> 暂存 -> 注册。
// Document 只保存 AssetId；物理文件存放在 project/assets/ 下。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/StrongId.h"

namespace pf {

struct AssetMetadata {
    AssetId id;
    std::string relative_path;  // 位于项目的 assets/ 目录内
    std::string media_type;     // "image/png"、"image/jpeg" 等
    std::string original_name;
    std::uint64_t file_size = 0;
    std::string content_hash;   // 类似 sha256 的十六进制串（V1 中回退为 FNV）
    // 图像尺寸（未知时为 0）
    std::uint64_t width = 0;
    std::uint64_t height = 0;
};

struct AssetImportRequest {
    std::filesystem::path source_path;
    ProjectId project_id;
};

struct AssetImportResult {
    enum class Status { Ok, SourceMissing, UnsupportedType, IoError };
    Status status = Status::Ok;
    std::string detail;
    AssetId asset_id;
    AssetMetadata metadata;
};

// ImportedAssetCandidate：已暂存但尚未注册（架构补充 3）。
struct ImportedAssetCandidate {
    AssetId id;
    AssetMetadata metadata;
    std::filesystem::path staged_path;  // 文件已在 assets/ 目录内
};

class AssetRegistry {
public:
    const AssetMetadata* Find(const AssetId& id) const;
    const std::unordered_map<AssetId, AssetMetadata>& All() const noexcept { return assets_; }

    void Register(AssetMetadata metadata);
    bool Unregister(const AssetId& id);

private:
    std::unordered_map<AssetId, AssetMetadata> assets_;
};

class AssetManager {
public:
    explicit AssetManager(std::filesystem::path assets_dir);

    // Stage：把文件复制到 assets 目录（或报告其已存在），
    // 计算元数据与哈希。不执行注册（导入与注册分离）。
    AssetImportResult Stage(const AssetImportRequest& request);
    ImportedAssetCandidate ToCandidate(const AssetImportResult& result) const;

    // 注册到 registry（成为正式的项目资源）。
    void Register(ImportedAssetCandidate candidate);
    AssetRegistry& registry() noexcept { return registry_; }
    const std::filesystem::path& assets_dir() const noexcept { return assets_dir_; }

private:
    std::filesystem::path assets_dir_;
    AssetRegistry registry_;
};

}  // namespace pf
