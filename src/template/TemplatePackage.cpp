#include "template/TemplatePackage.h"

#include "core/ContentHash.h"
#include "core/IdGenerator.h"
#include "core/ProjectPath.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <zlib.h>

namespace pf {
namespace {
constexpr std::size_t kTotalLimit = 64 * 1024 * 1024;
constexpr std::size_t kFileLimit = 16 * 1024 * 1024;
constexpr std::size_t kCountLimit = 256;

bool BuildResource(const std::string& name) {
    const auto extension = std::filesystem::path(name).extension().string();
    return extension == ".cls" || extension == ".sty" || extension == ".bst" ||
           name.starts_with("thumbnails/");
}

bool Token(const std::string& value) {
    return !value.empty() && value.size() <= 100 &&
           ((value.front() >= 'a' && value.front() <= 'z') ||
            (value.front() >= 'A' && value.front() <= 'Z') ||
            (value.front() >= '0' && value.front() <= '9')) &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '.';
           });
}
bool Digest(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
Result<std::string> Read(const std::filesystem::path& path, std::size_t limit) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit)
        return Unexpected("Cannot read template file (missing or too large): " +
                          path.string());
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(bytes.data(), static_cast<std::streamsize>(size)))
        return Unexpected("Cannot read template file: " + path.string());
    return bytes;
}

