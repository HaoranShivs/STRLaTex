#include "build/RuntimeManager.h"

#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

#include "build/Compiler.h"

namespace pf {

namespace {

// The executables the production build path needs (plan §18). latexmk drives
// the others, but they must all be present for the runtime to be usable.
const char* const kRequiredExecutables[] = {
    "latexmk", "pdflatex", "xelatex", "lualatex", "bibtex", "kpsewhich",
};

// A minimum document that exercises the exact feature the templates depend on:
// bold, italic and bold-italic must survive a real compile (plan §19).
const char* const kFontTestTex =
    "\\documentclass{article}\n"
    "\\usepackage{amsmath}\n"
    "\\begin{document}\n"
    "Normal\n\n"
    "\\textbf{Bold}\n\n"
    "\\textit{Italic}\n\n"
    "\\textbf{\\textit{Bold Italic}}\n"
    "\\end{document}\n";

}  // namespace

const char* ToString(RuntimeStatus status) {
    switch (status) {
        case RuntimeStatus::Healthy: return "Healthy";
        case RuntimeStatus::Missing: return "Missing";
        case RuntimeStatus::Corrupted: return "Corrupted";
        case RuntimeStatus::UnsupportedVersion: return "UnsupportedVersion";
    }
    return "?";
}

RuntimeManager::RuntimeManager(std::filesystem::path install_root)
    : install_root_(std::move(install_root)) {}

RuntimeInfo RuntimeManager::Initialize() {
    info_ = RuntimeInfo{};
    std::error_code ec;

    texlive_root_ = install_root_ / "runtime" / "texlive";
    if (!std::filesystem::exists(texlive_root_, ec)) {
        info_.status = RuntimeStatus::Missing;
        info_.problems.push_back("runtime directory not found: " +
                                 texlive_root_.string());
        return info_;
    }
    info_.texlive_root = texlive_root_;

    // The bin dir is platform-specific inside the runtime (plan §3).
    std::filesystem::path bin_dir;
    for (const auto& entry : std::filesystem::directory_iterator(texlive_root_ / "bin", ec)) {
        if (entry.is_directory()) {
            bin_dir = entry.path();
            break;
        }
    }
    if (ec || bin_dir.empty()) {
        info_.status = RuntimeStatus::Corrupted;
        info_.problems.push_back("runtime has no bin/<platform> directory");
        return info_;
    }

    if (!VerifyExecutables(bin_dir, &info_.problems)) {
        info_.status = RuntimeStatus::Corrupted;
        return info_;
    }

    if (!VerifyCompile(texlive_root_, &info_.problems)) {
        info_.status = RuntimeStatus::Corrupted;
        return info_;
    }

    info_.runtime_version = 1;
    info_.texlive_version = "2026";
    info_.status = RuntimeStatus::Healthy;
    return info_;
}

bool RuntimeManager::VerifyExecutables(const std::filesystem::path& bin_dir,
                                       std::vector<std::string>* problems) const {
    bool ok = true;
    std::error_code ec;
    for (const char* name : kRequiredExecutables) {
        if (!std::filesystem::exists(bin_dir / name, ec)) {
            problems->push_back(std::string("missing executable: ") + name);
            ok = false;
        }
    }
    return ok;
}

bool RuntimeManager::VerifyCompile(
    const std::filesystem::path& texlive_root,
    std::vector<std::string>* problems) const {
    std::error_code ec;
    std::filesystem::path bin_dir;
    for (const auto& entry :
         std::filesystem::directory_iterator(texlive_root / "bin", ec)) {
        if (entry.is_directory()) {
            bin_dir = entry.path();
            break;
        }
    }
    if (bin_dir.empty()) {
        problems->push_back("runtime has no bin/<platform> directory");
        return false;
    }

    // A throwaway workspace under the runtime's own var dir keeps the health
    // check self-contained and writable without touching the user's HOME.
    const auto work = texlive_root / "texmf-var" / "health";
    std::filesystem::create_directories(work, ec);

    CompileRequest request;
    request.workspace = work;
    BuildPackageFile main_file;
    main_file.path = "main.tex";
    main_file.content = kFontTestTex;
    request.package.files.push_back(main_file);
    request.package.entry_file = "main.tex";
    request.toolchain.engine = LatexEngine::PdfLatex;
    request.toolchain.bibliography_engine = BibliographyEngine::None;

    CompilerConfig config;
    config.engine = LatexEngine::PdfLatex;
    config.texlive_root = texlive_root;
    TexLiveCompiler compiler(config);
    auto result = compiler.Compile(request, nullptr);

    if (result.status != CompileStatus::Success) {
        problems->push_back("runtime health-check compile failed");
        for (const auto& message : result.messages) {
            problems->push_back("  " + message.text);
        }
        return false;
    }
    std::filesystem::remove_all(work, ec);
    return true;
}

}  // namespace pf
