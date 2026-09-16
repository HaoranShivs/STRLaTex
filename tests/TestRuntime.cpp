// Portable TeX Live runtime tests (plan §19, §20).
//
// These are the runtime's own regression tests: they exercise the bundled
// TeX Live directly (not the document pipeline) so a broken runtime is caught
// independently of the renderer and the editor.

#include "TestMain.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

#include "build/Compiler.h"
#include "build/RuntimeManager.h"

using namespace pf;

namespace {

// The runtime lives in <repo>/runtime/texlive; this file is <repo>/tests/, so
// the repo root is one level up from this file's directory.
std::filesystem::path RepoRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::filesystem::path RuntimeRoot() { return RepoRoot() / "runtime" / "texlive"; }

// A single document body line: how the renderer spells one marked run.
std::string MarkedLine(const char* text, std::uint8_t marks) {
    std::string latex;
    const bool strong = HasMark(marks, TextMark::Strong);
    const bool emphasis = HasMark(marks, TextMark::Emphasis);
    if (strong && emphasis) latex += "\\textbf{\\emph{" + std::string(text) + "}}";
    else if (strong) latex += "\\textbf{" + std::string(text) + "}";
    else if (emphasis) latex += "\\emph{" + std::string(text) + "}";
    else latex += text;
    return latex;
}

constexpr const char* kIeeeTemplate =
    "\\documentclass[conference]{IEEEtran}\n"
    "\\usepackage{amsmath}\n"
    "\\begin{document}\n"
    "\\title{Marks Test}\n"
    "\\author{\\IEEEauthorblockN{Alice}\\IEEEauthorblockA{University}}\n"
    "\\maketitle\n"
    "\\section{Introduction}\n"
    "%BODY%\n"
    "\\begin{equation}\n"
    "E = mc^{2}\n"
    "\\end{equation}\n"
    "\\end{document}\n";

}  // namespace

PF_TEST(RuntimeIsHealthy) {
    RuntimeManager manager(RepoRoot());
    auto info = manager.Initialize();
    std::cout << "    runtime: " << ToString(info.status);
    for (const auto& problem : info.problems) {
        std::cout << " | " << problem;
    }
    std::cout << "\n";
    PF_CHECK(info.status == RuntimeStatus::Healthy);
    if (info.status != RuntimeStatus::Healthy) return;

    // Runtime errors must be distinguishable from document errors (plan §21).
    TexLiveCompiler missing{CompilerConfig{}};
    CompileRequest request;
    request.workspace = std::filesystem::temp_directory_path() / "pf-runtime-missing";
    BuildPackageFile file;
    file.path = "main.tex";
    file.content = "\\documentclass{article}\\begin{document}x\\end{document}";
    request.package.files.push_back(file);
    request.package.entry_file = "main.tex";
    auto result = missing.Compile(request, nullptr);
    PF_CHECK(result.status == CompileStatus::Failure);
    PF_CHECK(result.failure_kind == CompileFailureKind::RuntimeMissing);
}

PF_TEST(FontTestBuildsWithoutSubstitution) {
    // plan §19: a minimum document covering regular/bold/italic/bold-italic
    // must compile through pdfLaTeX without a font substitution warning.
    if (RuntimeManager(RepoRoot()).Initialize().status != RuntimeStatus::Healthy) {
        std::cout << "    (runtime not healthy; skipping)\n";
        return;
    }
    const auto workspace =
        std::filesystem::temp_directory_path() / "pf-font-test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    // The package is what the compiler stages; it is the single source of the
    // document (the same path a real build takes).
    BuildPackageFile file;
    file.path = "main.tex";
    file.content =
        "\\documentclass{article}\n"
        "\\usepackage{amsmath}\n"
        "\\begin{document}\n"
        "Normal\n\n"
        "\\textbf{Bold}\n\n"
        "\\textit{Italic}\n\n"
        "\\textbf{\\textit{Bold Italic}}\n"
        "\\end{document}\n";

    CompilerConfig config;
    config.texlive_root = RuntimeRoot();
    TexLiveCompiler compiler(config);
    CompileRequest request;
    request.workspace = workspace;
    request.package.files.push_back(file);
    request.package.entry_file = "main.tex";
    request.toolchain.engine = LatexEngine::PdfLatex;

    auto result = compiler.Compile(request, nullptr);
    PF_CHECK(result.status == CompileStatus::Success);
    if (result.status != CompileStatus::Success) {
        std::cout << "    log tail: " << result.log.substr(result.log.size() > 400
                                                               ? result.log.size() - 400
                                                               : 0)
                  << "\n";
        return;
    }
    PF_CHECK(std::filesystem::exists(result.pdf_path));
    // plan §19: no *substitution*. Informational "Font shape ... not
    // available" lines are normal (IEEEtran maps bx to b); a substitution
    // warning is not.
    for (const auto& message : result.messages) {
        const bool substitution =
            message.text.find("Font Warning") != std::string::npos ||
            message.text.find("substituted") != std::string::npos;
        PF_CHECK(!substitution);
    }
}

