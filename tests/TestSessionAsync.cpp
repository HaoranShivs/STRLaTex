// M1 regression tests: single-owner application state + immutable async
// pipeline.
//
// These turn the architecture scenarios that used to live on paper into
// executable constraints. Each one drives a real ProjectSession with a
// compiler whose completion the test controls, so "a build was still running"
// is a fact rather than a hope.
//
//   1. edit while a build is running      -> stale build never reaches preview
//   2. undo while a build is running      -> discarded
//   3. template switch while building     -> old template's PDF discarded
//   4. project switch while building      -> other project's PDF discarded
//   5. edit while a save is running       -> project stays Dirty
//   6. autosave while editing             -> always one complete snapshot
//   7. deleting a referenced figure       -> document valid + dangling diagnostic
//
// Plus the pure preview-gate rules and the typed PreviewUpdate identity.

#include "TestMain.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "build/Compiler.h"
#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include "persistence/ProjectPersistence.h"
#include "project/PreviewGate.h"
#include "project/ProjectSession.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

// ---------------- Compiler with a controllable gate ----------------

// Blocks inside Compile() until the test releases it, which is how a test
// holds a build "in flight" while it keeps editing on the application thread.
// Also polls the coordinator's cancel flag so a cancelled/superseded build
// never wedges a destructor.
class GateCompiler final : public ICompiler {
public:
    CompileResult Compile(const CompileRequest&,
                          const std::atomic<bool>* cancel_requested) override {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        entered_condition_.notify_all();
        while (!released_) {
            if (cancel_requested && cancel_requested->load()) {
                CompileResult cancelled;
                cancelled.status = CompileStatus::Cancelled;
                return cancelled;
            }
            condition_.wait_for(lock, std::chrono::milliseconds{20});
        }
        CompileResult result;
        if (cancel_requested && cancel_requested->load()) {
            result.status = CompileStatus::Cancelled;
            return result;
        }
        result.status = CompileStatus::Success;
        int attempt = 0;
        {
            std::lock_guard<std::mutex> build_lock(builds_mutex_);
            attempt = ++builds_;
        }
        result.pdf_path = std::filesystem::temp_directory_path() /
                          ("pf-async-" + std::to_string(attempt) + ".pdf");
        return result;
    }

    // Let every build through immediately (used while seeding a document).
    void Open() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        entered_ = false;
    }
    // Arm the gate: the next Compile() blocks until Release().
    void Hold() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = false;
        entered_ = false;
    }
    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }
    bool WaitUntilEntered(int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return entered_condition_.wait_for(
            lock, std::chrono::milliseconds{timeout_ms},
            [this] { return entered_; });
    }
    int builds() const {
        std::lock_guard<std::mutex> lock(builds_mutex_);
        return builds_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable entered_condition_;
    bool entered_ = false;
    bool released_ = false;
    mutable std::mutex builds_mutex_;
    int builds_ = 0;
};

// The session owns its compiler, so the rig keeps ownership and lends it out.
class BorrowedCompiler final : public ICompiler {
public:
    explicit BorrowedCompiler(ICompiler* inner) : inner_(inner) {}
    CompileResult Compile(
        const CompileRequest& request,
        const std::atomic<bool>* cancel_requested) override {
        return inner_->Compile(request, cancel_requested);
    }

private:
    ICompiler* inner_;
};

// ---------------- Session rig ----------------

struct SessionRig {
    GateCompiler compiler;
    ProjectSession session;

    SessionRig() : session(MakeConfig(&compiler)) {}

    static ProjectSession::Config MakeConfig(ICompiler* compiler) {
        ProjectSession::Config config;
        config.compiler_factory = [compiler]() -> std::unique_ptr<ICompiler> {
            return std::make_unique<BorrowedCompiler>(compiler);
        };
        config.debounce = std::chrono::milliseconds{0};
        auto root = std::filesystem::temp_directory_path() / "pf-async-workspaces";
        std::filesystem::create_directories(root);
        config.workspace_root = root;
        return config;
    }
};

std::filesystem::path TempDir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

// Application thread: pump the event queue until `done` holds.
bool PumpUntil(ProjectSession& session, const std::function<bool()>& done,
               int timeout_ms = 10000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
    }
    return done();
}

void PumpFor(ProjectSession& session, int ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{5});
    }
}

// Drain until the coordinator is idle and nothing is queued.
void Settle(ProjectSession& session, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
        if (session.build_phase() == BuildPhase::Idle &&
            !session.HasPendingApplicationEvents() &&
            session.pending_saves() == 0) {
            break;
        }
    }
    session.ProcessApplicationEvents();
}

