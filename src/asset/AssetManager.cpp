#include "asset/AssetManager.h"

#include <cstring>
#include <fstream>
#include <functional>

#include "core/IdGenerator.h"

namespace pf {

namespace {

std::string DetectMediaType(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".eps") return "application/postscript";
    return {};
}

std::string FileHash(const std::filesystem::path& path) {
    // FNV-1a 64-bit as a cheap V1 content hash (not cryptographic).
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::uint64_t hash = 14695981039346656037ull;
    char buf[8192];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ull;
        }
        if (!in) break;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
    return out;
}

// Parse PNG/JPEG/GIF intrinsic dimensions (header sniffing).
void SniffImageSize(const std::filesystem::path& path, std::uint64_t& width,
                    std::uint64_t& height) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    unsigned char header[32] = {0};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    auto read_u32_be = [&](size_t off) {
        return (static_cast<std::uint64_t>(header[off]) << 24) |
               (static_cast<std::uint64_t>(header[off + 1]) << 16) |
               (static_cast<std::uint64_t>(header[off + 2]) << 8) |
               static_cast<std::uint64_t>(header[off + 3]);
    };
    // PNG: 8-byte signature, then IHDR: width at 16, height at 20 (big-endian)
    static const unsigned char png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(header, png_sig, 8) == 0) {
        width = read_u32_be(16);
        height = read_u32_be(20);
        return;
    }
    // GIF: "GIF8?a", little-endian width at 6, height at 8
    if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
        width = static_cast<std::uint64_t>(header[6]) |
                (static_cast<std::uint64_t>(header[7]) << 8);
        height = static_cast<std::uint64_t>(header[8]) |
                 (static_cast<std::uint64_t>(header[9]) << 8);
        return;
    }
    // JPEG: scan markers for SOFn
    if (header[0] == 0xFF && header[1] == 0xD8) {
        in.clear();
        in.seekg(2);
        while (in.good()) {
            unsigned char marker[2] = {0, 0};
            in.read(reinterpret_cast<char*>(marker), 2);
            if (marker[0] != 0xFF) break;
            unsigned char m = marker[1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
            unsigned char len[2] = {0, 0};
            in.read(reinterpret_cast<char*>(len), 2);
            size_t seg_len = (static_cast<size_t>(len[0]) << 8) | len[1];
            if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
                (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {
                unsigned char sof[5] = {0};
                in.read(reinterpret_cast<char*>(sof), 5);
                height = (static_cast<std::uint64_t>(sof[1]) << 8) | sof[2];
                width = (static_cast<std::uint64_t>(sof[3]) << 8) | sof[4];
                return;
            }
            in.seekg(static_cast<std::streamoff>(seg_len) - 2, std::ios::cur);
        }
    }
}

}  // namespace

AssetManager::AssetManager(std::filesystem::path assets_dir)
    : assets_dir_(std::move(assets_dir)) {
    std::error_code ec;
    std::filesystem::create_directories(assets_dir_, ec);
}

const AssetMetadata* AssetRegistry::Find(const AssetId& id) const {
    auto it = assets_.find(id);
    return it == assets_.end() ? nullptr : &it->second;
}

void AssetRegistry::Register(AssetMetadata metadata) {
    assets_[metadata.id] = std::move(metadata);
}

bool AssetRegistry::Unregister(const AssetId& id) { return assets_.erase(id) > 0; }

AssetImportResult AssetManager::Stage(const AssetImportRequest& request) {
    AssetImportResult result;
    const auto& src = request.source_path;
    if (!std::filesystem::exists(src)) {
        result.status = AssetImportResult::Status::SourceMissing;
        result.detail = "source file does not exist: " + src.string();
        return result;
    }
    std::string media = DetectMediaType(src);
    if (media.empty() || media == "application/pdf" || media == "image/svg+xml") {
        // V1 accepts raster images only for PDF embedding.
        result.status = AssetImportResult::Status::UnsupportedType;
        result.detail = "unsupported asset type: " + (media.empty() ? std::string("unknown") : media);
        return result;
    }
    std::error_code ec;
    auto filename = src.filename();
    auto dest = assets_dir_ / filename;
    // Avoid overwrite: prefix with asset counter when the name exists.
    if (std::filesystem::exists(dest)) {
        dest = assets_dir_ / (IdGenerator::NewAssetId() + "_" + filename.string());
    }
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        result.status = AssetImportResult::Status::IoError;
        result.detail = ec.message();
        return result;
    }

    AssetMetadata meta;
    meta.id = IdGenerator::NewAsset();
    meta.relative_path = dest.filename().string();
    meta.media_type = media;
    meta.original_name = filename.string();
    meta.file_size = std::filesystem::file_size(dest, ec);
    meta.content_hash = FileHash(dest);
    SniffImageSize(dest, meta.width, meta.height);

    result.status = AssetImportResult::Status::Ok;
    result.asset_id = meta.id;
    result.metadata = meta;
    return result;
}

ImportedAssetCandidate AssetManager::ToCandidate(const AssetImportResult& result) const {
    ImportedAssetCandidate candidate;
    candidate.id = result.metadata.id;
    candidate.metadata = result.metadata;
    candidate.staged_path = assets_dir_ / result.metadata.relative_path;
    return candidate;
}

void AssetManager::Register(ImportedAssetCandidate candidate) {
    registry_.Register(std::move(candidate.metadata));
}

}  // namespace pf
