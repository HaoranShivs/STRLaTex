#pragma once
// Asset management (architecture section 17): import -> staging -> register.
// Document only stores AssetId; physical files live in project/assets/.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/StrongId.h"

namespace pf {

struct AssetMetadata {
    AssetId id;
    std::string relative_path;  // inside project assets/ dir
    std::string media_type;     // "image/png", "image/jpeg", ...
    std::string original_name;
    std::uint64_t file_size = 0;
    std::string content_hash;   // sha256-ish hex (FNV fallback in V1)
    // image dimensions (0 if unknown)
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

// ImportedAssetCandidate: staged but not yet registered (architecture 补充 3).
struct ImportedAssetCandidate {
    AssetId id;
    AssetMetadata metadata;
    std::filesystem::path staged_path;  // file already inside assets/ dir
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

    // Stage: copy file into assets dir (or report it is already there),
    // compute metadata + hash. Does NOT register (import/registration split).
    AssetImportResult Stage(const AssetImportRequest& request);
    ImportedAssetCandidate ToCandidate(const AssetImportResult& result) const;

    // Registration into the registry (formal project resource).
    void Register(ImportedAssetCandidate candidate);
    AssetRegistry& registry() noexcept { return registry_; }
    const std::filesystem::path& assets_dir() const noexcept { return assets_dir_; }

private:
    std::filesystem::path assets_dir_;
    AssetRegistry registry_;
};

}  // namespace pf
