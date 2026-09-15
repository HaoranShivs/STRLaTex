#include "project/ProjectSession.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "bibliography/BibliographyService.h"
#include "core/IdGenerator.h"
#include "document/InlineText.h"

namespace pf {

ProjectSession::ProjectSession(Config config)
    : config_(std::move(config)),
      assets_(std::make_unique<AssetManager>(
          std::filesystem::path("/tmp/paperforge-assets"))),
      snapshot_factory_(assets_.get()),
      // The save worker hands completions back through the application event
      // queue; it never calls into the domain directly.
      save_coordinator_([this](const SaveCompletion& completion) {
          SaveCompletedEvent event;
          event.completion = completion;
          PostApplicationEvent(std::move(event));
      }) {
    // Wire the editing system after members exist (lambdas capture `this`).
    EditingSystem::Host ehost;
    ehost.project_id = [this] { return state_.id(); };
    ehost.revision = [this] { return state_.revision(); };
    ehost.bump_revision = [this] { return state_.BumpRevision(); };
    ehost.document = [this]() -> Document& { return state_.mutable_document(); };
    ehost.on_document_changed = [this](const DocumentChangedEvent& e) {
        if (document_changed_handler_) document_changed_handler_(e);
    };
    editing_.SetHost(std::move(ehost));

    // Compiler: injected factory (tests) or tectonic at the configured path.
    if (config_.compiler_factory) {
        compiler_ = config_.compiler_factory();
    } else {
        compiler_ = std::make_unique<TectonicCompiler>(config_.tectonic_path,
                                                       config_.tectonic_cache_dir);
    }

    BuildCoordinator::Host bhost;
    bhost.project_id = [this] { return state_.id(); };
    // Immutable configuration only - never mutable project state.
    bhost.workspace_root = [this] { return config_.workspace_root.string(); };
    // Worker thread: publish a value object, never read the live state.
    bhost.on_build_finished = [this](const BuildResult& result) {
        PostBuildResult(result);
    };
    bhost.on_phase_changed = [this](BuildPhase previous, BuildPhase current) {
        BuildPhaseChangedEvent event;
        event.previous = previous;
        event.current = current;
        PostApplicationEvent(std::move(event));
    };
    build_coordinator_ = std::make_unique<BuildCoordinator>(std::move(bhost),
                                                            compiler_.get());
    build_coordinator_->set_debounce(config_.debounce);
}

ProjectSession::~ProjectSession() {
    StopAutosaveTimer();
    // Stop the producers while the state they post about is still alive, and
    // give queued saves a chance to reach disk.
    build_coordinator_.reset();
    save_coordinator_.Shutdown();
}

// ---------------- Application event pump ----------------

void ProjectSession::NoteOwnerThreadUse() const {
    if (std::this_thread::get_id() != owner_thread_) {
        owner_thread_violations_.fetch_add(1);
    }
}

void ProjectSession::PostApplicationEvent(ApplicationEvent event) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        pending_events_.push_back(std::move(event));
        wake = wake_handler_;
    }
    events_condition_.notify_all();
    if (wake) wake();
}

void ProjectSession::PostBuildResult(BuildResult result) {
    BuildResultReadyEvent event;
    event.result = std::move(result);
    PostApplicationEvent(std::move(event));
}

void ProjectSession::SetWakeHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    wake_handler_ = std::move(handler);
}

bool ProjectSession::HasPendingApplicationEvents() const {
    std::lock_guard<std::mutex> lock(events_mutex_);
    return !pending_events_.empty();
}

void ProjectSession::ProcessApplicationEvents() {
    NoteOwnerThreadUse();
    std::vector<ApplicationEvent> events;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events.swap(pending_events_);
    }
    // Applied outside the lock: handlers may post further events.
    for (const auto& event : events) {
        std::visit([this](const auto& typed) { HandleEvent(typed); }, event);
    }
}

bool ProjectSession::WaitForApplicationEvent(std::chrono::milliseconds timeout) {
    {
        std::unique_lock<std::mutex> lock(events_mutex_);
        events_condition_.wait_for(
            lock, timeout,
            [this] { return !pending_events_.empty(); });
        if (pending_events_.empty()) return false;
    }
    ProcessApplicationEvents();
    return true;
}

void ProjectSession::HandleEvent(const BuildPhaseChangedEvent& event) {
    if (phase_handler_) phase_handler_(event.previous, event.current);
}

