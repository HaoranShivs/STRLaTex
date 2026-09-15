#pragma once
// ProjectSession: application aggregate / workflow coordinator
// (architecture section 十五). Owns ProjectState, EditingSystem, delegates
// to BuildCoordinator / SaveCoordinator / AssetManager / BibliographyService.
//
// M1 thread model - single-owner application state:
//
//   application thread                 background workers
//   ------------------                 ------------------
//   ProjectSession (this)              BuildCoordinator worker
//     ProjectState / Document            reads BuildSnapshot only
//     EditingSystem                      posts BuildResultReadyEvent
//     PreviewState
//     PersistenceState
//   ---------------------------------  ------------------------------------
//   ProcessApplicationEvents()  <----  SaveCoordinator worker
//     AcceptBuildResult()               reads SerializedProject only
//     ApplySaveCompletion()             posts SaveCompletedEvent
//
// Rule: ProjectState / ProjectSession / EditingSystem / Document are touched
// ONLY by the application thread. Workers see value objects (BuildSnapshot,
// SerializedProject, BuildResult, SaveCompletion) and nothing else.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "asset/AssetManager.h"
#include "bibliography/BibliographyService.h"
#include "build/BuildCoordinator.h"
#include "editing/EditingSystem.h"
#include "persistence/SaveCoordinator.h"
#include "project/ApplicationEvent.h"
#include "project/PreviewGate.h"
#include "project/PreviewUpdate.h"
#include "project/ProjectState.h"
#include "project/SnapshotFactory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace pf {

enum class LifecycleState : std::uint8_t {
    NoProject,
    Open,
    CloseRequested,
};

enum class PersistenceState : std::uint8_t {
    Clean,
    Dirty,
    Saving,
    SaveFailed,
};

enum class PreviewState : std::uint8_t {
    NoPreview,
    Fresh,
    Stale,
};

struct SessionPaths {
    std::filesystem::path project_dir;    // MyPaper/
    std::filesystem::path project_file;   // MyPaper/project.paper
    std::filesystem::path assets_dir;     // MyPaper/assets/
    std::filesystem::path build_dir;      // MyPaper/.paperforge/build/
    std::filesystem::path autosave_dir;   // MyPaper/.paperforge/autosave/
};

class ProjectSession {
public:
    struct Config {
        std::string tectonic_path = "tectonic";
        // Bundle cache for tectonic; empty disables the override.
        std::string tectonic_cache_dir;
        std::filesystem::path workspace_root = "/tmp/paperforge-sessions";
        std::chrono::milliseconds debounce{800};
        // Optional deterministic compiler (tests). Empty -> tectonic.
        std::function<std::unique_ptr<ICompiler>()> compiler_factory;
    };

    explicit ProjectSession(Config config);
    ~ProjectSession();

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    // ---- Lifecycle ----
    bool NewProject(const std::filesystem::path& project_dir);
    bool OpenProject(const std::filesystem::path& project_dir, std::string* error);
    // Open, preferring a newer autosave/recovery snapshot if one exists.
    // Returns true when a recovery snapshot was applied (caller may inform).
    bool OpenProjectWithRecovery(const std::filesystem::path& project_dir,
                                 std::string* error, bool* recovered = nullptr);
    void CloseProject();

    // ---- Application-thread event pump (M1) ----
    // Thread-safe. Called by background workers to hand a value object to the
    // application thread. Never touches ProjectState.
    void PostApplicationEvent(ApplicationEvent event);
    // Thread-safe. The build worker's completion path; also the seam tests use
    // to inject a late/foreign result and prove the gate rejects it. The
    // result is only *applied* in ProcessApplicationEvents().
    void PostBuildResult(BuildResult result);

    // Application/owner thread only. Drains queued events and applies them
    // (phase changes, build results, save completions, autosave ticks).
    void ProcessApplicationEvents();
    // Blocks until at least one event is available, then drains - used by
    // non-Qt drivers (CLI/tests) that poll for completion.
    bool WaitForApplicationEvent(std::chrono::milliseconds timeout);

    bool HasPendingApplicationEvents() const;

    // Installed by the host (Qt adapter) so a background post can wake the
    // application event loop instead of being polled. Called on worker
    // threads; must itself be thread-safe.
    void SetWakeHandler(std::function<void()> handler);

    // Application thread: the preview gate. Returns true when `result` was
    // produced for the project/revision/build that is current right now.
    // Stale results are dropped here, never in a worker.
    bool AcceptBuildResult(const BuildResult& result);

    // ---- Autosave (crash recovery) ----
    // Starts/stops a background timer (default 30s). The timer thread only
    // posts an AutosaveTickEvent; the snapshot is captured on the app thread.
    // Autosaves only when dirty and never clear Clean/Dirty (architecture 32).
    void StartAutosaveTimer(std::chrono::milliseconds interval =
                                std::chrono::milliseconds{30000});
    void StopAutosaveTimer();
    // True when an autosave snapshot newer than project.paper exists.
    bool HasRecoverySnapshot() const;
    SaveResult RecoverFromAutosave();

    // ---- Editing (application thread) ----
    EditResult Execute(const EditCommand& command);
    EditResult Undo();
    EditResult Redo();

    // ---- Build ----
    void RequestBuild(bool manual = false);
    void CancelBuild();
    BuildPhase build_phase() const;
    // Identity of the most recent build request (empty when none).
    const std::string& latest_snapshot_id() const noexcept {
        return latest_snapshot_id_;
    }
    const BuildId& latest_build_id() const noexcept { return latest_build_id_; }

    // ---- Save ----
    // Both enqueue an immutable snapshot on the save worker and return
    // immediately with SaveResult::Status::Queued. Completion is applied in
    // ProcessApplicationEvents().
    SaveResult Save();
    SaveResult Autosave();
    // Application thread: block until every queued save is written and its
    // completion is applied. Returns the last user-save result.
    SaveResult FlushSaves();
    size_t pending_saves() const { return save_coordinator_.pending(); }

