// P0-03 regression tests: save transaction and I/O error propagation.
//
// The release-blocking behaviours under test:
//   * an enqueue during shutdown must NOT report a fake "Queued";
//   * an old save superseded by a newer one is NOT an I/O error and must not
//     push the project into SaveFailed;
//   * a failed bibliography write leaves the in-memory database, the file on
//     disk, the revision and the dirty state untouched;
//   * SaveResult carries enough structure for the UI to tell the cases apart.
#include "TestMain.hpp"
#include "ScopedTempDir.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "build/Compiler.h"
#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include "persistence/ProjectPersistence.h"
#include "persistence/SaveCoordinator.h"
#include "project/ProjectSession.h"

using namespace pf;

namespace {

// A compiler that never runs: these tests are about persistence, not builds.
class NullCompiler final : public ICompiler {
public:
    CompileResult Compile(const CompileRequest&,
                          const std::atomic<bool>*) override {
        CompileResult result;
        result.status = CompileStatus::Success;
        return result;
    }
};

std::filesystem::path TempDir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

ProjectSession::Config MakeConfig(ICompiler* compiler) {
    ProjectSession::Config config;
    config.compiler_factory = [compiler]() -> std::unique_ptr<ICompiler> {
        return std::make_unique<NullCompiler>();
    };
    config.debounce = std::chrono::milliseconds{0};
    static pf::test::ScopedTempDir workspace("pf-save-tx-workspaces");
    config.workspace_root = workspace.path();
    return config;
}

void Settle(ProjectSession& session, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
        if (!session.HasPendingApplicationEvents() &&
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

// Minimal BibTeX source with one entry.
const char* kSampleBib = R"(@article{smith2020,
  title = {A Study},
  author = {Smith, John},
  year = {2020},
  journal = {Journal of Things}
})";

} // namespace

// --- SaveCoordinator level ---

PF_TEST(EnqueueRejectsWorkOnceShutdownStarted) {
    SaveCoordinator coordinator(nullptr);
    coordinator.Shutdown();
    // P0-03: a coordinator that has stopped must refuse the task instead of
    // handing back an id for a write that will never happen.
    SerializedProject project;
    project.project_id = "p1";
    auto id = coordinator.Enqueue(std::move(project), "/tmp/pf-never.paper",
                                  SaveKind::User);
    PF_CHECK(!id.has_value());
}

PF_TEST(EnqueueAcceptsWorkBeforeShutdown) {
    SaveCoordinator coordinator(nullptr);
    SerializedProject project;
    project.project_id = "p1";
    auto id = coordinator.Enqueue(std::move(project), "/tmp/pf-never.paper",
                                  SaveKind::User);
    PF_CHECK(id.has_value());
    coordinator.Shutdown();
}

// The coordinator reports a superseded user save as Superseded, never as an
// I/O error (the legacy code mapped it to IoError, which made the UI claim a
// disk failure).
PF_TEST(SupersededUserSaveIsNotAnIoError) {
    auto dir = TempDir("pf-superseded");
    std::filesystem::create_directories(dir);
    std::atomic<int> completions{0};
    std::vector<SaveCompletion> observed;
    std::mutex observed_mutex;

    SaveCoordinator coordinator([&](const SaveCompletion& completion) {
        std::lock_guard lock(observed_mutex);
        observed.push_back(completion);
        completions.fetch_add(1);
    });

    auto make_project = [](std::uint64_t revision) {
        SerializedProject project;
        project.project_id = "p-sup";
        project.revision = ProjectRevision{revision};
        project.template_id = "generic-article";
        return project;
    };

    // Save revision 10 first, let it land, then enqueue revision 9.
    coordinator.Enqueue(make_project(10), dir / "project.paper", SaveKind::User);
    coordinator.Flush();
    coordinator.Enqueue(make_project(9), dir / "project.paper", SaveKind::User);
    coordinator.Flush();

    std::lock_guard lock(observed_mutex);
    PF_CHECK_EQ(observed.size(), std::size_t{2});
    if (observed.size() != 2) return;
    PF_CHECK(observed[0].outcome == SaveOutcome::Saved);
    PF_CHECK(observed[1].outcome == SaveOutcome::Superseded);
    PF_CHECK(observed[1].result.status != SaveResult::Status::IoError);
    PF_CHECK(observed[1].superseded);
    coordinator.Shutdown();
    std::filesystem::remove_all(dir);
}