// Deliberately bounded ZIP32 reader. Only stored/deflated regular files are
// accepted. Central directory + local headers must agree; no external unzip
// commands are executed.
Result<std::map<std::string, std::string>> ReadZip(const std::string& bytes) {
    const auto u16 = [&](std::size_t p) -> std::uint32_t {
        if (p + 2 > bytes.size())
            throw std::runtime_error("Truncated ZIP header");
        return static_cast<unsigned char>(bytes[p]) |
               (static_cast<unsigned char>(bytes[p + 1]) << 8);
    };
    const auto u32 = [&](std::size_t p) -> std::uint32_t {
        return u16(p) | (u16(p + 2) << 16);
    };
    try {
        if (bytes.size() < 22)
            return Unexpected("Not a ZIP archive");
        std::size_t end = bytes.size() - 22;
        const auto first = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
        while (u32(end) != 0x06054b50 ||
               end + 22 + u16(end + 20) != bytes.size()) {
            if (end == first)
                return Unexpected("ZIP end record not found");
            --end;
        }
        const auto count = u16(end + 10), directory = u32(end + 16),
                   length = u32(end + 12);
        if (u16(end + 4) || u16(end + 6) || u16(end + 8) != count ||
            count > kCountLimit || directory > end || length != end - directory)
            return Unexpected(
                "Unsupported multi-volume/ZIP64 archive or too many entries");
        std::map<std::string, std::string> files;
        std::set<std::string> targets;
        std::vector<std::pair<std::size_t, std::size_t>> spans;
        std::size_t p = directory, total = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (p + 46 > end || u32(p) != 0x02014b50)
                return Unexpected("Invalid ZIP directory");
            const auto flags = u16(p + 8), method = u16(p + 10),
                       crc = u32(p + 16);
            const auto compressed = u32(p + 20), size = u32(p + 24),
                       local = u32(p + 42);
            const auto namesize = u16(p + 28), extra = u16(p + 30),
                       comment = u16(p + 32);
            if (p + 46 + namesize + extra + comment > end || !namesize ||
                u16(p + 34))
                return Unexpected("Invalid ZIP entry");
            std::string name = bytes.substr(p + 46, namesize);
            const bool is_dir = name.back() == '/';
            if (is_dir)
                name.pop_back();
            // Reject backslashes, aliases, links and device files on every host
            // platform.
            const auto mode = (u32(p + 38) >> 16) & 0170000;
            auto path = ProjectRelativePath::Parse(name);
            if (!path.ok() || path.value().value() != name ||
                name.find('\\') != std::string::npos ||
                (mode && mode != (is_dir ? 0040000 : 0100000)) ||
                !targets.insert(name).second)
                return Unexpected("Unsafe or duplicate ZIP path: " + name);
            if ((flags & ~0x080eU) || (method != 0 && method != 8) ||
                size > kFileLimit || total + size > kTotalLimit ||
                (size > 1024 * 1024 &&
                 size > 200ULL * std::max(1U, compressed)))
                return Unexpected("Unsupported/encrypted ZIP entry or "
                                  "decompression limit exceeded: " +
                                  name);
            if (local + 30ULL > directory || u32(local) != 0x04034b50 ||
                u16(local + 6) != flags || u16(local + 8) != method ||
                u16(local + 26) != namesize ||
                bytes.substr(local + 30, namesize) !=
                    bytes.substr(p + 46, namesize))
                return Unexpected("ZIP local header mismatch: " + name);
            if (!(flags & 8) &&
                (u32(local + 14) != crc || u32(local + 18) != compressed ||
                 u32(local + 22) != size))
                return Unexpected("ZIP sizes/CRC disagree: " + name);
            const std::size_t start =
                local + 30ULL + namesize + u16(local + 28);
            if (start > directory || compressed > directory - start)
                return Unexpected("Truncated ZIP data");
            for (auto [a, b] : spans)
                if (local < b && start + compressed > a)
                    return Unexpected("Overlapping ZIP entries");
            spans.emplace_back(local, start + compressed);
            std::string output(size, '\0');
            if (method == 0) {
                if (compressed != size)
                    return Unexpected("Invalid stored ZIP size");
                output.assign(bytes, start, size);
            } else {
                z_stream stream{};
                stream.next_in = reinterpret_cast<Bytef*>(
                    const_cast<char*>(bytes.data() + start));
                stream.avail_in = compressed;
                unsigned char dummy = 0;
                stream.next_out =
                    size ? reinterpret_cast<Bytef*>(output.data()) : &dummy;
                stream.avail_out = size ? size : 1;
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                    return Unexpected("ZIP inflater unavailable");
                const int status = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                if (status != Z_STREAM_END || stream.total_out != size ||
                    stream.total_in != compressed)
                    return Unexpected("Invalid compressed ZIP data: " + name);
            }
            if (crc32(0, reinterpret_cast<const Bytef*>(output.data()),
                      output.size()) != crc)
                return Unexpected("ZIP checksum failed: " + name);
            total += size;
            if (is_dir) {
                if (size)
                    return Unexpected("ZIP directory contains data");
            } else
                files.emplace(std::move(name), std::move(output));
            p += 46 + namesize + extra + comment;
        }
        if (p != end)
            return Unexpected("Invalid ZIP directory length");
        for (const auto& [name, content] : files) {
            for (auto parent = std::filesystem::path(name).parent_path();
                 !parent.empty(); parent = parent.parent_path())
                if (files.contains(parent.generic_string()))
                    return Unexpected("ZIP file/directory collision");
        }
        if (files.empty())
            return Unexpected("Empty template archive");
        // Publisher ZIPs commonly contain one wrapping directory. Remove it
        // once.
        const auto root =
            files.begin()->first.substr(0, files.begin()->first.find('/') + 1);
        if (!root.empty() &&
            std::all_of(files.begin(), files.end(), [&](const auto& f) {
                return f.first.starts_with(root);
            })) {
            std::map<std::string, std::string> unwrapped;
            for (auto& [name, content] : files)
                unwrapped.emplace(name.substr(root.size()), std::move(content));
            files = std::move(unwrapped);
        }
        return files;
    } catch (const std::exception& e) {
        return Unexpected(e.what());
    }
}

JsonValue Inventory(const std::map<std::string, std::string>& files) {
    JsonObject result;
    for (const auto& [name, bytes] : files)
        result[name] = Sha256(bytes);
    return result;
}

