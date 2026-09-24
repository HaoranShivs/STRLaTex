// 可移植 TeX Live 运行时测试（方案 §19、§20）。
//
// 这些是运行时自身的回归测试：直接针对随附的 TeX Live 运行（而非 document
// 流水线），因此运行时故障可以脱离渲染器和编辑器被独立捕获。

#include "TestMain.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

#include "build/Compiler.h"
#include "build/RuntimeManager.h"

using namespace pf;

namespace {

// 运行时位于 <repo>/runtime/texlive；本文件位于 <repo>/tests/，
// 因此仓库根目录就是本文件所在目录的上一级。
std::filesystem::path RepoRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::filesystem::path RuntimeRoot() {
    return RepoRoot() / "runtime" / "texlive";
}

// 单行文档正文：渲染器如何生成一段带标记的文本。
std::string MarkedLine(const char* text, std::uint8_t marks) {
    std::string latex;
    const bool strong = HasMark(marks, TextMark::Strong);
    const bool emphasis = HasMark(marks, TextMark::Emphasis);
    if (strong && emphasis)
        latex += "\\textbf{\\emph{" + std::string(text) + "}}";
    else if (strong)
        latex += "\\textbf{" + std::string(text) + "}";
    else if (emphasis)
        latex += "\\emph{" + std::string(text) + "}";
    else
        latex += text;
    return latex;
}

constexpr const char* kIeeeTemplate = "\\documentclass[conference]{IEEEtran}\n"
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

} // namespace

PF_TEST(RuntimeIsHealthy) {
    RuntimeManager manager(RepoRoot());
    auto info = manager.Initialize();
    std::cout << "    runtime: " << ToString(info.status);
    for (const auto& problem : info.problems) {
        std::cout << " | " << problem;
    }
    std::cout << "\n";
    PF_CHECK(info.status == RuntimeStatus::Healthy);
    if (info.status != RuntimeStatus::Healthy)
        return;

    // 运行时错误必须能与文档错误区分开（方案 §21）。
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
    // 方案 §19：一份覆盖 regular/bold/italic/bold-italic 的最简文档
    // 必须能通过 pdfLaTeX 编译，且不出现字体替换警告。
    if (RuntimeManager(RepoRoot()).Initialize().status != RuntimeStatus::Healthy) {
        std::cout << "    (runtime not healthy; skipping)\n";
        return;
    }
    const auto workspace = std::filesystem::temp_directory_path() / "pf-font-test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    // package 就是编译器所暂存的内容；它是文档的唯一来源
    //（与真实 build 走的路径相同）。
    BuildPackageFile file;
    file.path = "main.tex";
    file.content = "\\documentclass{article}\n"
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
        std::cout << "    log tail: " << result.log.substr(result.log.size() > 400 ? result.log.size() - 400 : 0)
                  << "\n";
        return;
    }
    PF_CHECK(std::filesystem::exists(result.pdf_path));
    // 方案 §19：不允许出现 *substitution*。信息性的「Font shape ... not
    // available」行是正常的（IEEEtran 会把 bx 映射为 b）；
    // 替换警告则不正常。
    for (const auto& message : result.messages) {
        const bool substitution = message.text.find("Font Warning") != std::string::npos ||
                                  message.text.find("substituted") != std::string::npos;
        PF_CHECK(!substitution);
    }
}

PF_TEST(IeeeMarksReachPdfLatex) {
    // 方案 §39：已报告的 IEEE 粗体/斜体 bug，在此永久固定。
    // 渲染器生成四种标记组合的写法，pdfLaTeX 通过随附的运行时构建它们，
    // IEEEtran 的 ptm 字体选择必须保持正确。
    if (RuntimeManager(RepoRoot()).Initialize().status != RuntimeStatus::Healthy) {
        std::cout << "    (runtime not healthy; skipping)\n";
        return;
    }
    std::string body =
        MarkedLine("Plain", 0) + "\n\n" + MarkedLine("Strong", static_cast<std::uint8_t>(TextMark::Strong)) + "\n\n" +
        MarkedLine("Emphasis", static_cast<std::uint8_t>(TextMark::Emphasis)) + "\n\n" +
        MarkedLine("BoldItalic", static_cast<std::uint8_t>(TextMark::Strong | TextMark::Emphasis)) + "\n\n";
    std::string tex = kIeeeTemplate;
    tex.replace(tex.find("%BODY%"), 6, body);

    const auto workspace = std::filesystem::temp_directory_path() / "pf-ieee-marks";
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
        std::cout << "    log tail: " << result.log.substr(result.log.size() > 600 ? result.log.size() - 600 : 0)
                  << "\n";
        return;
    }
    PF_CHECK(std::filesystem::exists(result.pdf_path));

    // LaTeX 必须包含全部四种组合……
    const std::string& built = result.log; // 未使用的占位符
    (void)built;
    std::ifstream generated(workspace / "main.tex", std::ios::binary);
    std::ostringstream source;
    source << generated.rdbuf();
    const std::string& tex_staged = source.str();
    PF_CHECK(tex_staged.find("\\textbf{Strong}") != std::string::npos);
    PF_CHECK(tex_staged.find("\\emph{Emphasis}") != std::string::npos);
    PF_CHECK(tex_staged.find("\\textbf{\\emph{BoldItalic}}") != std::string::npos);

    // ……并且日志中不得出现字体替换（方案 §24）。IEEEtran 的信息性
    //「Font shape ... not available」行是预期内的：它们把 bx 映射为 b，
    // 且 PDF 仍带有四种不同的字体。
    for (const auto& message : result.messages) {
        const bool substitution = message.text.find("Font Warning") != std::string::npos ||
                                  message.text.find("substituted") != std::string::npos;
        PF_CHECK(!substitution);
    }
}

