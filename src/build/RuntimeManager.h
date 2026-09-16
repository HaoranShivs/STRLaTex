#pragma once
// RuntimeManager: locates and verifies the bundled portable TeX Live runtime
// (plan §17, §18, §21).
//
// The runtime is what makes the build environment reproducible: the user's
// PATH, system TeX Live and MiKTeX are never consulted. If the runtime is
// incomplete, the application must say "the TeX runtime is unavailable"
// (an application/environment error) instead of presenting the failure as a
// LaTeX document error (plan §21, §38).

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
    std::vector<std::string> problems;   // human-readable diagnostics
};

class RuntimeManager {
public:
    // Runtime layout inside the application (plan §3):
    //   runtime/texlive/bin/<platform>/{latexmk,pdflatex,...}
    // `install_root` is the directory that contains `runtime/` (the
    // repository root for a source build, the app directory for a release).
    explicit RuntimeManager(std::filesystem::path install_root);

    // Locate the runtime and validate its structure + version file.
    RuntimeInfo Initialize();

    std::filesystem::path TexLiveRoot() const { return texlive_root_; }

    // Verify that the key executables exist and are runnable (plan §18).
    // `problems` accumulates a human-readable explanation per finding.
    bool VerifyExecutables(const std::filesystem::path& bin_dir,
                           std::vector<std::string>* problems) const;

    // The real minimum compile test (plan §18, §19): build a tiny document
    // that exercises bold/italic through the engine the templates use.
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
