#pragma once
// Project serialization: project.paper (JSON) read/write (architecture
// sections 15, 16, 43, 44).

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
    std::string bibliography_path = "references.bib";  // relative
    ProjectRevision revision;  // typed revision (single source of truth)
};

class ProjectSerializer {
public:
    // Serialize to JSON text.
    static std::string Serialize(const SerializedProject& project);

    // Parse JSON text. Returns error string on failure.
    // The input is fully validated (P0-02): parse limits, schema shape,
    // id/table/path invariants. Any untrusted .paper payload either yields a
    // valid project or a structured error string - never an exception, an
    // abort or a crash.
    static Result<SerializedProject, std::string> Deserialize(const std::string& json_text);
};

struct SaveRequest {
    std::string save_id;
    ProjectId project_id;
    ProjectRevision revision;
    std::filesystem::path destination;  // project.paper path
    SerializedProject snapshot;
};

struct SaveResult {
    // Queued: the immutable snapshot was handed to the save worker and the
    // write has not been observed yet. Completion arrives as an application
    // event (see project/ApplicationEvent.h).
    enum class Status { Ok, Queued, IoError, SerializeError };
    std::string save_id;
    ProjectRevision saved_revision;
    Status status = Status::Ok;
    std::string detail;
    std::optional<Diagnostic> diagnostic;
};

struct LoadRequest {
    std::filesystem::path project_file;  // project.paper path
};

struct LoadResult {
    enum class Status { Ok, FileMissing, ParseError, SchemaError, TooLarge, IoError };
    Status status = Status::Ok;
    std::string detail;
    std::optional<SerializedProject> project;
    std::vector<Diagnostic> diagnostics;
    // Non-empty when the file was written by an older schema and was brought
    // up to date in memory. Persistence never rewrites the file on load.
    MigrationResult migration;
};

class ProjectPersistence {
public:
    // Files above this size are rejected before reading (P0-02): a corrupted
    // or hostile .paper file cannot make the app allocate unbounded memory.
    static constexpr std::uintmax_t kMaxProjectFileBytes = 32 * 1024 * 1024;

    // Atomic save: serialize -> temp file -> replace.
    static SaveResult Save(const SaveRequest& request);
    static LoadResult Load(const LoadRequest& request);
};

}  // namespace pf
