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
  // Resolved by the session when the snapshot was captured (plan §14): the
  // engine is the template's decision, carried with the request so the
  // compile stage never re-derives it from the document.
  BuildToolchain toolchain;
  // Identity of this build attempt. Minted with the snapshot so the
  // application thread can tell one attempt from another when the result
  // comes back.
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
  // Classification of a failure (plan §37): runtime problems are distinct
  // from document LaTeX problems so the UI never mixes them.
  CompileFailureKind failure_kind = CompileFailureKind::None;
  ProjectId project_id;
  std::string snapshot_id;
  BuildId build_id;
  ProjectRevision revision;
  std::filesystem::path pdf_path;
  std::string log;
  std::vector<Diagnostic> diagnostics;
  // Session bookkeeping (Build Diagnostics plan §3): the wall-clock span of
  // the attempt and the compiler's exit code, so the Build Log footer can
  // show "Duration: 1.662 s" and the success rule is auditable (§31).
  std::int64_t started_ms = 0;
  std::int64_t finished_ms = 0;
  int exit_code = -1;
};

class BuildCoordinator {
public:
  struct Host {
    std::function<ProjectId()> project_id;
    std::function<std::string()> workspace_root;
    // TEMP-DEBUG: project-local directory for dumping generated LaTeX
    // (e.g. <project>/.paperforge/build). Empty optional disables dumps.
    std::function<std::optional<std::string>()> debug_dump_dir;
    std::function<void(const BuildResult &)> on_build_finished;
    std::function<void(BuildPhase, BuildPhase)> on_phase_changed;
    // Structured lifecycle events (Build Diagnostics plan §4). Emitted on the
    // worker thread for every step of a build, each carrying the build id it
    // belongs to. The host republishes them to the application thread; the
    // Build Log view renders them. The compiler also streams stdout/stderr
    // here so the log grows live during a compile (§44).
    std::function<void(const BuildEvent &)> on_build_event;
  };

  BuildCoordinator(Host host, ICompiler *compiler);
  // Production form (plan §13): the coordinator asks for a compiler per
  // build, chosen from the snapshot's toolchain. Exactly one of the two
  // constructors is used for the coordinator's lifetime.
  BuildCoordinator(
      Host host,
      std::function<std::unique_ptr<ICompiler>(const BuildToolchain &)>
          compiler_provider);
  ~BuildCoordinator();

  BuildCoordinator(const BuildCoordinator &) = delete;
  BuildCoordinator &operator=(const BuildCoordinator &) = delete;

  void RequestBuild(BuildSnapshot snapshot, bool manual = false);
  void Cancel();
  void set_debounce(std::chrono::milliseconds debounce);
  BuildPhase phase() const;

private:
  void WorkerLoop();
  void SetPhase(BuildPhase phase);
  // Publish one lifecycle event tagged with the build id (plan §4). No-op
  // when the host has no sink, so tests that ignore events keep working.
  void EmitEvent(const BuildId &build_id, BuildEventType type,
                 std::string message) const;
  BuildResult BuildOne(const BuildSnapshot &snapshot);

  Host host_;
  ICompiler *compiler_ = nullptr;
  std::function<std::unique_ptr<ICompiler>(const BuildToolchain &)>
      compiler_provider_;
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