Result<TemplatePackage> Describe(std::map<std::string, std::string> files) {
    TemplatePackage package;
    package.files = std::move(files);
    std::size_t total = 0;
    if (package.files.empty() || package.files.size() > kCountLimit)
        return Unexpected("Invalid template file count");
    for (const auto& [name, bytes] : package.files) {
        auto path = ProjectRelativePath::Parse(name);
        if (!path || path.value().value() != name ||
            name.find('\\') != std::string::npos || bytes.size() > kFileLimit)
            return Unexpected("Unsafe or oversized template resource: " + name);
        total += bytes.size();
        if (total > kTotalLimit)
            return Unexpected("Template resource limit exceeded");
        for (auto parent = std::filesystem::path(name).parent_path();
             !parent.empty(); parent = parent.parent_path())
            if (package.files.contains(parent.generic_string()))
                return Unexpected("Template file/directory collision");
    }
    JsonValue descriptor;
    if (auto it = package.files.find("paperforge-template.json");
        it != package.files.end()) {
        std::string error;
        auto json = JsonParse(it->second, &error);
        if (!json || !json->is_object())
            return Unexpected("Invalid paperforge-template.json: " + error);
        descriptor = *json;
    } else if (package.files.contains("cas-sc.cls") &&
               package.files.contains("cas-dc.cls") &&
               package.files.contains("cas-common.sty") &&
               package.files.contains("cas-model2-names.bst")) {
        std::string version;
        const std::regex version_pattern(
            R"(\\def\s*\\RCSversion\{([0-9.]+)\})");
        for (const auto& cls : {"cas-sc", "cas-dc"}) {
            const auto& source = package.files.at(std::string(cls) + ".cls");
            std::smatch match;
            if (!std::regex_search(source, match, version_pattern) ||
                source.find("\\RequirePackage{cas-common}") ==
                    std::string::npos)
                return Unexpected("CAS class interface not recognized; an "
                                  "adapter is required");
            if (version.empty())
                version = match[1];
            else if (version != match[1])
                return Unexpected("CAS class versions disagree");
        }
        JsonArray profiles;
        for (const auto& cls : {"cas-sc", "cas-dc"}) {
            profiles.push_back(JsonObject{
                {"id", std::string("elsevier-") + cls},
                {"name", std::string("Elsevier CAS ") +
                             (std::string(cls) == "cas-sc" ? "Single Column"
                                                           : "Double Column")},
                {"class", cls},
                {"frontmatter", "elsevier-cas-v1"},
                {"twoColumn", std::string(cls) == "cas-dc"},
                {"classOptions", JsonArray{"a4paper", "fleqn"}},
                {"bibliographyStyle", "unsrtnat"}});
        }
        descriptor = JsonObject{
            {"apiVersion", 1}, {"version", version}, {"profiles", profiles}};
    } else
        return Unexpected(
            "NeedsAdapter: ZIP resources are valid, but no supported template "
            "adapter or paperforge-template.json was found");

    const auto* api = descriptor.find("apiVersion");
    const auto* version = descriptor.find("version");
    const auto* profiles = descriptor.find("profiles");
    if (!api || !api->is_number() || api->as_double() != 1 || !version ||
        !Token(version->as_string()) || !profiles || !profiles->is_array() ||
        profiles->as_array().empty() || profiles->as_array().size() > 16)
        return Unexpected(
            "Unsupported template API or invalid version/profiles");
    for (const auto& [key, val] : descriptor.as_object())
        if (key != "apiVersion" && key != "version" && key != "profiles")
            return Unexpected("Unsupported manifest field: " + key);
    std::size_t resource_bytes = 0;
    for (const auto& [name, bytes] : package.files)
        if (BuildResource(name))
            resource_bytes += bytes.size();
    if (resource_bytes > kTotalLimit / profiles->as_array().size())
        return Unexpected(
            "Template profile resource expansion exceeds memory limit");
    package.version = version->as_string();
    package.digest = Sha256("paperforge-template-api-1\n" + descriptor.Dump(0) +
                            "\n" + Inventory(package.files).Dump(0));
    std::set<std::string> ids;
    for (const auto& profile : profiles->as_array()) {
        for (const auto& [key, val] : profile.as_object())
            if (key != "id" && key != "name" && key != "class" &&
                key != "frontmatter" && key != "twoColumn" &&
                key != "classOptions" && key != "bibliographyStyle")
                return Unexpected("Unsupported profile field: " + key);
        const auto get = [&](const char* key) {
            auto v = profile.find(key);
            return v ? v->as_string() : std::string{};
        };
        TemplateDefinition def;
        const auto id = get("id");
        def.name = get("name");
        def.document_class = get("class");
        def.frontmatter = get("frontmatter");
        def.bibliography_style = get("bibliographyStyle");
        if (!Token(id) || !Token(def.document_class) || def.name.empty() ||
            def.name.size() > 200 || !ids.insert(id).second ||
            (def.frontmatter != "article-v1" &&
             def.frontmatter != "ieee-conference-v1" &&
             def.frontmatter != "elsevier-cas-v1") ||
            (def.bibliography_style != "unsrtnat" &&
             def.bibliography_style != "IEEEtranN"))
            return Unexpected(
                "Invalid profile or unsupported component/citation contract: " +
                id);
        if (!package.files.contains(def.document_class + ".cls"))
            return Unexpected("Template class file is missing");
        if (def.frontmatter == "elsevier-cas-v1" &&
            !package.files.contains("cas-common.sty"))
            return Unexpected("CAS adapter requires cas-common.sty");
        auto options = profile.find("classOptions");
        if (!options || !options->is_array() || options->as_array().size() > 16)
            return Unexpected("Invalid class options");
        for (const auto& value : options->as_array()) {
            if (!Token(value.as_string()))
                return Unexpected("Invalid class option");
            def.class_options.push_back(value.as_string());
        }
        if (auto v = profile.find("twoColumn")) {
            if (!v->is_bool())
                return Unexpected("Invalid twoColumn capability");
            def.two_column = v->as_bool();
        }
        def.id = id + "@" + package.digest.substr(0, 12);
        def.binding = {def.id, package.version, package.digest, id, {}};
        def.name += " (" + package.version + ")";
        def.external = true;
        def.required.affiliations = true;
        def.required.author_affiliations = true;
        def.required.keywords = true;
        def.toolchain.bibliography_engine = BibliographyEngine::BibTex;
        if (def.frontmatter == "elsevier-cas-v1")
            def.packages = {{"amsmath", "", true},
                            {"amssymb", "", true},
                            {"graphicx", "", true},
                            {"booktabs", "", true},
                            {"hyperref", "", true}};
        else
            def.packages = {{"amsmath", ""},
                            {"amssymb", ""},
                            {"graphicx", ""},
                            {"booktabs", ""},
                            {"hyperref", "hidelinks"}};
        for (const auto& [name, content] : package.files) {
            if (BuildResource(name))
                def.resources.emplace(name, content);
        }
        package.profiles.push_back(std::move(def));
    }
    return package;
}

