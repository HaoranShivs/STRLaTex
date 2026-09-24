#include "core/ProjectPath.h"

#include <array>
#include <cstddef>
#include <system_error>

namespace pf {

std::string PathErrorMessage(PathError error) {
    switch (error) {
    case PathError::Empty:
        return "path is empty";
    case PathError::Absolute:
        return "path must be relative to the project";
    case PathError::OutsideProjectRoot:
        return "path escapes the project directory";
    case PathError::InvalidComponent:
        return "path contains an invalid component";
    case PathError::TooLong:
        return "path is too long";
    case PathError::NotResolved:
        return "path cannot be resolved inside the project";
    }
    return "unknown path error";
}

namespace {

constexpr std::size_t kMaxPathBytes = 1024;

bool ContainsNul(std::string_view raw) {
    return raw.find('\0') != std::string_view::npos;
}

// 当 `p` 的驱动器/根部分在任何平台上都会使其成为绝对路径时返回 true。POSIX
// 上的 std::filesystem 不把 "C:/x" 当作驱动器前缀（会解析为相对路径），但这类
// 路径仍必须拒绝：文件可能在 Windows 上写入并打开。
bool HasRootComponent(const std::filesystem::path& p) {
    if (p.is_absolute())
        return true;
    // root_name：Windows 上为 "C:"，POSIX 上为空。root_directory："/"。
    if (!p.root_name().empty() || !p.root_directory().empty())
        return true;
    // POSIX 侧对 Windows 写法的识别："C:/x"、"C:\\x" 或
    // "\\server\share"（前导分隔符）。
    const std::string native = p.native();
    if (!native.empty() && (native.front() == '/' || native.front() == '\\'))
        return true;
    if (native.size() >= 2 && native[1] == ':' &&
        ((native[0] >= 'a' && native[0] <= 'z') || (native[0] >= 'A' && native[0] <= 'Z'))) {
        return true;
    }
    return false;
}

// 与 Windows 无关的组件扫描：同时按两种分隔符切分，使在 Windows 上写入的文件
// （"assets\\x.png"）在 Linux 上按同一套规则判定。
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

} // namespace

Result<ProjectRelativePath, PathError> ProjectRelativePath::Parse(std::string_view raw) {
    if (raw.empty())
        return Unexpected2<PathError>(PathError::Empty);
    if (raw.size() > kMaxPathBytes)
        return Unexpected2<PathError>(PathError::TooLong);
    if (ContainsNul(raw))
        return Unexpected2<PathError>(PathError::InvalidComponent);

    const std::filesystem::path candidate(raw);
    if (HasRootComponent(candidate))
        return Unexpected2<PathError>(PathError::Absolute);

    // 组件级规则，对两种分隔符变体都适用。
    const RawComponents parts = SplitRaw(raw);
    std::string normalized;
    normalized.reserve(raw.size());
    for (std::size_t i = 0; i < parts.count; ++i) {
        const std::string& part = parts.parts[i];
        // "assets//x" -> 空组件；"." -> 当前目录；".." -> 向上穿越父目录。三者
        // 一律拒绝：项目相对路径命名的是树内的文件，而不是文件系统导航。
        if (part.empty())
            return Unexpected2<PathError>(PathError::InvalidComponent);
        if (part == "." || part == "..")
            return Unexpected2<PathError>(PathError::OutsideProjectRoot);
        // 末尾分隔符已经产生空组件；单独的 "." 或 ".." 已在上面处理。此处不检查
        // Windows 保留名——renderer 从不在大小写不敏感的驱动器布局上运行，因而
        // 无关紧要。
        if (!normalized.empty())
            normalized.push_back('/');
        normalized.append(part);
    }
    if (normalized.empty())
        return Unexpected2<PathError>(PathError::Empty);

    return ProjectRelativePath(std::move(normalized), std::filesystem::path(normalized));
}

Result<std::filesystem::path, PathError> ResolveProjectRelativePath(const std::filesystem::path& project_root,
                                                                    const ProjectRelativePath& relative) {
    std::error_code ec;
    // 弱规范化：解析已存在的部分（含符号链接），但不要求目标已存在。
    const std::filesystem::path root = std::filesystem::weakly_canonical(project_root, ec);
    if (ec)
        return Unexpected2<PathError>(PathError::NotResolved);

    std::filesystem::path target = std::filesystem::weakly_canonical(project_root / relative.path(), ec);
    if (ec)
        return Unexpected2<PathError>(PathError::NotResolved);

    // 通过逐组件遍历判断包含关系，绝不使用字符串前缀。
    auto root_it = root.begin();
    auto root_end = root.end();
    auto target_it = target.begin();
    // 在 POSIX 上两者都以 "/" 开头；跳过共同的根分隔符，使比较从第一个真实
    // 组件开始。
    while (root_it != root_end && target_it != target.end() && *root_it == *target_it) {
        ++root_it;
        ++target_it;
    }
    // root 的每个组件都必须匹配成功，否则 target 位于项目根之外。
    if (root_it != root_end)
        return Unexpected2<PathError>(PathError::OutsideProjectRoot);

    return target;
}

Result<std::filesystem::path, PathError> ResolveUntrustedProjectPath(const std::filesystem::path& project_root,
                                                                     std::string_view raw) {
    auto parsed = ProjectRelativePath::Parse(raw);
    if (!parsed.ok())
        return Unexpected2<PathError>(parsed.error());
    return ResolveProjectRelativePath(project_root, parsed.value());
}

} // namespace pf
