#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "build/BuildEvent.h"
#include "build/Compiler.h"
#include "core/Diagnostic.h"
#include "document/Document.h"

namespace pf {

struct BuildSnapshot {
    ProjectId project_id;
    std::string snapshot_id;
    // 由 session 在捕获 snapshot 时解析（方案 §14）：engine 是模板的决定，
    // 随请求一同携带，使 compile 阶段绝不会从 document 重新推导。
    BuildToolchain toolchain;
    // 本次 build 尝试的身份标识。与 snapshot 一同生成，使结果返回时
    // 应用线程能区分不同的尝试。
    BuildId build_id;
    ProjectRevision revision;
    std::shared_ptr<const Document> document;
    std::string template_id;
    std::string bibliography_bibtex;
    std::map<std::string, std::string> asset_files;
    std::map<std::string, std::filesystem::path> asset_sources;
};

enum class BuildPhase : std::uint8_t {
    Idle,
    Debouncing,
    Rendering,
    Compiling,
};

struct BuildResult {
    enum class Outcome : std::uint8_t {
        Success,
        Failure,
        Cancelled,
        Superseded,
    };

    Outcome outcome = Outcome::Failure;
    // 失败分类（方案 §37）：运行时问题与 document 的 LaTeX 问题相互区分，
    // 使 UI 绝不会将二者混淆。
    CompileFailureKind failure_kind = CompileFailureKind::None;
    ProjectId project_id;
    std::string snapshot_id;
    BuildId build_id;
    ProjectRevision revision;
    std::filesystem::path pdf_path;
    std::string log;
    std::vector<Diagnostic> diagnostics;
    // Session 记账（Build Diagnostics 方案 §3）：本次尝试的墙上时钟时长与
    // 编译器的退出码，使 Build Log 页脚能显示「Duration: 1.662 s」，
    // 且成功判定可审计（§31）。
    std::int64_t started_ms = 0;
    std::int64_t finished_ms = 0;
    int exit_code = -1;
};

class BuildCoordinator {
  public:
    struct Host {
        std::function<ProjectId()> project_id;
        std::function<std::string()> workspace_root;
        // TEMP-DEBUG：用于转储生成的 LaTeX 的项目本地目录
        //（例如 <project>/.paperforge/build）。optional 为空时禁用转储。
        std::function<std::optional<std::string>()> debug_dump_dir;
        std::function<void(const BuildResult&)> on_build_finished;
        std::function<void(BuildPhase, BuildPhase)> on_phase_changed;
        // 结构化的生命周期事件（Build Diagnostics 方案 §4）。在 worker 线程上
        // 为 build 的每一步发出，每个事件携带其所属的 build id。host 将它们
        // 转发到应用线程；Build Log 视图负责渲染。编译器也在此流式输出
        // stdout/stderr，使日志在 compile 期间实时增长（§44）。
        std::function<void(const BuildEvent&)> on_build_event;
    };

    BuildCoordinator(Host host, ICompiler* compiler);
    // 生产形式（方案 §13）：coordinator 为每次 build 请求一个 compiler，
    // 由 snapshot 的 toolchain 选定。coordinator 的生命周期内只会使用
    // 这两个构造函数之一。
    BuildCoordinator(Host host, std::function<std::unique_ptr<ICompiler>(const BuildToolchain&)> compiler_provider);
    ~BuildCoordinator();

    BuildCoordinator(const BuildCoordinator&) = delete;
    BuildCoordinator& operator=(const BuildCoordinator&) = delete;

    void RequestBuild(BuildSnapshot snapshot, bool manual = false);
    void Cancel();
    void set_debounce(std::chrono::milliseconds debounce);
    BuildPhase phase() const;

  private:
    void WorkerLoop();
    void SetPhase(BuildPhase phase);
    // 发布一个带 build id 标记的生命周期事件（方案 §4）。host 未设置接收端时
    // 为空操作，因此忽略事件的测试仍可正常工作。
    void EmitEvent(const BuildId& build_id, BuildEventType type, std::string message) const;
    BuildResult BuildOne(const BuildSnapshot& snapshot);

    Host host_;
    ICompiler* compiler_ = nullptr;
    std::function<std::unique_ptr<ICompiler>(const BuildToolchain&)> compiler_provider_;
    std::unique_ptr<ICompiler> owned_compiler_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<BuildSnapshot> pending_;
    std::chrono::steady_clock::time_point due_;
    std::chrono::milliseconds debounce_{800};
    std::thread worker_;
    std::atomic<bool> cancel_requested_{false};
    bool stopping_ = false;
    std::uint64_t request_generation_ = 0;
    BuildPhase phase_ = BuildPhase::Idle;
};

} // namespace pf