Result<TemplatePackage> LoadBundle(const std::filesystem::path& project,
                                   const std::string& digest) {
    if (!Digest(digest))
        return Unexpected("Invalid template digest");
    const std::string relative = "template-resources/" + digest;
    auto manifest_path =
        ResolveUntrustedProjectPath(project, relative + "/manifest.json");
    if (!manifest_path.ok())
        return Unexpected("Unsafe template resource location");
    auto raw = Read(manifest_path.value(), 256 * 1024);
    if (!raw)
        return Unexpected(raw.error());
    auto manifest = JsonParse(raw.value());
    const auto* inventory = manifest ? manifest->find("files") : nullptr;
    if (!inventory || !inventory->is_object() ||
        inventory->as_object().size() > kCountLimit)
        return Unexpected("Invalid template inventory");
    std::map<std::string, std::string> files;
    std::size_t total = 0;
    for (const auto& [name, hash] : inventory->as_object()) {
        if (!Digest(hash.as_string()))
            return Unexpected("Invalid resource hash");
        auto path =
            ResolveUntrustedProjectPath(project, relative + "/files/" + name);
        if (!ProjectRelativePath::Parse(name).ok() || !path.ok())
            return Unexpected("Unsafe template resource path");
        auto data = Read(path.value(), kFileLimit);
        if (!data || Sha256(data.value()) != hash.as_string())
            return Unexpected("Template resource missing or modified: " + name);
        total += data.value().size();
        if (total > kTotalLimit)
            return Unexpected("Template resource limit exceeded");
        files.emplace(name, std::move(data.value()));
    }
    auto package = Describe(std::move(files));
    if (!package || package.value().digest != digest)
        return Unexpected("Template package identity mismatch");
    return package;
}
} // namespace