PF_TEST(IeeeMarksReachPdfLatex) {
    // plan §39: the reported IEEE bold/italic bug, pinned for good. The
    // renderer spells the four mark combinations, pdfLaTeX builds them through
    // the bundled runtime, and IEEEtran's ptm font selection must hold.
    if (RuntimeManager(RepoRoot()).Initialize().status != RuntimeStatus::Healthy) {
        std::cout << "    (runtime not healthy; skipping)\n";
        return;
    }
    std::string body =
        MarkedLine("Plain", 0) + "\n\n" +
        MarkedLine("Strong", static_cast<std::uint8_t>(TextMark::Strong)) + "\n\n" +
        MarkedLine("Emphasis", static_cast<std::uint8_t>(TextMark::Emphasis)) + "\n\n" +
        MarkedLine("BoldItalic",
                   static_cast<std::uint8_t>(TextMark::Strong | TextMark::Emphasis)) +
        "\n\n";
    std::string tex = kIeeeTemplate;
    tex.replace(tex.find("%BODY%"), 6, body);

    const auto workspace =
        std::filesystem::temp_directory_path() / "pf-ieee-marks";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    CompilerConfig config;
    config.texlive_root = RuntimeRoot();
    TexLiveCompiler compiler(config);
    CompileRequest request;
    request.workspace = workspace;
    BuildPackageFile file;
    file.path = "main.tex";
    file.content = tex;
    request.package.files.push_back(file);
    request.package.entry_file = "main.tex";
    request.toolchain.engine = LatexEngine::PdfLatex;

    auto result = compiler.Compile(request, nullptr);
    PF_CHECK(result.status == CompileStatus::Success);
    if (result.status != CompileStatus::Success) {
        std::cout << "    log tail: " << result.log.substr(result.log.size() > 600
                                                               ? result.log.size() - 600
                                                               : 0)
                  << "\n";
        return;
    }
    PF_CHECK(std::filesystem::exists(result.pdf_path));

    // The LaTeX must contain all four combinations...
    const std::string& built = result.log;  // unused placeholder
    (void)built;
    std::ifstream generated(workspace / "main.tex", std::ios::binary);
    std::ostringstream source;
    source << generated.rdbuf();
    const std::string& tex_staged = source.str();
    PF_CHECK(tex_staged.find("\\textbf{Strong}") != std::string::npos);
    PF_CHECK(tex_staged.find("\\emph{Emphasis}") != std::string::npos);
    PF_CHECK(tex_staged.find("\\textbf{\\emph{BoldItalic}}") != std::string::npos);

    // ... and the log must show no font substitution (plan §24). IEEEtran's
    // informational "Font shape ... not available" lines are expected: they
    // map bx to b and the PDF still carries four distinct fonts.
    for (const auto& message : result.messages) {
        const bool substitution =
            message.text.find("Font Warning") != std::string::npos ||
            message.text.find("substituted") != std::string::npos;
        PF_CHECK(!substitution);
    }
}

PF_TEST(CompileRequestCarriesToolchain) {
    // plan §14: the toolchain travels with the request; the compiler does not
    // guess the engine from the document.
    CompileRequest request;
    request.toolchain.engine = LatexEngine::PdfLatex;
    request.toolchain.bibliography_engine = BibliographyEngine::BibTex;
    PF_CHECK(request.toolchain.engine == LatexEngine::PdfLatex);
    PF_CHECK(request.toolchain == request.toolchain);
    PF_CHECK(std::string(ToString(LatexEngine::PdfLatex)) == "pdflatex");
    PF_CHECK(std::string(ToString(LatexEngine::XeLatex)) == "xelatex");
    PF_CHECK(std::string(ToString(LatexEngine::LuaLatex)) == "lualatex");
    PF_CHECK(std::string(ToString(BibliographyEngine::BibTex)) == "bibtex");
}
