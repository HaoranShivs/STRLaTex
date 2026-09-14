#pragma once
// Project serialization: project.paper (JSON) read/write (architecture
// sections 15, 16, 43, 44).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "asset/AssetManager.h"
#include "core/Diagnostic.h"
#include "core/Json.h"
#include "core/Result.h"
#include "document/Document.h"

namespace pf {

struct SerializedProject {
    std::string project_id;
    std::string schema_version = "1";
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
    enum class Status { Ok, IoError, SerializeError };
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
    enum class Status { Ok, FileMissing, ParseError, SchemaError };
    Status status = Status::Ok;
    std::string detail;
    std::optional<SerializedProject> project;
    std::vector<Diagnostic> diagnostics;
};

class ProjectPersistence {
public:
    // Atomic save: serialize -> temp file -> replace.
    static SaveResult Save(const SaveRequest& request);
    static LoadResult Load(const LoadRequest& request);
};

}  // namespace pf
