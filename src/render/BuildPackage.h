#pragma once
#include "build/Toolchain.h"
#include <string>
#include <vector>

namespace pf {
struct BuildPackageFile {
    std::string path;
    std::string content;
    bool executable = false;
};
struct BuildPackage {
    std::string entry_file = "main.tex";
    std::vector<BuildPackageFile> files;
    std::vector<std::string> required_assets;
    std::vector<std::string> bibliography_files;
    std::vector<std::string> template_files;
    BuildToolchain toolchain;
    bool requires_isolation = false;
    std::string plan_digest;
};
} // namespace pf