    // ---- Template ----
    void ChangeTemplate(const std::string& template_id);

    // ---- Assets ----
    AssetImportResult ImportAsset(const std::filesystem::path& source);
    // Stage + register + insert figure in one user-facing operation.
    EditResult InsertFigureFromSource(const std::filesystem::path& source,
                                      const NodeId& parent,
                                      std::optional<size_t> index = std::nullopt);

    // ---- Bibliography ----
    BibliographyImportResult ImportBibliography(const std::string& bibtex_text);
    CitationSearchResult SearchCitations(const std::string& query);
    bool HasBibliography() const { return !bibliography_bibtex_.empty(); }

    // ---- State access (application thread only) ----
    ProjectState& state() noexcept { return state_; }
    const ProjectState& state() const noexcept { return state_; }
    EditingSystem& editing() noexcept { return editing_; }
    UndoHistory& history() noexcept { return editing_.history(); }
    DocumentIndex& index() noexcept { return editing_.index(); }
    AssetManager& assets() noexcept { return *assets_; }
    BibliographyDatabase& bibliography() noexcept { return bibliography_db_; }

    // ---- Single-owner diagnostics ----
    // True when the caller runs on the thread that constructed the session.
    bool IsOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner_thread_;
    }
    // Counts calls into owner-only entry points from a foreign thread. A
    // correct build keeps this at zero; regression tests assert it.
    std::size_t owner_thread_violations() const noexcept {
        return owner_thread_violations_.load();
    }

    LifecycleState lifecycle_state() const noexcept { return lifecycle_state_; }
    PersistenceState persistence_state() const noexcept { return persistence_state_; }
    PreviewState preview_state() const noexcept { return preview_state_; }
    ProjectRevision current_revision() const noexcept { return state_.revision(); }
    const SessionPaths& paths() const noexcept { return paths_; }
    const std::string& bibliography_bibtex() const noexcept {
        return bibliography_bibtex_;
    }

    // ---- Observers (application thread) ----
    // Accepted build results only (stale ones never reach here).
    void SetBuildResultHandler(std::function<void(const BuildResult&)> handler) {
        build_result_handler_ = std::move(handler);
    }
    // Typed preview event: which project/build/revision produced the PDF.
    void SetPreviewUpdateHandler(
        std::function<void(const PreviewUpdate&)> handler) {
        preview_update_handler_ = std::move(handler);
    }
    void SetSaveResultHandler(
        std::function<void(const SaveResult&, SaveKind)> handler) {
        save_result_handler_ = std::move(handler);
    }
    void SetPhaseHandler(std::function<void(BuildPhase, BuildPhase)> handler) {
        phase_handler_ = std::move(handler);
    }
    void SetDocumentChangedHandler(
        std::function<void(const DocumentChangedEvent&)> handler) {
        document_changed_handler_ = std::move(handler);
    }

private:
    void EnsureDirectories();
    void MarkDirty();
    // Smoke detector for the single-owner rule: bumps a counter when an
    // owner-only entry point is entered from a foreign thread.
    void NoteOwnerThreadUse() const;
    // Application thread: capture the current state as an immutable snapshot.
    SaveSnapshot CaptureSaveSnapshot() const;

    // Event application (application thread).
    void HandleEvent(const BuildPhaseChangedEvent& event);
    void HandleEvent(const BuildResultReadyEvent& event);
    void HandleEvent(const SaveCompletedEvent& event);
    void HandleEvent(const AutosaveTickEvent& event);
    void ApplySaveCompletion(const SaveCompletion& completion);
    // Identity of the newest build request; used by the preview gate.
    PreviewGateInput CurrentGateInput() const;

    Config config_;
    SessionPaths paths_;
    ProjectState state_;
    std::unique_ptr<AssetManager> assets_;
    BibliographyDatabase bibliography_db_;
    std::string bibliography_bibtex_;
    std::uint64_t bibliography_revision_ = 0;
    SnapshotFactory snapshot_factory_;
    EditingSystem editing_;
    std::unique_ptr<ICompiler> compiler_;
    SaveCoordinator save_coordinator_;
    std::unique_ptr<BuildCoordinator> build_coordinator_;

    LifecycleState lifecycle_state_ = LifecycleState::NoProject;
    PersistenceState persistence_state_ = PersistenceState::Clean;
    PreviewState preview_state_ = PreviewState::NoPreview;

    // Thread that owns every mutable member below (M1 single-owner rule).
    const std::thread::id owner_thread_ = std::this_thread::get_id();
    mutable std::atomic<std::size_t> owner_thread_violations_{0};

    // Identity of the most recent build request (application thread).
    std::string latest_snapshot_id_;
    BuildId latest_build_id_;

    // Autosave timer state
    std::thread autosave_thread_;
    std::atomic<bool> autosave_stop_{false};
    std::chrono::milliseconds autosave_interval_{30000};

    // Cross-thread mailbox: events posted by workers, drained by the owner.
    mutable std::mutex events_mutex_;
    std::condition_variable events_condition_;
    std::vector<ApplicationEvent> pending_events_;
    std::function<void()> wake_handler_;

    // Last user-save result observed (application thread).
    SaveResult last_user_save_result_;

    std::function<void(const BuildResult&)> build_result_handler_;
    std::function<void(const PreviewUpdate&)> preview_update_handler_;
    std::function<void(const SaveResult&, SaveKind)> save_result_handler_;
    std::function<void(BuildPhase, BuildPhase)> phase_handler_;
    std::function<void(const DocumentChangedEvent&)> document_changed_handler_;
};

}  // namespace pf
