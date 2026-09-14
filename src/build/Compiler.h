#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "render/LatexRenderer.h"

namespace pf {

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
};

struct CompileResult {
    CompileStatus status = CompileStatus::Failure;
    std::filesystem::path pdf_path;
    std::string log;
    std::vector<CompilerMessage> messages;
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
    explicit TectonicCompiler(std::string executable);

    CompileResult Compile(
        const CompileRequest& request,
        const std::atomic<bool>* cancel_requested = nullptr) override;

private:
    std::string executable_;
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
