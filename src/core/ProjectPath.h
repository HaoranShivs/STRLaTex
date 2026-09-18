#pragma once
// P0-04: typed, validated project-relative paths.
//
// A project file stores asset/bibliography locations as relative paths. Those
// bytes are untrusted input: a hand-edited or corrupted .paper file must not
// be able to make the app read, copy or overwrite anything outside the
// project root. This module is the single boundary for such paths:
//
//   * ProjectRelativePath - a string that has passed Parse() and is known to
//     be relative, in-bounds and free of traversal components.
//   * ResolveProjectRelativePath - the only sanctioned root-relative
//     resolution, using component-wise comparison (never string prefixes).
//
// Serialization may only round-trip ProjectRelativePath values; business code
// must not write raw std::string paths back into project files.

#include <filesystem>
#include <string>
#include <string_view>

#include "core/Result.h"

namespace pf {

enum class PathError : std::uint8_t {
    Empty,
    Absolute,
    OutsideProjectRoot,
    InvalidComponent,   // "." / ".." / empty component / NUL
    TooLong,
    NotResolved,        // resolution-time failure (root missing, symlink out)
};

// Human-readable message for UI/log surfaces.
std::string PathErrorMessage(PathError error);

class ProjectRelativePath {
public:
    // Parse an untrusted string into a validated project-relative path.
    // Rejects: empty, absolute paths, drive/UNC prefixes, "." and ".."
    // components, empty components, NUL bytes and overlong paths.
    static Result<ProjectRelativePath, PathError> Parse(std::string_view raw);

    // Normalized, portable form (forward slashes, no redundant separators) as
    // it should be stored in a project file.
    const std::string& value() const noexcept { return value_; }
    // Filesystem path form for joining with a project root.
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    explicit ProjectRelativePath(std::string value, std::filesystem::path path)
        : value_(std::move(value)), path_(std::move(path)) {}

    std::string value_;               // normalized textual form
    std::filesystem::path path_;      // same path in fs form
};

// The only sanctioned root-relative resolution. Both root and target are
// canonicalized with weakly_canonical (the target may not exist yet) and
// containment is compared component-wise, so "..", symlinks and
// look-alike-prefix names ("project2") cannot escape the root.
Result<std::filesystem::path, PathError> ResolveProjectRelativePath(
    const std::filesystem::path& project_root,
    const ProjectRelativePath& relative);

// Convenience: validate + resolve in one step for an untrusted string.
Result<std::filesystem::path, PathError> ResolveUntrustedProjectPath(
    const std::filesystem::path& project_root, std::string_view raw);

}  // namespace pf
