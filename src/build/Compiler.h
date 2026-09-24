#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "build/Toolchain.h"
#include "render/LatexRenderer.h"

namespace pf {

// 编译阶段配置（方案 §9）：使用哪个引擎，以及 TeX 环境所在的位置。
// texlive_root 是随附的运行时；为空表示「不可用」，此时编译器将其报为
// 运行时错误，而不是文档错误。
struct CompilerConfig {
    LatexEngine engine = LatexEngine::PdfLatex;
    BibliographyEngine bibliography_engine = BibliographyEngine::None;
    std::filesystem::path texlive_root;
    bool keep_logs = true;
};

// 编译失败原因的归类（方案 §37）。运行时问题与文档问题彼此区分，
// 以便 UI 以不同方式展示。
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

// 编译器运行期间交给 UI 的原始输出分片（Build Diagnostics 方案 §44）：
// 即使 Diagnostic 解析器只看到已完成的日志，编译器仍会持续把 stdout/stderr
// 流式写入 Build Log。
struct CompileOutputChunk {
    bool is_stderr = false;
    std::string text;
};

struct CompileRequest {
    BuildPackage package;
    std::filesystem::path workspace;
    // 生成的 package 中的目标名称 -> 磁盘上的源文件。
    std::map<std::string, std::filesystem::path> asset_sources;
    // 本次 build 已解析的 toolchain（方案 §14）：由模板在请求时确定，
    // 绝不在编译器内部重新推导。
    BuildToolchain toolchain;
    // 可选的实时输出钩子。子进程每产生一段 stdout/stderr，就由编译器自己的
    // 线程调用一次；接收方必须复制自己需要保留的内容（该分片会被复用）。
    std::function<void(const CompileOutputChunk&)> on_output;
};

struct CompileResult {
    CompileStatus status = CompileStatus::Failure;
    std::filesystem::path pdf_path;
    std::string log;
    std::vector<CompilerMessage> messages;
    // 值得与 build.log 一起保留的辅助日志（方案 §15）：
    // latexmk.log、main.log、main.blg ...
    std::vector<std::filesystem::path> auxiliary_logs;
    CompileFailureKind failure_kind = CompileFailureKind::None;
    // 编译器进程的退出码；没有进程运行时为 -1（方案 §3：
    // 属于 BuildSession 记录的一部分）。
    int exit_code = -1;
};

class ICompiler {
  public:
    virtual ~ICompiler() = default;
    virtual CompileResult Compile(const CompileRequest& request,
                                  const std::atomic<bool>* cancel_requested = nullptr) = 0;
};

class TectonicCompiler final : public ICompiler {
  public:
    // cache_dir 存在时，会作为 TECTONIC_CACHE_DIR 与 HOME 导出给 tectonic
    // 子进程。这样可以把 build 固定到项目随附的 bundle，而不是环境 $HOME
    // 中的任意缓存，从而保证离线 build 可复现。
    explicit TectonicCompiler(std::string executable, std::string cache_dir = {});

    CompileResult Compile(const CompileRequest& request, const std::atomic<bool>* cancel_requested = nullptr) override;

  private:
    std::string executable_;
    std::string cache_dir_;
};

// 生产后端（方案 §10、§30）：在隔离环境中，用随附的便携版 TeX Live
// 运行时驱动 latexmk。
class TexLiveCompiler final : public ICompiler {
  public:
    explicit TexLiveCompiler(CompilerConfig config);

    CompileResult Compile(const CompileRequest& request, const std::atomic<bool>* cancel_requested = nullptr) override;

  private:
    CompilerConfig config_;
};

class MockCompiler final : public ICompiler {
  public:
    explicit MockCompiler(bool succeeds = true) : succeeds_(succeeds) {}

    CompileResult Compile(const CompileRequest& request, const std::atomic<bool>* cancel_requested = nullptr) override;

  private:
    bool succeeds_;
};

} // namespace pf