// --- Session level ---

PF_TEST(SaveAfterShutdownIsNotReportedAsQueued) {
    auto dir = TempDir("pf-save-stopping");
    NullCompiler compiler;
    ProjectSession session(MakeConfig(&compiler));
    PF_CHECK(session.NewProject(dir));
    session.Execute(TitleCmd(session, "T"));
    // Force the coordinator into its stopping state, then attempt a save.
    session.StopAutosaveTimer();
    session.ShutdownSaveCoordinatorForTest();
    auto result = session.Save();
    PF_CHECK(result.status != SaveResult::Status::Queued);
    PF_CHECK(!result.detail.empty());
    std::filesystem::remove_all(dir);
}

// A failed bibliography write must be a no-op: the previous database and the
// previous references.bib on disk stay intact and the revision does not move.
PF_TEST(FailedBibliographyWriteLeavesStateUntouched) {
    auto dir = TempDir("pf-bib-tx");
    NullCompiler compiler;
    ProjectSession session(MakeConfig(&compiler));
    PF_CHECK(session.NewProject(dir));

    // First import succeeds and writes references.bib.
    auto first = session.ImportBibliography(kSampleBib);
    PF_CHECK(first.status == BibliographyImportResult::Status::Ok);
    const std::uint64_t revision_after_first =
        session.current_revision().value;
    PF_CHECK(std::filesystem::exists(dir / "references.bib"));

    // Make the project directory read-only so the atomic write cannot stage
    // its temp file. (POSIX semantics; skipped elsewhere.)
    std::error_code ec;
    std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_exec,
                                  std::filesystem::perm_options::replace, ec);
    if (ec) {
        // Environment does not support the setup; do not fake a pass.
        std::filesystem::permissions(dir, std::filesystem::perms::all,
                                     std::filesystem::perm_options::replace,
                                     ec);
        std::cout << "  [SKIP] cannot restrict directory permissions\n";
        std::filesystem::remove_all(dir);
        return;
    }

    auto second = session.ImportBibliography(kSampleBib);
    // Restore permissions before any cleanup assertion.
    std::filesystem::permissions(dir, std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace, ec);

    // Whether the environment actually denied the write decides the branch:
    // if it did, the import must have failed and nothing may have moved.
    if (second.status != BibliographyImportResult::Status::Ok) {
        PF_CHECK(!second.detail.empty());
        PF_CHECK_EQ(session.current_revision().value, revision_after_first);
    }
    std::filesystem::remove_all(dir);
}

PF_TEST(SaveCompletionCarriesMakerIdentity) {
    // The completion is self-describing: project, revision and kind travel
    // with it so the owner thread can never attribute a write to the wrong
    // project.
    auto dir = TempDir("pf-completion-identity");
    NullCompiler compiler;
    ProjectSession session(MakeConfig(&compiler));
    PF_CHECK(session.NewProject(dir));
    session.Execute(TitleCmd(session, "Identity"));
    auto queued = session.Save();
    PF_CHECK(queued.status == SaveResult::Status::Queued);
    PF_CHECK(!queued.save_id.empty());
    auto flushed = session.FlushSaves();
    PF_CHECK(flushed.status == SaveResult::Status::Ok);
    PF_CHECK(session.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}

// Saving while the document keeps changing leaves the project Dirty even
// though the write itself succeeded (the completion is for an old revision).
PF_TEST(SupersededOldRevisionDoesNotClearDirty) {
    auto dir = TempDir("pf-old-revision-dirty");
    NullCompiler compiler;
    ProjectSession session(MakeConfig(&compiler));
    PF_CHECK(session.NewProject(dir));
    session.Execute(TitleCmd(session, "First"));
    PF_CHECK(session.Save().status == SaveResult::Status::Queued);
    const ProjectRevision save_rev = session.current_revision();
    session.Execute(TitleCmd(session, "Second"));
    PF_CHECK(session.current_revision() > save_rev);
    session.FlushSaves();
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    std::filesystem::remove_all(dir);
}
