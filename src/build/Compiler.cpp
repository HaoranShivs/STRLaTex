#include "build/Compiler.h"

#include <cstdlib>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pf {
namespace {

std::string QuoteShell(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

bool StagePackage(const CompileRequest& request, std::string* error) {
    std::error_code ec;
    // Each build gets its own workspace (plan §25), and a retried build must
    // not inherit the previous attempt's latexmk state: latexmk otherwise
    // reports "gave an error in previous invocation" and refuses to run, even
    // though the freshly staged sources are fine.
    std::filesystem::remove_all(request.workspace, ec);
    std::filesystem::create_directories(request.workspace, ec);
    if (ec) {
        *error = "cannot create build workspace: " + ec.message();
        return false;
    }
    for (const auto& file : request.package.files) {
        const auto destination = request.workspace / file.path;
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            *error = "cannot write generated file: " + destination.string();
            return false;
        }
        output << file.content;
    }
    for (const auto& [destination_name, source] : request.asset_sources) {
        const auto destination = request.workspace / destination_name;
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            *error = "cannot stage asset " + source.string() + ": " +
                     ec.message();
            return false;
        }
    }
    return true;
}

// ---- Diagnostic parsing (plan §16) ----
//
// pdfLaTeX logs are free text, so recognition is by pattern. Anything that
// fails to parse stays literal in the message text: a document never loses a
// diagnostic because the parser could not classify it.

// A "file:line:" style compiler reference, e.g. "main.tex:13: <message>".
bool ParseFileLine(const std::string& line, CompilerMessage* message) {
    const size_t first = line.find(':');
    if (first == std::string::npos) return false;
    const size_t second = line.find(':', first + 1);
    if (second == std::string::npos) return false;
    try {
        const std::uint32_t number = static_cast<std::uint32_t>(
            std::stoul(line.substr(first + 1, second - first - 1)));
        message->file = line.substr(0, first);
        message->line = number;
        message->text = line.substr(second + 1);
        return true;
    } catch (...) {
        return false;
    }
}

// The primary error marker of TeX logs: "!" followed by the message. The
// source line is found in an accompanying "l.<number>" line or the last
// file:line reference, both of which follow it.
std::vector<CompilerMessage> ParseMessages(const std::string& log,
                                           CompileFailureKind* failure_kind) {
    std::vector<CompilerMessage> messages;
    CompileFailureKind kind = CompileFailureKind::None;
    auto note_kind = [&](CompileFailureKind candidate) {
        if (kind == CompileFailureKind::None ||
            kind == CompileFailureKind::LatexError) {
            kind = candidate;
        }
    };

    std::istringstream lines(log);
    std::string line;
    std::uint32_t last_source_line = 1;
    std::string last_file = "main.tex";
    bool saw_font_warning = false;
    bool saw_missing_package = false;
    bool saw_bibliography = false;
    while (std::getline(lines, line)) {
        // Track the last "file:line:" reference so a "!" block can be
        // attributed to a source location.
        {
            CompilerMessage reference;
            if (ParseFileLine(line, &reference)) {
                last_file = reference.file;
                last_source_line = reference.line;
            }
        }

        if (!line.empty() && line[0] == '!') {
            CompilerMessage message;
            message.file = last_file;
            message.line = last_source_line;
            message.is_error = true;
            message.text = line.substr(line.size() > 1 ? 1 : 0);
            messages.push_back(std::move(message));
            note_kind(CompileFailureKind::LatexError);
            continue;
        }
        if (line.rfind("l.", 0) == 0) {
            // "l.13 The offending text" carries the line number of the
            // preceding error.
            try {
                const size_t space = line.find(' ');
                const std::uint32_t number = static_cast<std::uint32_t>(
                    std::stoul(line.substr(2, space - 2)));
                if (!messages.empty() && messages.back().line == 1) {
                    messages.back().line = number;
                }
            } catch (...) {
            }
            continue;
        }

        CompilerMessage message;
        const bool file_line = ParseFileLine(line, &message);
        const bool warning = line.find("Warning") != std::string::npos;
        const bool overfull = line.rfind("Overfull \\hbox", 0) == 0 ||
                              line.rfind("Underfull \\hbox", 0) == 0;
        if (!file_line && !warning && !overfull && line.find("!") == std::string::npos) {
            continue;
        }

        message.is_error = !warning;
        if (message.file.empty()) message.file = last_file;
        if (message.line == 0) message.line = last_source_line;
        if (message.text.empty()) message.text = line;
        messages.push_back(message);

        if (line.find("Font Warning") != std::string::npos ||
            line.find("font substitution") != std::string::npos ||
            line.find("substituted") != std::string::npos) {
            saw_font_warning = true;
        }
        if (line.find("File `") != std::string::npos &&
            line.find("' not found") != std::string::npos) {
            saw_missing_package = true;
        }
        if (line.find("Citation") != std::string::npos ||
            line.find("Bibliography") != std::string::npos) {
            saw_bibliography = true;
        }
    }
    if (saw_missing_package) note_kind(CompileFailureKind::PackageMissing);
    if (saw_font_warning) note_kind(CompileFailureKind::FontMissing);
    if (saw_bibliography) note_kind(CompileFailureKind::BibliographyError);
    if (failure_kind) *failure_kind = kind;
    return messages;
}