JsonValue TemplateBindingToJson(const TemplateBinding& b) {
    JsonObject options;
    for (const auto& [key, val] : b.options)
        options[key] = val;
    return JsonObject{{"id", b.id},
                      {"version", b.version},
                      {"digest", b.digest},
                      {"profile", b.profile},
                      {"options", options}};
}
Result<TemplateBinding> TemplateBindingFromJson(const JsonValue& json) {
    if (!json.is_object())
        return Unexpected("templateBinding must be an object");
    TemplateBinding b;
    auto string = [&](const char* key) {
        const auto* v = json.find(key);
        return v ? v->as_string() : std::string{};
    };
    b.id = string("id");
    b.version = string("version");
    b.digest = string("digest");
    b.profile = string("profile");
    if (b.id.empty() || b.id.size() > 200 || !Token(b.version) ||
        !Token(b.profile) || !Digest(b.digest))
        return Unexpected("Invalid pinned template identity");
    if (const auto* options = json.find("options")) {
        if (!options->is_object() || !options->as_object().empty())
            return Unexpected(
                "This template API has no configurable binding options");
    }
    return b;
}
Result<TemplatePackage>
InspectTemplateArchive(const std::filesystem::path& archive) {
    auto raw = Read(archive, kTotalLimit);
    if (!raw)
        return Unexpected(raw.error());
    auto files = ReadZip(raw.value());
    if (!files)
        return Unexpected(files.error());
    return Describe(std::move(files.value()));
}
Result<void> InstallTemplatePackage(const std::filesystem::path& project,
                                    const TemplatePackage& package) {
    auto recomputed = Describe(package.files);
    if (!recomputed || recomputed.value().digest != package.digest)
        return Unexpected("Template changed after inspection");
    auto target = ResolveUntrustedProjectPath(project, "template-resources/" +
                                                           package.digest);
    if (!target)
        return Unexpected("Unsafe template installation directory");
    std::error_code ec;
    if (std::filesystem::exists(target.value(), ec)) {
        auto existing = LoadBundle(project, package.digest);
        return existing ? Result<void>{}
                        : Result<void>{Unexpected(existing.error())};
    }
    auto pending = ResolveUntrustedProjectPath(
        project, "template-resources/.pending-" + IdGenerator::NewSnapshotId());
    if (!pending)
        return Unexpected("Unsafe temporary template directory");
    std::filesystem::create_directories(pending.value() / "files", ec);
    auto fail = [&](const std::string& error) -> Result<void> {
        std::error_code ignored;
        std::filesystem::remove_all(pending.value(), ignored);
        return Unexpected(error);
    };
    if (ec)
        return fail("Cannot create template directory: " + ec.message());
    for (const auto& [name, bytes] : package.files) {
        const auto path = pending.value() / "files" / name;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
            return fail("Cannot create template resource directory");
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), bytes.size());
        out.close();
        if (!out)
            return fail("Cannot store template resource: " + name);
    }
    std::ofstream out(pending.value() / "manifest.json", std::ios::binary);
    out << JsonValue(JsonObject{{"apiVersion", 1},
                                {"digest", package.digest},
                                {"files", Inventory(package.files)}})
               .Dump();
    out.close();
    if (!out)
        return fail("Cannot store template inventory");
    std::filesystem::rename(pending.value(), target.value(), ec);
    if (ec)
        return fail("Cannot publish template bundle: " + ec.message());
    return {};
}
Result<std::vector<TemplateDefinition>>
LoadProjectTemplates(const std::filesystem::path& project) {
    std::vector<TemplateDefinition> result;
    auto root = ResolveUntrustedProjectPath(project, "template-resources");
    if (!root)
        return Unexpected("Unsafe template catalog path");
    std::error_code ec;
    if (!std::filesystem::exists(root.value(), ec))
        return result;
    std::size_t count = 0, loaded_bytes = 0;
    for (std::filesystem::directory_iterator it(root.value(), ec), end;
         !ec && it != end; it.increment(ec)) {
        const auto digest = it->path().filename().string();
        if (digest.starts_with(".pending-"))
            continue;
        if (++count > 32)
            return Unexpected("Too many imported template bundles");
        auto package = LoadBundle(project, digest);
        if (!package)
            return Unexpected(package.error());
        for (const auto& [name, bytes] : package.value().files)
            loaded_bytes += bytes.size();
        for (auto& def : package.value().profiles) {
            for (const auto& [name, bytes] : def.resources)
                loaded_bytes += bytes.size();
            if (loaded_bytes > kTotalLimit)
                return Unexpected(
                    "Project template catalog exceeds memory limit");
            result.push_back(std::move(def));
        }
    }
    if (ec)
        return Unexpected("Cannot read template catalog: " + ec.message());
    return result;
}
Result<void> VerifyTemplateBinding(const std::filesystem::path& project,
                                   const TemplateBinding& binding) {
    auto package = LoadBundle(project, binding.digest);
    if (!package)
        return Unexpected(package.error());
    for (const auto& def : package.value().profiles)
        if (def.binding == binding)
            return {};
    return Unexpected("Pinned template profile/version is unavailable");
}
} // namespace pf