void ProjectSession::HandleEvent(const BuildResultReadyEvent& event) {
    AcceptBuildResult(event.result);
}

void ProjectSession::HandleEvent(const SaveCompletedEvent& event) {
    ApplySaveCompletion(event.completion);
}

void ProjectSession::HandleEvent(const AutosaveTickEvent&) {
    // The timer thread only rang the bell; the snapshot is captured here, on
    // the thread that owns the document.
    if (lifecycle_state_ == LifecycleState::Open &&
        persistence_state_ == PersistenceState::Dirty) {
        Autosave();
    }
}

PreviewGateInput ProjectSession::CurrentGateInput() const {
    PreviewGateInput input;
    input.has_project = lifecycle_state_ == LifecycleState::Open;
    input.project_id = state_.id();
    input.revision = state_.revision();
    input.snapshot_id = latest_snapshot_id_;
    input.build_id = latest_build_id_;
    return input;
}

bool ProjectSession::AcceptBuildResult(const BuildResult& result) {
    NoteOwnerThreadUse();
    // Architecture section 47: the staleness decision belongs to the thread
    // that owns the revision. Workers never make it.
    const PreviewGateDecision decision =
        EvaluatePreviewGate(CurrentGateInput(), result);
    if (decision != PreviewGateDecision::Accept) return false;

    if (result.outcome == BuildResult::Outcome::Success) {
        preview_state_ = PreviewState::Fresh;
    } else if (result.outcome == BuildResult::Outcome::Failure) {
        // Keep the last successful PDF on disk; the preview is out of date.
        if (preview_state_ != PreviewState::NoPreview) {
            preview_state_ = PreviewState::Stale;
        }
    }

    PreviewUpdate update;
    update.project_id = result.project_id;
    update.build_id = result.build_id;
    update.revision = result.revision;
    update.success = result.outcome == BuildResult::Outcome::Success;
    if (update.success) {
        update.pdf.path = result.pdf_path;
        update.pdf.build_id = result.build_id;
        update.pdf.revision = result.revision;
    }
    if (build_result_handler_) build_result_handler_(result);
    if (preview_update_handler_) preview_update_handler_(update);
    return true;
}

// ---------------- Lifecycle ----------------

void ProjectSession::EnsureDirectories() {
    std::error_code ec;
    std::filesystem::create_directories(paths_.project_dir, ec);
    std::filesystem::create_directories(paths_.assets_dir, ec);
    std::filesystem::create_directories(paths_.build_dir, ec);
    std::filesystem::create_directories(paths_.autosave_dir, ec);
    // Re-point asset manager at this project's assets dir.
    assets_ = std::make_unique<AssetManager>(paths_.assets_dir);
    snapshot_factory_ = SnapshotFactory(assets_.get());
}

bool ProjectSession::NewProject(const std::filesystem::path& project_dir) {
    CloseProject();
    paths_.project_dir = project_dir;
    paths_.project_file = project_dir / "project.paper";
    paths_.assets_dir = project_dir / "assets";
    paths_.build_dir = project_dir / ".paperforge" / "build";
    paths_.autosave_dir = project_dir / ".paperforge" / "autosave";
    EnsureDirectories();

    state_.Reset();
    state_.SetId(ProjectId(IdGenerator::NewProjectId()));
    state_.mutable_settings().name = project_dir.filename().string();
    lifecycle_state_ = LifecycleState::Open;
    persistence_state_ = PersistenceState::Dirty;  // not yet saved
    preview_state_ = PreviewState::NoPreview;
    return true;
}

