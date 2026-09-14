#pragma once
// ProjectSession: application aggregate / workflow coordinator
// (architecture section 十五). Owns ProjectState, EditingSystem, delegates
// to BuildCoordinator / SaveCoordinator / AssetManager / BibliographyService.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "asset/AssetManager.h"
#include "bibliography/BibliographyService.h"
#include "build/BuildCoordinator.h"
#include "editing/EditingSystem.h"
#include "project/ProjectState.h"
#include "project/SnapshotFactory.h"
#include "persistence/SaveCoordinator.h"

#include <atomic>
#include <chrono>
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
        std::filesystem::path workspace_root = "/tmp/paperforge-sessions";
        std::chrono::milliseconds debounce{800};
    };

    explicit ProjectSession(Config config);
    ~ProjectSession();

    // ---- Lifecycle ----
    bool NewProject(const std::filesystem::path& project_dir);
    bool OpenProject(const std::filesystem::path& project_dir, std::string* error);
    // Open, preferring a newer autosave/recovery snapshot if one exists.
    // Returns true when a recovery snapshot was applied (caller may inform).
    bool OpenProjectWithRecovery(const std::filesystem::path& project_dir,
                                 std::string* error, bool* recovered = nullptr);
    void CloseProject();

    // ---- Autosave (crash recovery) ----
    // Starts/stops a background autosave timer (default 30s; autosaves only
    // when dirty; does not change Clean/Dirty state per architecture 32).
    void StartAutosaveTimer(std::chrono::milliseconds interval =
                                std::chrono::milliseconds{30000});
    void StopAutosaveTimer();
    // True when an autosave snapshot newer than project.paper exists.
    bool HasRecoverySnapshot() const;
    SaveResult RecoverFromAutosave();

    // ---- Editing ----
    EditResult Execute(const EditCommand& command);
    EditResult Undo();
    EditResult Redo();

    // ---- Build ----
    void RequestBuild(bool manual = false);
    void CancelBuild();
    BuildPhase build_phase() const;

    // ---- Save ----
    SaveResult Save();
    SaveResult Autosave();

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

    // ---- State access ----
    ProjectState& state() noexcept { return state_; }
    const ProjectState& state() const noexcept { return state_; }
    EditingSystem& editing() noexcept { return editing_; }
    UndoHistory& history() noexcept { return editing_.history(); }
    DocumentIndex& index() noexcept { return editing_.index(); }
    AssetManager& assets() noexcept { return *assets_; }
    BibliographyDatabase& bibliography() noexcept { return bibliography_db_; }

    LifecycleState lifecycle_state() const noexcept { return lifecycle_state_; }
    PersistenceState persistence_state() const noexcept { return persistence_state_; }
    PreviewState preview_state() const noexcept { return preview_state_; }
    ProjectRevision current_revision() const noexcept { return state_.revision(); }
    const SessionPaths& paths() const noexcept { return paths_; }
    const std::string& bibliography_bibtex() const noexcept {
        return bibliography_bibtex_;
    }

    // ---- Observers ----
    void SetBuildResultHandler(std::function<void(const BuildResult&)> handler) {
        build_result_handler_ = std::move(handler);
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
    void OnBuildFinished(const BuildResult& result);
    void MarkDirty();
    SerializedProject CaptureProjectSnapshot() const;

    Config config_;
    SessionPaths paths_;
    ProjectState state_;
    std::unique_ptr<AssetManager> assets_;
    BibliographyDatabase bibliography_db_;
    std::string bibliography_bibtex_;
    std::uint64_t bibliography_revision_ = 0;
    SnapshotFactory snapshot_factory_;
    EditingSystem editing_;
    SaveCoordinator save_coordinator_;
    std::unique_ptr<TectonicCompiler> compiler_;
    std::unique_ptr<BuildCoordinator> build_coordinator_;

    LifecycleState lifecycle_state_ = LifecycleState::NoProject;
    PersistenceState persistence_state_ = PersistenceState::Clean;
    PreviewState preview_state_ = PreviewState::NoPreview;

    // Autosave timer state
    std::thread autosave_thread_;
    std::atomic<bool> autosave_stop_{false};
    std::chrono::milliseconds autosave_interval_{30000};

    std::function<void(const BuildResult&)> build_result_handler_;
    std::function<void(BuildPhase, BuildPhase)> phase_handler_;
    std::function<void(const DocumentChangedEvent&)> document_changed_handler_;
};

}  // namespace pf
