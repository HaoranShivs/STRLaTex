#include "core/ProjectPath.h"

#include <array>
#include <cstddef>
#include <system_error>

namespace pf {

std::string PathErrorMessage(PathError error) {
    switch (error) {
        case PathError::Empty: return "path is empty";
        case PathError::Absolute: return "path must be relative to the project";
        case PathError::OutsideProjectRoot: return "path escapes the project directory";
        case PathError::InvalidComponent: return "path contains an invalid component";
        case PathError::TooLong: return "path is too long";
        case PathError::NotResolved: return "path cannot be resolved inside the project";
    }
    return "unknown path error";
}

namespace {

constexpr std::size_t kMaxPathBytes = 1024;

bool ContainsNul(std::string_view raw) {
    return raw.find('\0') != std::string_view::npos;
}

// True when the drive/root part of `p` would make it absolute on any
// platform. std::filesystem on POSIX ignores "C:/x" as a drive prefix (it
// parses as a relative path), but such a path must be rejected anyway: the
// file may be written on and opened from Windows.
bool HasRootComponent(const std::filesystem::path& p) {
    if (p.is_absolute())
        return true;
    // root_name: "C:" on Windows, empty on POSIX. root_directory: "/".
    if (!p.root_name().empty() || !p.root_directory().empty())
        return true;
    // POSIX-side detection of Windows spellings: "C:/x", "C:\\x" or
    // "\\server\share" (leading separator).
    const std::string native = p.native();
    if (!native.empty() && (native.front() == '/' || native.front() == '\\'))
        return true;
    if (native.size() >= 2 && native[1] == ':' &&
        ((native[0] >= 'a' && native[0] <= 'z') ||
         (native[0] >= 'A' && native[0] <= 'Z'))) {
        return true;
    }
    return false;
}

// Windows-agnostic component scan: split on both separators so a file written
// on Windows ("assets\\x.png") is judged by the same rules on Linux.
struct RawComponents {
    std::array<std::string, 64> parts;
    std::size_t count = 0;
};

RawComponents SplitRaw(std::string_view raw) {
    RawComponents out;
    std::size_t start = 0;
    while (start <= raw.size() && out.count < out.parts.size()) {
        std::size_t end = raw.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            out.parts[out.count++] = std::string(raw.substr(start));
            break;
        }
        out.parts[out.count++] = std::string(raw.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

}  // namespace

Result<ProjectRelativePath, PathError> ProjectRelativePath::Parse(
    std::string_view raw) {
    if (raw.empty())
        return Unexpected2<PathError>(PathError::Empty);
    if (raw.size() > kMaxPathBytes)
        return Unexpected2<PathError>(PathError::TooLong);
    if (ContainsNul(raw))
        return Unexpected2<PathError>(PathError::InvalidComponent);

    const std::filesystem::path candidate(raw);
    if (HasRootComponent(candidate))
        return Unexpected2<PathError>(PathError::Absolute);

    // Component-level rules, evaluated on both separator variants.
    const RawComponents parts = SplitRaw(raw);
    std::string normalized;
    normalized.reserve(raw.size());
    for (std::size_t i = 0; i < parts.count; ++i) {
        const std::string& part = parts.parts[i];
        // "assets//x" -> empty component; "." -> current directory; ".." ->
        // parent traversal. All three are rejected: a project-relative path
        // names a file inside the tree, not a filesystem navigation.
        if (part.empty())
            return Unexpected2<PathError>(PathError::InvalidComponent);
        if (part == "." || part == "..")
            return Unexpected2<PathError>(PathError::OutsideProjectRoot);
        // Trailing separator already produced an empty component; a lone "."
        // or ".." was handled above. Windows reserved names are not checked
        // here - the renderer never runs on a case-insensitive drive layout
        // that would matter.
        if (!normalized.empty())
            normalized.push_back('/');
        normalized.append(part);
    }
    if (normalized.empty())
        return Unexpected2<PathError>(PathError::Empty);

    return ProjectRelativePath(std::move(normalized),
                               std::filesystem::path(normalized));
}

Result<std::filesystem::path, PathError> ResolveProjectRelativePath(
    const std::filesystem::path& project_root,
    const ProjectRelativePath& relative) {
    std::error_code ec;
    // Weakly canonical: resolve what exists (symlinks included) without
    // requiring the target to exist yet.
    const std::filesystem::path root = std::filesystem::weakly_canonical(project_root, ec);
    if (ec)
        return Unexpected2<PathError>(PathError::NotResolved);

    std::filesystem::path target =
        std::filesystem::weakly_canonical(project_root / relative.path(), ec);
    if (ec)
        return Unexpected2<PathError>(PathError::NotResolved);

    // Containment by component walk, never by string prefix.
    auto root_it = root.begin();
    auto root_end = root.end();
    auto target_it = target.begin();
    // On POSIX both begin with "/"; skip the common root separators so the
    // comparison starts at the first real component.
    while (root_it != root_end && target_it != target.end() &&
           *root_it == *target_it) {
        ++root_it;
        ++target_it;
    }
    // Every root component must have matched: the target lies outside the
    // project root otherwise.
    if (root_it != root_end)
        return Unexpected2<PathError>(PathError::OutsideProjectRoot);

    return target;
}

Result<std::filesystem::path, PathError> ResolveUntrustedProjectPath(
    const std::filesystem::path& project_root, std::string_view raw) {
    auto parsed = ProjectRelativePath::Parse(raw);
    if (!parsed.ok())
        return Unexpected2<PathError>(parsed.error());
    return ResolveProjectRelativePath(project_root, parsed.value());
}

}  // namespace pf