EditCommand TitleCmd(ProjectSession& session, const std::string& title) {
    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = session.state().id();
    cmd.base_revision = session.current_revision();
    cmd.origin = EditOrigin::User;
    SetTitlePayload payload;
    payload.title = InlineFromText(title);
    cmd.payload = payload;
    return cmd;
}

// A document that validates and renders: title + one section + one paragraph.
NodeId SeedDocument(ProjectSession& session) {
    session.Execute(TitleCmd(session, "Seed Title"));
    EditCommand section_cmd;
    section_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    section_cmd.project_id = session.state().id();
    section_cmd.base_revision = session.current_revision();
    InsertSectionPayload section;
    section.index = 0;
    section.title = InlineFromText("Introduction");
    section_cmd.payload = section;
    auto result = session.Execute(section_cmd);

    InsertParagraphPayload para;
    para.parent = result.created_node;
    para.content = InlineFromText("Body text.");
    EditCommand para_cmd;
    para_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    para_cmd.project_id = session.state().id();
    para_cmd.base_revision = session.current_revision();
    para_cmd.payload = para;
    session.Execute(para_cmd);
    return result.created_node;
}

SaveResult SaveAndFlush(ProjectSession& session) {
    session.Save();
    return session.FlushSaves();
}

std::string CurrentTitle(const ProjectSession& session) {
    return InlineToPlainText(session.state().document().front_matter().title);
}

// Const view of a loaded snapshot's title (document() is only const-correct
// through a const reference).
std::string TitleOf(const SerializedProject& project) {
    const Document& doc = project.document;
    return InlineToPlainText(doc.front_matter().title);
}

// Write a 1x1 PNG so the asset manager can stage a real figure.
std::filesystem::path WriteTinyPng() {
    static const unsigned char png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // signature
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR len+type
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  // 1x1
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
        0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
        0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    auto path = std::filesystem::temp_directory_path() / "pf-async-figure.png";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png), sizeof(png));
    return path;
}

}  // namespace

// ================= Preview gate (pure rules) =================

PF_TEST(PreviewGateAcceptsOnlyTheCurrentBuild) {
    BuildResult result;
    result.project_id = ProjectId("p1");
    result.revision = ProjectRevision{10};
    result.snapshot_id = "snap1";
    result.build_id = BuildId("b1");

    PreviewGateInput current;
    current.has_project = true;
    current.project_id = ProjectId("p1");
    current.revision = ProjectRevision{10};
    current.snapshot_id = "snap1";
    current.build_id = BuildId("b1");
    PF_CHECK(EvaluatePreviewGate(current, result) ==
             PreviewGateDecision::Accept);

    PreviewGateInput closed = current;
    closed.has_project = false;
    PF_CHECK(EvaluatePreviewGate(closed, result) ==
             PreviewGateDecision::NoProject);

    // Project identity is checked before revision: switching projects must
    // never let the old project's PDF through, even at the same revision.
    PreviewGateInput other_project = current;
    other_project.project_id = ProjectId("p2");
    PF_CHECK(EvaluatePreviewGate(other_project, result) ==
             PreviewGateDecision::ForeignProject);

    PreviewGateInput newer = current;
    newer.revision = ProjectRevision{11};
    PF_CHECK(EvaluatePreviewGate(newer, result) ==
             PreviewGateDecision::StaleRevision);

    PreviewGateInput newer_snapshot = current;
    newer_snapshot.snapshot_id = "snap2";
    PF_CHECK(EvaluatePreviewGate(newer_snapshot, result) ==
             PreviewGateDecision::StaleSnapshot);

    PreviewGateInput newer_build = current;
    newer_build.build_id = BuildId("b2");
    PF_CHECK(EvaluatePreviewGate(newer_build, result) ==
             PreviewGateDecision::StaleBuild);
}

// ================= Typed preview identity =================

