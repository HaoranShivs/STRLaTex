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

#include "build/Compiler.h"
#include "core/Diagnostic.h"
#include "document/Document.h"

namespace pf {

struct BuildSnapshot {
    ProjectId project_id;
    std::string snapshot_id;
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
    ProjectId project_id;
    std::string snapshot_id;
    ProjectRevision revision;
    std::filesystem::path pdf_path;
    std::string log;
    std::vector<Diagnostic> diagnostics;
};

class BuildCoordinator {
public:
    struct Host {
        std::function<ProjectId()> project_id;
        std::function<std::string()> workspace_root;
        std::function<void(const BuildResult&)> on_build_finished;
        std::function<void(BuildPhase, BuildPhase)> on_phase_changed;
    };

    BuildCoordinator(Host host, ICompiler* compiler);
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
    BuildResult BuildOne(const BuildSnapshot& snapshot);

    Host host_;
    ICompiler* compiler_;
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

}  // namespace pf