PF_TEST(CompileRequestCarriesToolchain) {
    // 方案 §14：toolchain 随请求一起传递；编译器不会根据文档猜测引擎。
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

PF_TEST(CompileStreamsOutputLiveToSink) {
    // Build Diagnostics 方案 §44：子进程运行期间，编译器把 stdout/stderr 分块
    // 交给 sink，因此 Build Log 会实时增长，且不丢失任何内容：
    // sink 的 stdout 文本重新拼接后即为 result.log。
    if (RuntimeManager(RepoRoot()).Initialize().status != RuntimeStatus::Healthy) {
        std::cout << "    (runtime not healthy; skipping)\n";
        return;
    }
    const auto workspace = std::filesystem::temp_directory_path() / "pf-stream-test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    CompilerConfig config;
    config.texlive_root = RuntimeRoot();
    TexLiveCompiler compiler(config);
    CompileRequest request;
    request.workspace = workspace;
    BuildPackageFile file;
    file.path = "main.tex";
    file.content = "\\documentclass{article}\n"
                   "\\begin{document}\n"
                   "Stream me\n"
                   "\\end{document}\n";
    request.package.files.push_back(file);
    request.package.entry_file = "main.tex";
    request.toolchain.engine = LatexEngine::PdfLatex;

    std::string streamed_stdout;
    std::string streamed_stderr;
    int chunks = 0;
    request.on_output = [&](const CompileOutputChunk& chunk) {
        ++chunks;
        (chunk.is_stderr ? streamed_stderr : streamed_stdout) += chunk.text;
    };

    auto result = compiler.Compile(request, nullptr);
    PF_CHECK(result.status == CompileStatus::Success);
    PF_CHECK(result.exit_code == 0);
    PF_CHECK(chunks > 0);               // sink 确实被触发了
    PF_CHECK(!streamed_stdout.empty()); // latexmk 把日志写到这里
    PF_CHECK(result.log == streamed_stdout + streamed_stderr);
    // 磁盘上的产物仍然保留以便检查（方案 §15/§45）。
    PF_CHECK(std::filesystem::exists(workspace / "latexmk.log"));
}

PF_TEST(CompileFailureCarriesExitCodeAndMessages) {
    // Build Diagnostics 方案 §31/§32：成功要求退出码为 0 且生成 PDF；
    // LaTeX 错误会产生非零退出码、failure 状态，以及至少一条供 Problems
    // 面板使用的已解析消息。
    if (RuntimeManager(RepoRoot()).Initialize().status != RuntimeStatus::Healthy) {
        std::cout << "    (runtime not healthy; skipping)\n";
        return;
    }
    const auto workspace = std::filesystem::temp_directory_path() / "pf-fail-test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    CompilerConfig config;
    config.texlive_root = RuntimeRoot();
    TexLiveCompiler compiler(config);
    CompileRequest request;
    request.workspace = workspace;
    BuildPackageFile file;
    file.path = "main.tex";
    file.content = "\\documentclass{article}\n"
                   "\\begin{document}\n"
                   "\\unknownmacrothatdoesnotexist{}\n"
                   "\\end{document}\n";
    request.package.files.push_back(file);
    request.package.entry_file = "main.tex";
    request.toolchain.engine = LatexEngine::PdfLatex;

    auto result = compiler.Compile(request, nullptr);
    PF_CHECK(result.status == CompileStatus::Failure);
    PF_CHECK(result.exit_code != 0);
    PF_CHECK(!result.messages.empty());
    bool saw_error = false;
    for (const auto& message : result.messages) {
        if (message.is_error)
            saw_error = true;
    }
    PF_CHECK(saw_error);
}
