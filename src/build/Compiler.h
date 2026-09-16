#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "render/LatexRenderer.h"
#include "build/Toolchain.h"

namespace pf {


// Compile-stage configuration (plan §9): which engine, and where the TeX
// environment lives. texlive_root is the bundled runtime; empty means "not
// available" and the compiler reports that as a runtime error, not a
// document error.
struct CompilerConfig {
    LatexEngine engine = LatexEngine::PdfLatex;
    BibliographyEngine bibliography_engine = BibliographyEngine::None;
    std::filesystem::path texlive_root;
    bool keep_logs = true;
};

// Classification of why a compile failed (plan §37). Runtime problems are
// distinct from document problems so the UI can show them differently.
enum class CompileFailureKind : std::uint8_t {
    None,
    RuntimeMissing,
    RuntimeCorrupted,
    PackageMissing,
    FontMissing,
    LatexError,
    BibliographyError,
    Timeout,
    Cancelled,
    InternalError,
};

const char* ToString(CompileFailureKind kind);

enum class CompileStatus : std::uint8_t {
    Success,
    Failure,
    Cancelled,
};

struct CompilerMessage {
    std::string file;
    std::uint32_t line = 0;
    bool is_error = true;
    std::string text;
};

struct CompileRequest {
    BuildPackage package;
    std::filesystem::path workspace;
    // Destination name in the generated package -> source file on disk.
    std::map<std::string, std::filesystem::path> asset_sources;
    // Resolved toolchain for this build (plan §14): decided by the template at
    // request time, never re-derived inside the compiler.
    BuildToolchain toolchain;
};

struct CompileResult {
    CompileStatus status = CompileStatus::Failure;
    std::filesystem::path pdf_path;
    std::string log;
    std::vector<CompilerMessage> messages;
    // Auxiliary logs worth keeping next to build.log (plan §15):
    // latexmk.log, main.log, main.blg ...
    std::vector<std::filesystem::path> auxiliary_logs;
    CompileFailureKind failure_kind = CompileFailureKind::None;
};

class ICompiler {
public:
    virtual ~ICompiler() = default;
    virtual CompileResult Compile(
        const CompileRequest& request,
        const std::atomic<bool>* cancel_requested = nullptr) = 0;
};

class TectonicCompiler final : public ICompiler {
public:
    // cache_dir, when it exists, is exported as TECTONIC_CACHE_DIR and HOME
    // for the tectonic child process. That pins builds to the bundle shipped
    // with the project instead of whatever cache the ambient $HOME holds,
    // which keeps offline builds reproducible.
    explicit TectonicCompiler(std::string executable,
                              std::string cache_dir = {});

    CompileResult Compile(
        const CompileRequest& request,
        const std::atomic<bool>* cancel_requested = nullptr) override;

private:
    std::string executable_;
    std::string cache_dir_;
};

// The production backend (plan §10, §30): drives latexmk from the bundled
// portable TeX Live runtime with an isolated environment.
class TexLiveCompiler final : public ICompiler {
public:
    explicit TexLiveCompiler(CompilerConfig config);

    CompileResult Compile(
        const CompileRequest& request,
        const std::atomic<bool>* cancel_requested = nullptr) override;

private:
    CompilerConfig config_;
};

class MockCompiler final : public ICompiler {
public:
    explicit MockCompiler(bool succeeds = true) : succeeds_(succeeds) {}

    CompileResult Compile(
        const CompileRequest& request,
        const std::atomic<bool>* cancel_requested = nullptr) override;

private:
    bool succeeds_;
};

}  // namespace pf
