// P0-01 regression tests: unsaved-changes protection and the autosave
// recovery closure.
//
// The release blockers under test:
//   * a brand-new project that was never saved but WAS autosaved must be
//     recoverable - the old OpenProjectWithRecovery() required project.paper
//     to exist first, so that work was unreachable and lost;
//   * recovery must prefer the newer of project.paper / autosave.paper;
//   * EnsureDirectories failures must not let a project enter Open.
#include "TestMain.hpp"
#include "ScopedTempDir.hpp"

#include <fstream>
#include <memory>
#include <string>

#include "build/Compiler.h"
#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include "persistence/ProjectPersistence.h"
#include "project/ProjectSession.h"

using namespace pf;

namespace {

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

ProjectSession::Config MakeConfig() {
    ProjectSession::Config config;
    config.compiler_factory = []() -> std::unique_ptr<ICompiler> {
        return std::make_unique<NullCompiler>();
    };
    config.debounce = std::chrono::milliseconds{0};
    // Unique per test process: `ctest -j` runs several binaries at once and a
    // fixed workspace root made them clobber each other.
    static pf::test::ScopedTempDir workspace("pf-p01-workspaces");
    config.workspace_root = workspace.path();
    return config;
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

std::string CurrentTitle(const ProjectSession& session) {
    return InlineToPlainText(session.state().document().front_matter().title);
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

} // namespace

// Case B of the rectification plan: the project was never saved, only the
// autosave holds the work. Recovery must produce an open, Dirty project with
// the recovered content - not NotFound.
PF_TEST(RecoveryFromAutosaveOnlyNewProject) {
    auto dir = TempDir("pf-p01-autosave-only");
    ProjectSession writer(MakeConfig());
    PF_CHECK(writer.NewProject(dir));
    writer.Execute(TitleCmd(writer, "Unsaved Draft"));
    // Autosave writes only the snapshot; project.paper is never created.
    auto autosave = writer.Autosave();
    PF_CHECK(autosave.status == SaveResult::Status::Queued);
    writer.FlushSaves();
    PF_CHECK(!std::filesystem::exists(dir / "project.paper"));
    PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" /
                                     "autosave.paper"));

    // A fresh session opens the directory with recovery.
    ProjectSession reader(MakeConfig());
    std::string error;
    bool recovered = false;
    const bool ok = reader.OpenProjectWithRecovery(dir, &error, &recovered);
    PF_CHECK(ok);
    if (!ok) {
        std::cout << "    error: " << error << "\n";
        std::filesystem::remove_all(dir);
        return;
    }
    PF_CHECK(recovered);
    PF_CHECK(reader.lifecycle_state() == LifecycleState::Open);
    PF_CHECK(reader.persistence_state() == PersistenceState::Dirty);
    PF_CHECK(CurrentTitle(reader) == "Unsaved Draft");
    std::filesystem::remove_all(dir);
}

// Case C: neither file exists -> the open must fail, and the failure must be
// reported rather than leaving a half-open project.
PF_TEST(RecoveryFailsWhenNeitherFileExists) {
    auto dir = TempDir("pf-p01-nothing");
    std::filesystem::create_directories(dir);
    ProjectSession session(MakeConfig());
    std::string error;
    bool recovered = true;
    const bool ok = session.OpenProjectWithRecovery(dir, &error, &recovered);
    PF_CHECK(!ok);
    PF_CHECK(!error.empty());
    PF_CHECK(!recovered);
    PF_CHECK(session.lifecycle_state() == LifecycleState::NoProject);
    std::filesystem::remove_all(dir);
}

// Case A: project.paper exists and the autosave is newer -> recovery wins.
PF_TEST(RecoveryPrefersNewerAutosave) {
    auto dir = TempDir("pf-p01-newer-autosave");
    ProjectSession writer(MakeConfig());
    PF_CHECK(writer.NewProject(dir));
    writer.Execute(TitleCmd(writer, "Saved Version"));
    PF_CHECK(writer.Save().status == SaveResult::Status::Queued);
    writer.FlushSaves();
    PF_CHECK(writer.persistence_state() == PersistenceState::Clean);

    // Edit and autosave: the autosave is now the newer snapshot.
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    writer.Execute(TitleCmd(writer, "Newer Autosaved Version"));
    PF_CHECK(writer.Autosave().status == SaveResult::Status::Queued);
    writer.FlushSaves();

    ProjectSession reader(MakeConfig());
    std::string error;
    bool recovered = false;
    PF_CHECK(reader.OpenProjectWithRecovery(dir, &error, &recovered));
    PF_CHECK(recovered);
    PF_CHECK(CurrentTitle(reader) == "Newer Autosaved Version");
    PF_CHECK(reader.persistence_state() == PersistenceState::Dirty);
    std::filesystem::remove_all(dir);
}

// Case A (no recovery needed): project.paper is newer than the autosave, so
// the on-disk save wins and the project opens Clean.
PF_TEST(RecoveryKeepsProjectFileWhenItIsNewer) {
    auto dir = TempDir("pf-p01-newer-project");
    ProjectSession writer(MakeConfig());
    PF_CHECK(writer.NewProject(dir));
    writer.Execute(TitleCmd(writer, "Old Autosave"));
    PF_CHECK(writer.Autosave().status == SaveResult::Status::Queued);
    writer.FlushSaves();

    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    writer.Execute(TitleCmd(writer, "Authoritative Save"));
    PF_CHECK(writer.Save().status == SaveResult::Status::Queued);
    writer.FlushSaves();

    ProjectSession reader(MakeConfig());
    std::string error;
    bool recovered = true;
    PF_CHECK(reader.OpenProjectWithRecovery(dir, &error, &recovered));
    PF_CHECK(!recovered);
    PF_CHECK(CurrentTitle(reader) == "Authoritative Save");
    PF_CHECK(reader.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}

// P0-03 closure: a directory that cannot be created must fail the project
// open instead of entering LifecycleState::Open with a broken layout.
PF_TEST(NewProjectFailsWhenDirectoryCannotBeCreated) {
    auto base = TempDir("pf-p01-bad-dir");
    std::filesystem::create_directories(base);
    // A regular FILE where the project directory is expected: every
    // create_directories under it must fail.
    auto blocker = base / "blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory";
    }
    ProjectSession session(MakeConfig());
    std::string error;
    const bool ok = session.NewProject(blocker, &error);
    PF_CHECK(!ok);
    PF_CHECK(!error.empty());
    PF_CHECK(session.lifecycle_state() == LifecycleState::NoProject);
    std::filesystem::remove_all(base);
}

// A dirty project must be recoverable through the SAME guard the UI uses;
// this asserts the state the guard branches on is observable and correct.
PF_TEST(DirtyStateIsObservableForTheNavigationGuard) {
    auto dir = TempDir("pf-p01-dirty-state");
    ProjectSession session(MakeConfig());
    PF_CHECK(session.NewProject(dir));
    // A new project starts Dirty: the guard must ask before discarding it.
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    session.Execute(TitleCmd(session, "T"));
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    // Saving clears it, which is what lets the guard proceed silently.
    PF_CHECK(session.Save().status == SaveResult::Status::Queued);
    session.FlushSaves();
    PF_CHECK(session.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}