PF_TEST(PreviewUpdateCarriesArtifactIdentity) {
    auto dir = TempDir("pf-async-preview-identity");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    Settle(rig.session);

    std::optional<PreviewUpdate> update;
    rig.session.SetPreviewUpdateHandler(
        [&](const PreviewUpdate& u) { update = u; });
    rig.session.SetBuildResultHandler([&](const BuildResult&) {
        // Handlers only ever run on the owning thread.
        PF_CHECK(rig.session.IsOwnerThread());
    });

    rig.session.RequestBuild(true);
    PF_CHECK(PumpUntil(rig.session, [&] { return update.has_value(); }));

    PF_CHECK(update->success);
    PF_CHECK(update->project_id == rig.session.state().id());
    PF_CHECK(update->build_id == rig.session.latest_build_id());
    PF_CHECK(!update->build_id.empty());
    PF_CHECK(update->revision == rig.session.current_revision());
    PF_CHECK(update->pdf.valid());
    PF_CHECK(update->pdf.build_id == update->build_id);
    PF_CHECK(update->pdf.revision == update->revision);
    PF_CHECK(rig.session.preview_state() == PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= Scenario 1: edit while building =================

PF_TEST(ScenarioEditDuringBuildDropsStalePreview) {
    auto dir = TempDir("pf-async-edit-during-build");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    SaveAndFlush(rig.session);
    Settle(rig.session);

    std::vector<BuildResult> accepted;
    rig.session.SetBuildResultHandler([&](const BuildResult& r) {
        // Invariant: anything that reaches the application layer matched the
        // revision that is current right now.
        PF_CHECK(r.revision == rig.session.current_revision());
        PF_CHECK(r.project_id == rig.session.state().id());
        accepted.push_back(r);
    });

    // rev N: build starts and blocks inside the compiler.
    rig.compiler.Hold();
    rig.session.RequestBuild(true);
    PF_CHECK(rig.compiler.WaitUntilEntered());
    const ProjectRevision building_rev = rig.session.current_revision();

    // The user keeps typing while the compiler runs: rev N+1.
    rig.session.Execute(TitleCmd(rig.session, "Edited While Building"));
    PF_CHECK(rig.session.current_revision() > building_rev);
    PF_CHECK(rig.session.preview_state() != PreviewState::Fresh);

    // The old build now returns. It must not become the preview.
    rig.compiler.Release();
    PF_CHECK(PumpUntil(rig.session, [&] {
        return rig.session.preview_state() == PreviewState::Fresh;
    }));

    for (const auto& r : accepted) {
        PF_CHECK(r.revision != building_rev);  // stale build discarded
    }
    rig.session.ProcessApplicationEvents();
    PF_CHECK(rig.session.owner_thread_violations() == 0);

    // Even a late, hand-delivered result for the old revision is refused by
    // the application-thread gate.
    const PreviewState before = rig.session.preview_state();
    BuildResult stale = accepted.empty() ? BuildResult{} : accepted.front();
    stale.project_id = rig.session.state().id();
    stale.revision = building_rev;
    stale.outcome = BuildResult::Outcome::Success;
    rig.session.PostBuildResult(stale);
    rig.session.ProcessApplicationEvents();
    PF_CHECK(rig.session.preview_state() == before);

    std::filesystem::remove_all(dir);
}

// ================= Scenario 2: undo after a build was requested ==========

PF_TEST(ScenarioUndoDuringBuildDiscardsResult) {
    auto dir = TempDir("pf-async-undo-during-build");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    Settle(rig.session);

    std::vector<BuildResult> accepted;
    rig.session.SetBuildResultHandler([&](const BuildResult& r) {
        PF_CHECK(r.revision == rig.session.current_revision());
        accepted.push_back(r);
    });

    rig.compiler.Hold();
    rig.session.RequestBuild(true);
    PF_CHECK(rig.compiler.WaitUntilEntered());
    const ProjectRevision pre_edit = rig.session.current_revision();

    rig.session.Execute(TitleCmd(rig.session, "Typed Then Undone"));
    const ProjectRevision after_edit = rig.session.current_revision();
    PF_CHECK(after_edit > pre_edit);

    // Undo produces yet another revision; the build for `after_edit` is now
    // behind the document.
    rig.session.Undo();
    const ProjectRevision after_undo = rig.session.current_revision();
    PF_CHECK(after_undo > after_edit);

    rig.compiler.Release();
    PF_CHECK(PumpUntil(rig.session, [&] {
        return rig.session.preview_state() == PreviewState::Fresh;
    }));

    for (const auto& r : accepted) {
        PF_CHECK(r.revision != after_edit);  // the undone revision
        PF_CHECK(r.revision != pre_edit);
    }
    PF_CHECK(rig.session.preview_state() == PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= Scenario 3: template switch while building ============

PF_TEST(ScenarioTemplateSwitchWhileBuildingDropsOldPdf) {
    auto dir = TempDir("pf-async-template-switch");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    Settle(rig.session);

    std::vector<BuildResult> accepted;
    rig.session.SetBuildResultHandler([&](const BuildResult& r) {
        PF_CHECK(r.revision == rig.session.current_revision());
        accepted.push_back(r);
    });

    // IEEE build starts and blocks.
    rig.compiler.Hold();
    rig.session.ChangeTemplate("ieee-conference");
    PF_CHECK(rig.session.state().template_selection() == "ieee-conference");
    PF_CHECK(rig.compiler.WaitUntilEntered());
    const ProjectRevision ieee_rev = rig.session.current_revision();

    // The user switches back to the generic template.
    rig.session.ChangeTemplate("generic-article");
    PF_CHECK(rig.session.state().template_selection() == "generic-article");
    const ProjectRevision generic_rev = rig.session.current_revision();
    PF_CHECK(generic_rev > ieee_rev);

    rig.compiler.Release();
    PF_CHECK(PumpUntil(rig.session, [&] {
        return rig.session.preview_state() == PreviewState::Fresh;
    }));

    for (const auto& r : accepted) {
        PF_CHECK(r.revision != ieee_rev);  // old template's PDF never showed
    }
    // The preview that did land belongs to the generic template's revision.
    PF_CHECK(rig.session.preview_state() == PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= Scenario 4: project switch while building ============

PF_TEST(ScenarioProjectSwitchDiscardsOtherProjectBuild) {
    auto dir_a = TempDir("pf-async-project-a");
    auto dir_b = TempDir("pf-async-project-b");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir_a);
    SeedDocument(rig.session);
    Settle(rig.session);
    const ProjectId project_a = rig.session.state().id();

    std::vector<BuildResult> accepted;
    rig.session.SetBuildResultHandler([&](const BuildResult& r) {
        // A result may only ever speak for the project that is open now.
        PF_CHECK(r.project_id == rig.session.state().id());
        accepted.push_back(r);
    });

    rig.compiler.Hold();
    rig.session.RequestBuild(true);
    PF_CHECK(rig.compiler.WaitUntilEntered());

    // Switch to another project while A's build is in flight.
    rig.session.NewProject(dir_b);
    const ProjectId project_b = rig.session.state().id();
    PF_CHECK(!(project_a == project_b));
    accepted.clear();

    rig.compiler.Release();
    PumpFor(rig.session, 500);
    rig.session.ProcessApplicationEvents();

    // Nothing from project A may enter project B's preview: a revision check
    // alone would not catch this, the project id must be part of the gate.
    PF_CHECK(accepted.empty());
    PF_CHECK(rig.session.preview_state() != PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir_a);
    std::filesystem::remove_all(dir_b);
}

// ================= Scenario 5: edit while saving =================

PF_TEST(ScenarioEditDuringSaveStaysDirty) {
    auto dir = TempDir("pf-async-edit-during-save");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    Settle(rig.session);

    PF_CHECK(SaveAndFlush(rig.session).status == SaveResult::Status::Ok);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Clean);
    const std::string saved_title = CurrentTitle(rig.session);

    // Save rev20 (queued, written by the worker)...
    auto queued = rig.session.Save();
    PF_CHECK(queued.status == SaveResult::Status::Queued);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Saving);
    const ProjectRevision save_rev = queued.saved_revision;

    // ...the user keeps typing before the write completes: rev21.
    rig.session.Execute(TitleCmd(rig.session, "Edited During Save"));
    const ProjectRevision edit_rev = rig.session.current_revision();
    PF_CHECK(edit_rev > save_rev);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Dirty);

    // The save of rev20 finishes. The project must stay Dirty: the written
    // snapshot is not the current state.
    PF_CHECK(rig.session.FlushSaves().status == SaveResult::Status::Ok);
    PF_CHECK(rig.session.current_revision() == edit_rev);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Dirty);

    // What landed on disk is the rev20 snapshot, not the edited document.
    LoadRequest request;
    request.project_file = dir / "project.paper";
    auto loaded = ProjectPersistence::Load(request);
    PF_CHECK(loaded.project.has_value());
    if (loaded.project) {
        PF_CHECK(loaded.project->revision == save_rev);
        PF_CHECK(TitleOf(*loaded.project) == saved_title);
    }

    // Saving again from the same revision clears Dirty.
    PF_CHECK(SaveAndFlush(rig.session).status == SaveResult::Status::Ok);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}

// ================= Scenario 6: autosave during editing =================

PF_TEST(ScenarioAutosaveWritesCompleteSnapshotWhileEditing) {
    auto dir = TempDir("pf-async-autosave-editing");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    PF_CHECK(SaveAndFlush(rig.session).status == SaveResult::Status::Ok);
    Settle(rig.session);

    // Every state the document passes through, keyed by revision. The
    // autosave file must match one of these pairs exactly - never a mixture.
    std::map<std::uint64_t, std::string> revision_title;
    revision_title[rig.session.current_revision().value] =
        CurrentTitle(rig.session);

    int autosaves = 0;
    rig.session.SetSaveResultHandler(
        [&](const SaveResult& result, SaveKind kind) {
            PF_CHECK(rig.session.IsOwnerThread());
            if (kind == SaveKind::Autosave &&
                result.status == SaveResult::Status::Ok) {
                ++autosaves;
            }
        });

    rig.session.StartAutosaveTimer(std::chrono::milliseconds{40});
    for (int i = 0; i < 20; ++i) {
        const std::string title = "Edit " + std::to_string(i);
        rig.session.Execute(TitleCmd(rig.session, title));
        revision_title[rig.session.current_revision().value] = title;
        // Keep typing while the timer fires.
        PumpFor(rig.session, 25);
    }
    rig.session.StopAutosaveTimer();
    // A tick may have been posted just before the timer stopped; drain twice.
    rig.session.FlushSaves();
    rig.session.ProcessApplicationEvents();
    rig.session.FlushSaves();

    PF_CHECK(autosaves > 0);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Dirty);

    LoadRequest request;
    request.project_file = dir / ".paperforge" / "autosave" / "autosave.paper";
    auto loaded = ProjectPersistence::Load(request);
    PF_CHECK(loaded.project.has_value());
    if (loaded.project) {
        // The snapshot is internally consistent: the revision it claims is
        // exactly the revision whose content it holds.
        const auto it = revision_title.find(loaded.project->revision.value);
        PF_CHECK(it != revision_title.end());
        if (it != revision_title.end()) {
            PF_CHECK(TitleOf(*loaded.project) == it->second);
        }
    }
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= Scenario 7: delete a referenced figure =================

PF_TEST(ScenarioDeleteReferencedFigureEmitsDanglingDiagnostic) {
    auto dir = TempDir("pf-async-dangling-xref");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    const NodeId section = SeedDocument(rig.session);
    Settle(rig.session);

    const auto png = WriteTinyPng();
    auto figure = rig.session.InsertFigureFromSource(png, section);
    PF_CHECK(figure.status == EditStatus::Applied);
    const NodeId figure_id = figure.created_node;
    PF_CHECK(!figure_id.empty());

    // A paragraph that references the figure.
    InsertParagraphPayload para;
    para.parent = section;
    para.content = InlineFromText("See the figure for details.");
    EditCommand para_cmd;
    para_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    para_cmd.project_id = rig.session.state().id();
    para_cmd.base_revision = rig.session.current_revision();
    para_cmd.payload = para;
    auto para_result = rig.session.Execute(para_cmd);
    PF_CHECK(para_result.status == EditStatus::Applied);

    EditCommand xref_cmd;
    xref_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    xref_cmd.project_id = rig.session.state().id();
    xref_cmd.base_revision = rig.session.current_revision();
    InsertCrossReferencePayload xref;
    xref.paragraph = para_result.created_node;
    xref.target = figure_id;
    xref_cmd.payload = xref;
    PF_CHECK(rig.session.Execute(xref_cmd).status == EditStatus::Applied);

    // The reference resolves before the delete.
    const Document& before = rig.session.state().document();
    PF_CHECK(before.ContainsNode(figure_id));

    // Delete the figure the paragraph points at.
    EditCommand del_cmd;
    del_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    del_cmd.project_id = rig.session.state().id();
    del_cmd.base_revision = rig.session.current_revision();
    DeleteBlockPayload del;
    del.node = figure_id;
    del_cmd.payload = del;
    PF_CHECK(rig.session.Execute(del_cmd).status == EditStatus::Applied);

    // The document stays structurally valid ...
    const Document& doc = rig.session.state().document();
    PF_CHECK(!doc.ContainsNode(figure_id));
    for (const auto& id : doc.CollectNodeIds()) {
        PF_CHECK(!(id == figure_id));
    }
    // ... and the dangling reference surfaces as a diagnostic, not a crash.
    ValidationInput input;
    input.document = &doc;
    input.template_id = rig.session.state().template_selection();
    input.revision = rig.session.current_revision();
    input.snapshot_id = "snap-dangling";
    auto validation = Validator().Validate(input);
    bool found = false;
    for (const auto& d : validation.diagnostics) {
        if (d.code == "E-MISSING-XREF-TARGET") found = true;
    }
    PF_CHECK(found);
    PF_CHECK(validation.can_render);
    std::filesystem::remove(png);
    std::filesystem::remove_all(dir);
}