// Run `command`, wait up to the deadline, and kill the whole process group on
// cancel or timeout (plan §36): latexmk spawns pdflatex/bibtex children, so
// killing only the parent would leave writers behind on the workspace.
int RunCommand(const std::string& command,
               const std::chrono::seconds timeout,
               const std::atomic<bool>* cancel_requested,
               bool* cancelled) {
#ifdef _WIN32
    (void)cancel_requested;
    (void)timeout;
    return std::system(command.c_str());
#else
    const pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);  // own process group, so a kill reaches the children
        execl("/bin/sh", "sh", "-c", command.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) return -1;

    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (waitpid(child, &status, WNOHANG) == 0) {
        if ((cancel_requested && cancel_requested->load()) ||
            std::chrono::steady_clock::now() >= deadline) {
            *cancelled = true;
            // SIGTERM to the whole group first, then SIGKILL if it survives.
            kill(-child, SIGTERM);
            for (int grace = 0; grace < 20; ++grace) {
                if (waitpid(child, &status, WNOHANG) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            kill(-child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace

const char* ToString(LatexEngine engine) {
    switch (engine) {
        case LatexEngine::PdfLatex: return "pdflatex";
        case LatexEngine::XeLatex: return "xelatex";
        case LatexEngine::LuaLatex: return "lualatex";
    }
    return "?";
}

const char* ToString(BibliographyEngine engine) {
    switch (engine) {
        case BibliographyEngine::None: return "none";
        case BibliographyEngine::BibTex: return "bibtex";
        case BibliographyEngine::Biber: return "biber";
    }
    return "?";
}

const char* ToString(CompileFailureKind kind) {
    switch (kind) {
        case CompileFailureKind::None: return "None";
        case CompileFailureKind::RuntimeMissing: return "RuntimeMissing";
        case CompileFailureKind::RuntimeCorrupted: return "RuntimeCorrupted";
        case CompileFailureKind::PackageMissing: return "PackageMissing";
        case CompileFailureKind::FontMissing: return "FontMissing";
        case CompileFailureKind::LatexError: return "LatexError";
        case CompileFailureKind::BibliographyError: return "BibliographyError";
        case CompileFailureKind::Timeout: return "Timeout";
        case CompileFailureKind::Cancelled: return "Cancelled";
        case CompileFailureKind::InternalError: return "InternalError";
    }
    return "?";
}

// ---- TectonicCompiler (kept, but no longer the production path, §26) ----

TectonicCompiler::TectonicCompiler(std::string executable,
                                 std::string cache_dir)
    : executable_(std::move(executable)), cache_dir_(std::move(cache_dir)) {
    std::error_code ec;
    if (!executable_.empty() &&
        std::filesystem::exists(executable_, ec)) {
        executable_ = std::filesystem::absolute(executable_, ec).string();
    }
    (void)cache_dir_;  // bundle cache resolution stayed as before; see below
    cache_dir_ = std::move(cache_dir);
    std::error_code ec2;
    if (!cache_dir_.empty() && std::filesystem::exists(cache_dir_, ec2)) {
        cache_dir_ = std::filesystem::absolute(cache_dir_, ec2).string();
    } else {
        cache_dir_.clear();
    }
}

CompileResult TectonicCompiler::Compile(
    const CompileRequest& request,
    const std::atomic<bool>* cancel_requested) {
    CompileResult result;
    if (cancel_requested && cancel_requested->load()) {
        result.status = CompileStatus::Cancelled;
        result.failure_kind = CompileFailureKind::Cancelled;
        return result;
    }
    if (executable_.empty()) {
        result.log = "tectonic executable was not configured";
        result.failure_kind = CompileFailureKind::RuntimeMissing;
        result.messages.push_back(
            {"main.tex", 1, true, result.log});
        return result;
    }
    std::string stage_error;
    if (!StagePackage(request, &stage_error)) {
        result.log = stage_error;
        result.failure_kind = CompileFailureKind::InternalError;
        result.messages.push_back({"main.tex", 1, true, stage_error});
        return result;
    }

    const auto log_path = request.workspace / "build.log";
    // Pin the bundle cache when one was configured for this build so the
    // build stays offline-reproducible.
    std::string env_prefix;
    if (!cache_dir_.empty()) {
        env_prefix = "TECTONIC_CACHE_DIR=" + QuoteShell(cache_dir_) +
                     " HOME=" + QuoteShell(cache_dir_) + " ";
    }
    const std::string command =
        "cd " + QuoteShell(request.workspace.string()) + " && " +
        env_prefix + QuoteShell(executable_) +
        " -X compile --keep-logs --outdir " +
        QuoteShell(request.workspace.string()) + " " +
        QuoteShell(request.package.entry_file) + " >" +
        QuoteShell(log_path.string()) + " 2>&1";
    bool cancelled = false;
    const int exit_code =
        RunCommand(command, std::chrono::minutes{2}, cancel_requested,
                   &cancelled);

    std::ifstream log_file(log_path, std::ios::binary);
    std::ostringstream log;
    log << log_file.rdbuf();
    result.log = log.str();
    CompileFailureKind kind = CompileFailureKind::None;
    result.messages = ParseMessages(result.log, &kind);
    result.auxiliary_logs.push_back(log_path);
    result.pdf_path =
        request.workspace /
        (std::filesystem::path(request.package.entry_file).stem().string() +
         ".pdf");
    if (cancelled || (cancel_requested && cancel_requested->load())) {
        result.status = CompileStatus::Cancelled;
        result.failure_kind = CompileFailureKind::Cancelled;
    } else if (exit_code == 0 && std::filesystem::exists(result.pdf_path)) {
        result.status = CompileStatus::Success;
    } else {
        result.status = CompileStatus::Failure;
        result.failure_kind =
            kind == CompileFailureKind::None ? CompileFailureKind::LatexError
                                             : kind;
        if (result.messages.empty()) {
            result.messages.push_back(
                {"main.tex", 1, true, "Tectonic compilation failed"});
        }
    }
    return result;
}

// ---- TexLiveCompiler (plan §10, §30) ----

TexLiveCompiler::TexLiveCompiler(CompilerConfig config)
    : config_(std::move(config)) {}

CompileResult TexLiveCompiler::Compile(
    const CompileRequest& request,
    const std::atomic<bool>* cancel_requested) {
    CompileResult result;
    if (cancel_requested && cancel_requested->load()) {
        result.status = CompileStatus::Cancelled;
        result.failure_kind = CompileFailureKind::Cancelled;
        return result;
    }

    // The bundled runtime is the only supported TeX environment (plan §3, §12):
    // the user's PATH, system TeX Live and MiKTeX are deliberately not
    // consulted. The executables live in bin/<platform>/.
    std::error_code ec;
    std::filesystem::path bin_dir;
    if (!config_.texlive_root.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(
                 config_.texlive_root / "bin", ec)) {
            if (entry.is_directory()) {
                bin_dir = entry.path();
                break;
            }
        }
    }
    if (ec || bin_dir.empty() ||
        !std::filesystem::exists(bin_dir / "latexmk", ec)) {
        result.log = "STRLaTex TeX runtime is incomplete or corrupted: " +
                     (config_.texlive_root / "bin").string() +
                     "/<platform>/latexmk is missing";
        result.failure_kind = CompileFailureKind::RuntimeMissing;
        result.messages.push_back({"runtime", 0, true, result.log});
        return result;
    }
    const std::string bin_dir_str = bin_dir.string();
    const std::string latexmk = (bin_dir / "latexmk").string();

    std::string stage_error;
    if (!StagePackage(request, &stage_error)) {
        result.log = stage_error;
        result.failure_kind = CompileFailureKind::InternalError;
        result.messages.push_back({"main.tex", 1, true, stage_error});
        return result;
    }

    // latexmk drives the whole chain (plan §11): the engine switch selects the
    // compiler, the remaining flags are the same for every engine.
    const char* engine_flag = "-pdf";
    switch (request.toolchain.engine) {
        case LatexEngine::PdfLatex: engine_flag = "-pdf"; break;
        case LatexEngine::XeLatex: engine_flag = "-xelatex"; break;
        case LatexEngine::LuaLatex: engine_flag = "-lualatex"; break;
    }

    const auto log_path = request.workspace / "latexmk.log";
    // Environment isolation (plan §12): only the bundled bin dir is on PATH,
    // TeX caches live under the runtime, and the ambient shell environment is
    // not allowed to influence the result.
    const std::string command =
        "cd " + QuoteShell(request.workspace.string()) + " && " +
        "PATH=" + QuoteShell(bin_dir_str + ":/usr/bin:/bin") + " " +
        "HOME=" + QuoteShell(config_.texlive_root.string()) + " " +
        "TEXMFHOME=" + QuoteShell((config_.texlive_root / "texmf-home").string()) + " " +
        "TEXMFVAR=" + QuoteShell((config_.texlive_root / "texmf-var").string()) + " " +
        "TEXMFCACHE=" + QuoteShell((config_.texlive_root / "texmf-cache").string()) + " " +
        QuoteShell(latexmk) + " " + engine_flag +
        " -interaction=nonstopmode -file-line-error -halt-on-error " +
        QuoteShell(request.package.entry_file) + " >" +
        QuoteShell(log_path.string()) + " 2>&1";
    bool cancelled = false;
    const int exit_code =
        RunCommand(command, std::chrono::seconds{120}, cancel_requested,
                   &cancelled);

    std::ifstream log_file(log_path, std::ios::binary);
    std::ostringstream log;
    log << log_file.rdbuf();
    result.log = log.str();
    CompileFailureKind kind = CompileFailureKind::None;
    result.messages = ParseMessages(result.log, &kind);
    result.auxiliary_logs.push_back(log_path);
    // main.log is the compiler's own log; keep it next to latexmk.log (§15).
    const auto main_log = request.workspace / "main.log";
    if (std::filesystem::exists(main_log, ec)) {
        result.auxiliary_logs.push_back(main_log);
    }
    result.pdf_path =
        request.workspace /
        (std::filesystem::path(request.package.entry_file).stem().string() +
         ".pdf");

    if (cancelled || (cancel_requested && cancel_requested->load())) {
        result.status = CompileStatus::Cancelled;
        result.failure_kind = CompileFailureKind::Cancelled;
    } else if (exit_code == 0 && std::filesystem::exists(result.pdf_path)) {
        result.status = CompileStatus::Success;
    } else {
        result.status = CompileStatus::Failure;
        result.failure_kind =
            kind == CompileFailureKind::None ? CompileFailureKind::LatexError
                                             : kind;
        if (result.messages.empty()) {
            result.messages.push_back(
                {"main.tex", 1, true, "LaTeX compilation failed"});
        }
    }
    return result;
}

// ---- MockCompiler ----

CompileResult MockCompiler::Compile(
    const CompileRequest& request,
    const std::atomic<bool>* cancel_requested) {
    CompileResult result;
    if (cancel_requested && cancel_requested->load()) {
        result.status = CompileStatus::Cancelled;
        result.failure_kind = CompileFailureKind::Cancelled;
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(request.workspace, ec);
    result.pdf_path = request.workspace / "main.pdf";
    if (succeeds_) {
        std::ofstream pdf(result.pdf_path, std::ios::binary | std::ios::trunc);
        pdf << "%PDF-1.4\n% PaperForge mock compiler\n";
        result.status = CompileStatus::Success;
        result.log = "mock compilation succeeded";
    } else {
        result.status = CompileStatus::Failure;
        result.failure_kind = CompileFailureKind::LatexError;
        result.log = "main.tex:1: simulated error";
        result.messages.push_back(
            {"main.tex", 1, true, "simulated error"});
    }
    return result;
}

}  // namespace pf