bool ProjectSession::OpenProject(const std::filesystem::path& project_dir,
                                 std::string* error) {
    CloseProject();
    paths_.project_dir = project_dir;
    paths_.project_file = project_dir / "project.paper";
    paths_.assets_dir = project_dir / "assets";
    paths_.build_dir = project_dir / ".paperforge" / "build";
    paths_.autosave_dir = project_dir / ".paperforge" / "autosave";
    EnsureDirectories();

    LoadRequest request;
    request.project_file = paths_.project_file;
    auto result = ProjectPersistence::Load(request);
    if (!result.project) {
        if (error) *error = result.detail;
        lifecycle_state_ = LifecycleState::NoProject;
        return false;
    }
    const auto& sp = *result.project;
    state_.Reset();
    state_.SetId(ProjectId(sp.project_id.empty() ? IdGenerator::NewProjectId()
                                                 : sp.project_id));
    state_.SetRevision(ProjectRevision{sp.revision});
    state_.mutable_document() = sp.document;
    state_.mutable_template() = sp.template_id.empty() ? "generic-article"
                                                       : sp.template_id;
    state_.mutable_settings().bibliography_path = sp.bibliography_path;
    for (const auto& meta : sp.assets) {
        assets_->registry().Register(meta);
    }
    // Load bibliography file if present.
    auto bib_path = paths_.project_dir / sp.bibliography_path;
    if (std::filesystem::exists(bib_path)) {
        BibliographyService service(bibliography_db_);
        auto import = service.ImportFile(bib_path.string());
        if (import.status == BibliographyImportResult::Status::Ok) {
            std::ifstream in(bib_path, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            bibliography_bibtex_ = ss.str();
            bibliography_revision_ = import.bibliography_revision;
        }
    }
    lifecycle_state_ = LifecycleState::Open;
    persistence_state_ = PersistenceState::Clean;
    preview_state_ = PreviewState::NoPreview;
    latest_snapshot_id_.clear();
    latest_build_id_.clear();
    return true;
}

void ProjectSession::CloseProject() {
    if (lifecycle_state_ == LifecycleState::NoProject) return;
    StopAutosaveTimer();
    build_coordinator_->Cancel();
    // Queued saves snapshot the project being closed: let them land before the
    // state is reset, otherwise the user's last Ctrl+S could be lost.
    save_coordinator_.Flush();
    ProcessApplicationEvents();
    state_.Reset();
    bibliography_db_.Clear();
    bibliography_bibtex_.clear();
    latest_snapshot_id_.clear();
    latest_build_id_.clear();
    lifecycle_state_ = LifecycleState::NoProject;
    persistence_state_ = PersistenceState::Clean;
    preview_state_ = PreviewState::NoPreview;
}

bool ProjectSession::OpenProjectWithRecovery(const std::filesystem::path& project_dir,
                                             std::string* error, bool* recovered) {
    if (recovered) *recovered = false;
    if (!OpenProject(project_dir, error)) return false;
    if (HasRecoverySnapshot()) {
        auto result = RecoverFromAutosave();
        if (result.status == SaveResult::Status::Ok && recovered) {
            *recovered = true;
            // Recovered content is not yet user-saved.
            persistence_state_ = PersistenceState::Dirty;
        }
    }
    return true;
}

bool ProjectSession::HasRecoverySnapshot() const {
    auto autosave_file = paths_.autosave_dir / "autosave.paper";
    if (!std::filesystem::exists(autosave_file)) return false;
    // Newer than project.paper?
    auto autosave_time = std::filesystem::last_write_time(autosave_file);
    if (!std::filesystem::exists(paths_.project_file)) return true;
    auto project_time = std::filesystem::last_write_time(paths_.project_file);
    return autosave_time >= project_time;
}

SaveResult ProjectSession::RecoverFromAutosave() {
    SaveResult fail;
    fail.status = SaveResult::Status::IoError;
    fail.detail = "no recovery snapshot";
    if (lifecycle_state_ != LifecycleState::Open) return fail;
    auto autosave_file = paths_.autosave_dir / "autosave.paper";
    if (!std::filesystem::exists(autosave_file)) return fail;

    LoadRequest request;
    request.project_file = autosave_file;
    auto result = ProjectPersistence::Load(request);
    if (!result.project) {
        fail.detail = result.detail;
        return fail;
    }
    const auto& sp = *result.project;
    state_.SetRevision(sp.revision);
    state_.mutable_document() = sp.document;
    state_.mutable_template() = sp.template_id.empty() ? "generic-article"
                                                       : sp.template_id;
    for (const auto& meta : sp.assets) {
        assets_->registry().Register(meta);
    }
    SaveResult ok;
    ok.status = SaveResult::Status::Ok;
    ok.saved_revision = sp.revision;
    return ok;
}

void ProjectSession::StartAutosaveTimer(std::chrono::milliseconds interval) {
    StopAutosaveTimer();
    autosave_interval_ = interval;
    autosave_stop_.store(false);
    autosave_thread_ = std::thread([this] {
        while (!autosave_stop_.load()) {
            // Sleep in small steps so Stop is responsive.
            for (std::chrono::milliseconds waited{0};
                 waited < autosave_interval_ && !autosave_stop_.load();
                 waited += std::chrono::milliseconds{100}) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (autosave_stop_.load()) break;
            // Only ring the bell. Reading the Document from this thread would
            // race the user's edits; the app thread captures the snapshot.
            PostApplicationEvent(AutosaveTickEvent{});
        }
    });
}

void ProjectSession::StopAutosaveTimer() {
    autosave_stop_.store(true);
    if (autosave_thread_.joinable()) autosave_thread_.join();
}

void ProjectSession::MarkDirty() {
    persistence_state_ = PersistenceState::Dirty;
    preview_state_ = PreviewState::Stale;
}

// ---------------- Editing ----------------

EditResult ProjectSession::Execute(const EditCommand& command) {
    NoteOwnerThreadUse();
    auto result = editing_.Apply(command);
    if (result.status == EditStatus::Applied) {
        MarkDirty();
        RequestBuild(false);
    }
    return result;
}

EditResult ProjectSession::Undo() {
    auto result = editing_.Undo();
    if (result.status == EditStatus::Applied) {
        MarkDirty();
        RequestBuild(false);
    }
    return result;
}

EditResult ProjectSession::Redo() {
    auto result = editing_.Redo();
    if (result.status == EditStatus::Applied) {
        MarkDirty();
        RequestBuild(false);
    }
    return result;
}

// ---------------- Build ----------------

void ProjectSession::RequestBuild(bool manual) {
    NoteOwnerThreadUse();
    if (lifecycle_state_ != LifecycleState::Open) return;
    auto snapshot = snapshot_factory_.CreateBuildSnapshot(state_, bibliography_bibtex_);
    // Remember the identity of this ask on the owning thread; a result can
    // only be accepted if it matches.
    latest_snapshot_id_ = snapshot.snapshot_id;
    latest_build_id_ = snapshot.build_id;
    build_coordinator_->RequestBuild(std::move(snapshot), manual);
}

void ProjectSession::CancelBuild() { build_coordinator_->Cancel(); }

BuildPhase ProjectSession::build_phase() const { return build_coordinator_->phase(); }

// ---------------- Save ----------------

SaveSnapshot ProjectSession::CaptureSaveSnapshot() const {
    // Application thread: deep copy of the mutable state, handed to the worker.
    return snapshot_factory_.CreateSaveSnapshot(state_);
}

SaveResult ProjectSession::Save() {
    NoteOwnerThreadUse();
    SaveResult queued;
    if (lifecycle_state_ != LifecycleState::Open) {
        queued.status = SaveResult::Status::IoError;
        queued.detail = "no project open";
        return queued;
    }
    auto snapshot = CaptureSaveSnapshot();
    queued.status = SaveResult::Status::Queued;
    queued.saved_revision = snapshot.revision;
    queued.save_id = save_coordinator_
                         .Enqueue(std::move(snapshot.serialized),
                                  paths_.project_file, SaveKind::User)
                         .value();
    persistence_state_ = PersistenceState::Saving;
    // Bibliography side-car: one small file, written on the owner thread.
    if (!bibliography_bibtex_.empty()) {
        std::ofstream out(paths_.project_dir / "references.bib",
                          std::ios::binary | std::ios::trunc);
        out << bibliography_bibtex_;
    }
    return queued;
}

SaveResult ProjectSession::Autosave() {
    NoteOwnerThreadUse();
    SaveResult queued;
    if (lifecycle_state_ != LifecycleState::Open) {
        queued.status = SaveResult::Status::IoError;
        queued.detail = "no project open";
        return queued;
    }
    auto snapshot = CaptureSaveSnapshot();
    queued.status = SaveResult::Status::Queued;
    queued.saved_revision = snapshot.revision;
    queued.save_id = save_coordinator_
                         .Enqueue(std::move(snapshot.serialized),
                                  paths_.autosave_dir / "autosave.paper",
                                  SaveKind::Autosave)
                         .value();
    // Autosave does not change Clean/Dirty state (architecture section 32).
    return queued;
}

SaveResult ProjectSession::FlushSaves() {
    save_coordinator_.Flush();
    // Completions are posted as events; apply them so the caller observes the
    // final persistence state.
    ProcessApplicationEvents();
    return last_user_save_result_;
}

void ProjectSession::ApplySaveCompletion(const SaveCompletion& completion) {
    // A save that finished after the project was switched must not speak for
    // the new project.
    if (completion.project_id != state_.id() ||
        lifecycle_state_ != LifecycleState::Open) {
        if (save_result_handler_) {
            save_result_handler_(completion.result, completion.kind);
        }
        return;
    }
    if (completion.kind == SaveKind::User) {
        last_user_save_result_ = completion.result;
        if (completion.result.status == SaveResult::Status::Ok) {
            // Only Clean when nothing changed while the snapshot was being
            // written: save rev20 -> edit rev21 -> save20 finishes must leave
            // the project Dirty.
            if (completion.revision == state_.revision()) {
                persistence_state_ = PersistenceState::Clean;
            }
        } else if (completion.revision == state_.revision()) {
            persistence_state_ = PersistenceState::SaveFailed;
        } else {
            // An older snapshot failed while a newer one is in flight; let the
            // newer save decide.
            if (persistence_state_ != PersistenceState::Saving) {
                persistence_state_ = PersistenceState::SaveFailed;
            }
        }
    }
    if (save_result_handler_) save_result_handler_(completion.result, completion.kind);
}

// ---------------- Template ----------------

void ProjectSession::ChangeTemplate(const std::string& template_id) {
    const auto* def = TemplateRegistry::Instance().Find(template_id);
    if (!def) return;
    std::string old_id = state_.template_selection();
    if (old_id == template_id) return;
    state_.mutable_template() = template_id;
    // Template change: ProjectRevision +1, DocumentVersion unchanged
    // (architecture rule 补充 8). Push a template history action.
    state_.BumpRevision();
    HistoryEntry entry;
    TemplateHistoryAction action;
    action.old_template = old_id;
    action.new_template = template_id;
    entry.action = std::move(action);
    entry.operation_id = OperationId(IdGenerator::NewOperationId());
    entry.resulting_revision = state_.revision();
    editing_.history().Push(std::move(entry));
    MarkDirty();
    RequestBuild(false);
}

// ---------------- Assets ----------------

AssetImportResult ProjectSession::ImportAsset(const std::filesystem::path& source) {
    AssetImportRequest request;
    request.source_path = source;
    request.project_id = state_.id();
    auto result = assets_->Stage(request);
    if (result.status == AssetImportResult::Status::Ok) {
        // Registration happens only when the asset is actually used
        // (architecture 补充 rule 3) - InsertFigureFromSource registers.
    }
    return result;
}

EditResult ProjectSession::InsertFigureFromSource(const std::filesystem::path& source,
                                                  const NodeId& parent,
                                                  std::optional<size_t> index) {
    auto imported = ImportAsset(source);
    if (imported.status != AssetImportResult::Status::Ok) {
        EditResult r;
        r.status = EditStatus::Rejected;
        r.failure = FailureReason::InvalidTarget;
        r.detail = "asset import failed: " + imported.detail;
        return r;
    }
    // Register now that it is being used.
    assets_->Register(assets_->ToCandidate(imported));

    // Re-resolve the anchor against the current revision
    // (architecture 补充 rule 4).
    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = state_.id();
    cmd.base_revision = state_.revision();
    cmd.origin = EditOrigin::User;
    InsertFigurePayload payload;
    payload.parent = parent;
    payload.index = index;
    payload.asset_id = imported.asset_id;
    cmd.payload = payload;
    return Execute(cmd);
}

// ---------------- Bibliography ----------------

BibliographyImportResult ProjectSession::ImportBibliography(
    const std::string& bibtex_text) {
    BibliographyService service(bibliography_db_);
    auto result = service.ImportText(bibtex_text);
    if (result.status == BibliographyImportResult::Status::Ok) {
        bibliography_bibtex_ = bibtex_text;
        bibliography_revision_ = result.bibliography_revision;
        // Bibliography change: ProjectRevision +1 (architecture section 40).
        state_.BumpRevision();
        MarkDirty();
        RequestBuild(false);
    }
    return result;
}

CitationSearchResult ProjectSession::SearchCitations(const std::string& query) {
    CitationSearchRequest request;
    request.query = query;
    BibliographyService service(bibliography_db_);
    return service.Search(request);
}

}  // namespace pf
