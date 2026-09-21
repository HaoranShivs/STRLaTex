#pragma once
// P0-04：带类型、经校验的项目相对路径。
//
// 项目文件以相对路径存储 asset/bibliography 位置。这些字节属于不可信输入：
// 手工编辑过或损坏的 .paper 文件绝不能使应用读取、复制或覆盖项目根之外的任何
// 内容。本模块是这类路径的唯一边界：
//
//   * ProjectRelativePath——已通过 Parse()、确知为相对、在界内且不含穿越组件
//     的字符串。
//   * ResolveProjectRelativePath——唯一被认可的根相对解析，采用逐组件比较
//     （绝不使用字符串前缀）。
//
// 序列化只允许对 ProjectRelativePath 值做往返；业务代码不得把原始 std::string
// 路径写回项目文件。

#include <filesystem>
#include <string>
#include <string_view>

#include "core/Result.h"

namespace pf {

enum class PathError : std::uint8_t {
    Empty,
    Absolute,
    OutsideProjectRoot,
    InvalidComponent,   // "." / ".." / 空组件 / NUL
    TooLong,
    NotResolved,        // 解析期失败（root 缺失、符号链接指向外部）
};

// 供 UI/日志界面使用的人类可读消息。
std::string PathErrorMessage(PathError error);

class ProjectRelativePath {
public:
    // 将不可信字符串解析为经校验的项目相对路径。
    // 拒绝：空路径、绝对路径、驱动器/UNC 前缀、"." 与 ".." 组件、空组件、
    // NUL 字节以及超长路径。
    static Result<ProjectRelativePath, PathError> Parse(std::string_view raw);

    // 规范化、可移植的形式（正斜杠、无冗余分隔符），项目文件中应存储这种形式。
    const std::string& value() const noexcept { return value_; }
    // 文件系统路径形式，用于与项目根拼接。
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    explicit ProjectRelativePath(std::string value, std::filesystem::path path)
        : value_(std::move(value)), path_(std::move(path)) {}

    std::string value_;               // 规范化文本形式
    std::filesystem::path path_;      // 同一路径的 fs 形式
};

// 唯一被认可的根相对解析。root 与 target 都用 weakly_canonical 规范化（target
// 可能尚不存在），包含关系按组件逐一比较，因此 ".."、符号链接以及前缀相似的
// 名称（"project2"）都无法逃出 root。
Result<std::filesystem::path, PathError> ResolveProjectRelativePath(
    const std::filesystem::path& project_root,
    const ProjectRelativePath& relative);

// 便捷函数：对不可信字符串一步完成校验与解析。
Result<std::filesystem::path, PathError> ResolveUntrustedProjectPath(
    const std::filesystem::path& project_root, std::string_view raw);

}  // namespace pf
