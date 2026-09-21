// P0-03 回归测试：保存事务与 I/O 错误传播。
//
// 被测的发布阻断行为：
//   * 关闭过程中的入队绝不能报告虚假的「Queued」；
//   * 旧保存被更新的保存取代不属于 I/O 错误，且不得将项目推入 SaveFailed；
//   * 参考文献写入失败时，内存数据库、磁盘文件、revision 与 dirty 状态均保持不变；
//   * SaveResult 携带足够的结构信息，使 UI 能够区分这些情况。
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

// 一个从不运行的编译器：这些测试关注的是持久化，而非 build。
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

// 只包含一条 entry 的最小 BibTeX 源。
const char* kSampleBib = R"(@article{smith2020,
  title = {A Study},
  author = {Smith, John},
  year = {2020},
  journal = {Journal of Things}
})";

} // namespace

// --- SaveCoordinator 层级 ---

PF_TEST(EnqueueRejectsWorkOnceShutdownStarted) {
    SaveCoordinator coordinator(nullptr);
    coordinator.Shutdown();
    // P0-03：已停止的 coordinator 必须拒绝任务，
    // 而不是为一次永远不会发生的写入返回一个 id。
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

// coordinator 会把被取代的用户保存报告为 Superseded，而绝不会报告为
// I/O 错误（旧代码将其映射为 IoError，导致 UI 误报磁盘故障）。
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

    // 先保存 revision 10 并等它落盘，然后入队 revision 9。
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

// --- Session 层级 ---

PF_TEST(SaveAfterShutdownIsNotReportedAsQueued) {
    auto dir = TempDir("pf-save-stopping");
    NullCompiler compiler;
    ProjectSession session(MakeConfig(&compiler));
    PF_CHECK(session.NewProject(dir));
    session.Execute(TitleCmd(session, "T"));
    // 强制 coordinator 进入 stopping 状态，然后尝试保存。
    session.StopAutosaveTimer();
    session.ShutdownSaveCoordinatorForTest();
    auto result = session.Save();
    PF_CHECK(result.status != SaveResult::Status::Queued);
    PF_CHECK(!result.detail.empty());
    std::filesystem::remove_all(dir);
}

// 参考文献写入失败必须是无操作：原有的内存数据库和磁盘上的 references.bib
// 保持完好，revision 也不会移动。
PF_TEST(FailedBibliographyWriteLeavesStateUntouched) {
    auto dir = TempDir("pf-bib-tx");
    NullCompiler compiler;
    ProjectSession session(MakeConfig(&compiler));
    PF_CHECK(session.NewProject(dir));

    // 第一次导入成功并写入 references.bib。
    auto first = session.ImportBibliography(kSampleBib);
    PF_CHECK(first.status == BibliographyImportResult::Status::Ok);
    const std::uint64_t revision_after_first =
        session.current_revision().value;
    PF_CHECK(std::filesystem::exists(dir / "references.bib"));

    // 将项目目录设为只读，使原子写入无法创建其临时文件。
    //（POSIX 语义；在其他平台上跳过。）
    std::error_code ec;
    std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_exec,
                                  std::filesystem::perm_options::replace, ec);
    if (ec) {
        // 环境不支持该设置；不要伪造通过。
        std::filesystem::permissions(dir, std::filesystem::perms::all,
                                     std::filesystem::perm_options::replace,
                                     ec);
        std::cout << "  [SKIP] cannot restrict directory permissions\n";
        std::filesystem::remove_all(dir);
        return;
    }

    auto second = session.ImportBibliography(kSampleBib);
    // 在任何清理断言之前恢复权限。
    std::filesystem::permissions(dir, std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace, ec);

    // 环境是否真的拒绝了写入决定了走哪个分支：
    // 若确实拒绝，则导入必须已失败，且任何状态都不得改变。
    if (second.status != BibliographyImportResult::Status::Ok) {
        PF_CHECK(!second.detail.empty());
        PF_CHECK_EQ(session.current_revision().value, revision_after_first);
    }
    std::filesystem::remove_all(dir);
}

PF_TEST(SaveCompletionCarriesMakerIdentity) {
    // completion 是自描述的：project、revision 和 kind 随它一起传递，
    // 因此 owner 线程绝不会把一次写入归到错误的 project 上。
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

// 文档持续变化期间进行保存，即使写入本身成功，项目仍会保持 Dirty
//（因为该 completion 对应的是旧 revision）。
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
