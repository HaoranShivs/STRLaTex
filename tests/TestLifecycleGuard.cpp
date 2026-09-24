// P0-01 回归测试：未保存更改的保护与自动保存恢复闭环。
//
// 被测的发布阻塞项：
//   * 一个从未保存但确实执行过自动保存的全新项目必须可恢复——旧的 OpenProjectWithRecovery() 要求
//     project.paper 先存在，导致这部分工作无法触达并丢失；
//   * 恢复必须优先选择 project.paper / autosave.paper 中较新的一个；
//   * EnsureDirectories 失败时不得让项目进入 Open。
#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

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
    CompileResult Compile(const CompileRequest&, const std::atomic<bool>*) override {
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
    config.compiler_factory = []() -> std::unique_ptr<ICompiler> { return std::make_unique<NullCompiler>(); };
    config.debounce = std::chrono::milliseconds{0};
    // 每个测试进程独享：`ctest -j` 会同时运行多个二进制，固定的 workspace 根目录
    // 会让它们互相覆盖。
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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
        if (!session.HasPendingApplicationEvents() && session.pending_saves() == 0) {
            break;
        }
    }
    session.ProcessApplicationEvents();
}

} // namespace

// 整改方案的 Case B：项目从未保存，只有自动保存持有这些工作。恢复必须产生一个
// 已打开的、Dirty 的项目，并带有恢复出的内容——而不是 NotFound。
PF_TEST(RecoveryFromAutosaveOnlyNewProject) {
    auto dir = TempDir("pf-p01-autosave-only");
    ProjectSession writer(MakeConfig());
    PF_CHECK(writer.NewProject(dir));
    writer.Execute(TitleCmd(writer, "Unsaved Draft"));
    // 自动保存只写入 snapshot；project.paper 永远不会被创建。
    auto autosave = writer.Autosave();
    PF_CHECK(autosave.status == SaveResult::Status::Queued);
    writer.FlushSaves();
    PF_CHECK(!std::filesystem::exists(dir / "project.paper"));
    PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" / "autosave.paper"));

    // 一个新会话以恢复方式打开该目录。
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

// Case C：两个文件都不存在 -> 打开必须失败，且必须报告该失败，而不是留下一个
// 半打开的项目。
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

// Case A：project.paper 存在且自动保存更新 -> 恢复胜出。
PF_TEST(RecoveryPrefersNewerAutosave) {
    auto dir = TempDir("pf-p01-newer-autosave");
    ProjectSession writer(MakeConfig());
    PF_CHECK(writer.NewProject(dir));
    writer.Execute(TitleCmd(writer, "Saved Version"));
    PF_CHECK(writer.Save().status == SaveResult::Status::Queued);
    writer.FlushSaves();
    PF_CHECK(writer.persistence_state() == PersistenceState::Clean);

    // 编辑并自动保存：此时自动保存是更新的 snapshot。
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

// Case A（无需恢复）：project.paper 比自动保存更新，因此磁盘上的保存胜出，
// 项目以 Clean 打开。
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

// P0-03 闭环：无法创建的目录必须使项目打开失败，而不是带着损坏的目录布局
// 进入 LifecycleState::Open。
PF_TEST(NewProjectFailsWhenDirectoryCannotBeCreated) {
    auto base = TempDir("pf-p01-bad-dir");
    std::filesystem::create_directories(base);
    // 在本应是项目目录的位置放一个普通文件：其下所有 create_directories
    // 都必须失败。
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

// Dirty 项目必须能通过 UI 所用的同一个 guard 恢复；这里断言 guard 据以分支的
// 状态可观测且正确。
PF_TEST(DirtyStateIsObservableForTheNavigationGuard) {
    auto dir = TempDir("pf-p01-dirty-state");
    ProjectSession session(MakeConfig());
    PF_CHECK(session.NewProject(dir));
    // 新项目初始为 Dirty：guard 在丢弃它之前必须先询问。
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    session.Execute(TitleCmd(session, "T"));
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    // 保存会清除该状态，这正是 guard 能够静默继续的原因。
    PF_CHECK(session.Save().status == SaveResult::Status::Queued);
    session.FlushSaves();
    PF_CHECK(session.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}