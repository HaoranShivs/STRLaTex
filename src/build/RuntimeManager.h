#pragma once
// RuntimeManager：定位并校验随附的便携式 TeX Live runtime
// （方案 §17、§18、§21）。
//
// runtime 是 build 环境可复现的关键：绝不查询用户的 PATH、系统 TeX Live
// 与 MiKTeX。若 runtime 不完整，应用必须报告「TeX runtime 不可用」
// （应用/环境错误），而不能把该失败呈现为 LaTeX 文档错误
// （方案 §21、§38）。

#include <filesystem>
#include <string>
#include <vector>

#include "core/Result.h"

namespace pf {

enum class RuntimeStatus {
    Healthy,
    Missing,
    Corrupted,
    UnsupportedVersion,
};

const char* ToString(RuntimeStatus status);

struct RuntimeProblem {
    std::string detail;
};

struct RuntimeInfo {
    int runtime_version = 0;
    std::string texlive_version;
    std::filesystem::path texlive_root;
    RuntimeStatus status = RuntimeStatus::Missing;
    std::vector<std::string> problems;   // 人类可读的诊断信息
};

class RuntimeManager {
public:
    // runtime 在应用内的目录布局（方案 §3）：
    //   runtime/texlive/bin/<platform>/{latexmk,pdflatex,...}
    // `install_root` 是包含 `runtime/` 的目录（源码构建时为仓库根目录，
    // 发布版中为应用目录）。
    explicit RuntimeManager(std::filesystem::path install_root);

    // 定位 runtime，并校验其结构与版本文件。
    RuntimeInfo Initialize();

    std::filesystem::path TexLiveRoot() const { return texlive_root_; }

    // 校验关键可执行文件是否存在且可运行（方案 §18）。
    // `problems` 会为每项发现累积一条人类可读的说明。
    bool VerifyExecutables(const std::filesystem::path& bin_dir,
                           std::vector<std::string>* problems) const;

    // 真正的最小编译测试（方案 §18、§19）：用模板所用的引擎
    // 构建一个小文档，以验证粗体/斜体。
    bool VerifyCompile(const std::filesystem::path& texlive_root,
                       std::vector<std::string>* problems) const;

    RuntimeStatus status() const { return info_.status; }
    const RuntimeInfo& info() const { return info_; }

private:
    std::filesystem::path install_root_;
    std::filesystem::path texlive_root_;
    mutable RuntimeInfo info_;
};

}  // namespace pf
