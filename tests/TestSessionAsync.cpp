// M1 回归测试：单属主应用状态 + 不可变异步流水线。
//
// 这些测试把过去只停留在纸面上的架构场景变成可执行的约束。每个测试都驱动
// 一个真实的 ProjectSession，并使用一个其完成时机由测试控制的 compiler，
// 因此「build 仍在运行」是事实，而不是期望。
//
//   1. 在 build 运行时编辑          -> 陈旧的 build 永不进入预览
//   2. 在 build 运行时 undo         -> 被丢弃
//   3. 构建期间切换模板             -> 旧模板的 PDF 被丢弃
//   4. 构建期间切换项目             -> 其他项目的 PDF 被丢弃
//   5. 在 save 运行时编辑           -> 项目保持 Dirty
//   6. 编辑期间 autosave            -> 始终是一个完整 snapshot
//   7. 删除被引用的图               -> document 合法 + 悬空引用 diagnostic
//
// 此外还有纯 preview-gate 规则与带类型的 PreviewUpdate 标识。

#include "ScopedTempDir.hpp"
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

// ---------------- 带有可控 gate 的 Compiler ----------------

// 在 Compile() 内阻塞，直到测试将其释放；测试正是借此让 build 保持「在途」，
// 同时在应用线程上继续编辑。还会轮询协调器的 cancel 标志，
// 使被取消或被取代的 build 永远不会卡住析构过程。
class GateCompiler final : public ICompiler {
  public:
    CompileResult Compile(const CompileRequest&, const std::atomic<bool>* cancel_requested) override {
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
        result.pdf_path = std::filesystem::temp_directory_path() / ("pf-async-" + std::to_string(attempt) + ".pdf");
        return result;
    }

    // 立即放行每个 build（在填充 document 时使用）。
    void Open() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        entered_ = false;
    }
    // 布防 gate：下一次 Compile() 会阻塞到 Release() 为止。
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
        return entered_condition_.wait_for(lock, std::chrono::milliseconds{timeout_ms}, [this] { return entered_; });
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

// session 拥有自己的 compiler，因此测试装置保留所有权并将其借出。
class BorrowedCompiler final : public ICompiler {
  public:
    explicit BorrowedCompiler(ICompiler* inner) : inner_(inner) {}
    CompileResult Compile(const CompileRequest& request, const std::atomic<bool>* cancel_requested) override {
        return inner_->Compile(request, cancel_requested);
    }

  private:
    ICompiler* inner_;
};

// ---------------- Session 测试装置 ----------------

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
        // E-08：固定路径会在并发运行的 GUI 测试二进制之间发生冲突，
        // 残留文件还可能掩盖失败。
        static pf::test::ScopedTempDir workspace("pf-async-workspaces");
        config.workspace_root = workspace.path();
        return config;
    }
};

std::filesystem::path TempDir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

// 应用线程：泵送事件队列，直到 `done` 成立。
bool PumpUntil(ProjectSession& session, const std::function<bool()>& done, int timeout_ms = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
    }
    return done();
}

void PumpFor(ProjectSession& session, int ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{5});
    }
}

// 持续排空，直到协调器空闲且没有排队事项。
void Settle(ProjectSession& session, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
        if (session.build_phase() == BuildPhase::Idle && !session.HasPendingApplicationEvents() &&
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

// 一个可通过校验并可渲染的 document：标题 + 一个小节 + 一个段落。
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

// 已加载 snapshot 标题的 const 视图（document() 只有通过 const 引用
// 才符合 const 正确性）。
std::string TitleOf(const SerializedProject& project) {
    const Document& doc = project.document;
    return InlineToPlainText(doc.front_matter().title);
}

// 写入一个 1x1 PNG，让 asset manager 能够暂存真实的图。
std::filesystem::path WriteTinyPng() {
    static const unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // 签名
                                        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR 长度+类型
                                        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // 1x1
                                        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00,
                                        0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01,
                                        0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00,
                                        0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    auto path = std::filesystem::temp_directory_path() / "pf-async-figure.png";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png), sizeof(png));
    return path;
}

} // namespace

// ================= Preview gate（纯规则） =================

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
    PF_CHECK(EvaluatePreviewGate(current, result) == PreviewGateDecision::Accept);

    PreviewGateInput closed = current;
    closed.has_project = false;
    PF_CHECK(EvaluatePreviewGate(closed, result) == PreviewGateDecision::NoProject);

    // 项目标识先于 revision 检查：切换项目时绝不能让旧项目的 PDF 通过，
    // 即使 revision 相同也不行。
    PreviewGateInput other_project = current;
    other_project.project_id = ProjectId("p2");
    PF_CHECK(EvaluatePreviewGate(other_project, result) == PreviewGateDecision::ForeignProject);

    PreviewGateInput newer = current;
    newer.revision = ProjectRevision{11};
    PF_CHECK(EvaluatePreviewGate(newer, result) == PreviewGateDecision::StaleRevision);

    PreviewGateInput newer_snapshot = current;
    newer_snapshot.snapshot_id = "snap2";
    PF_CHECK(EvaluatePreviewGate(newer_snapshot, result) == PreviewGateDecision::StaleSnapshot);

    PreviewGateInput newer_build = current;
    newer_build.build_id = BuildId("b2");
    PF_CHECK(EvaluatePreviewGate(newer_build, result) == PreviewGateDecision::StaleBuild);
}

// ================= 带类型的预览标识 =================

PF_TEST(PreviewUpdateCarriesArtifactIdentity) {
    auto dir = TempDir("pf-async-preview-identity");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    Settle(rig.session);

    std::optional<PreviewUpdate> update;
    rig.session.SetPreviewUpdateHandler([&](const PreviewUpdate& u) { update = u; });
    rig.session.SetBuildResultHandler([&](const BuildResult&) {
        // handler 只在属主线程上运行。
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

// ================= 场景 1：build 期间编辑 =================

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
        // 不变式：任何到达应用层的结果都与当前最新的 revision 匹配。
        PF_CHECK(r.revision == rig.session.current_revision());
        PF_CHECK(r.project_id == rig.session.state().id());
        accepted.push_back(r);
    });

    // rev N：build 启动，并在 compiler 内部阻塞。
    rig.compiler.Hold();
    rig.session.RequestBuild(true);
    PF_CHECK(rig.compiler.WaitUntilEntered());
    const ProjectRevision building_rev = rig.session.current_revision();

    // 用户在 compiler 运行期间继续输入：rev N+1。
    rig.session.Execute(TitleCmd(rig.session, "Edited While Building"));
    PF_CHECK(rig.session.current_revision() > building_rev);
    PF_CHECK(rig.session.preview_state() != PreviewState::Fresh);

    // 旧 build 现在返回。它绝不能成为预览。
    rig.compiler.Release();
    PF_CHECK(PumpUntil(rig.session, [&] { return rig.session.preview_state() == PreviewState::Fresh; }));

    for (const auto& r : accepted) {
        PF_CHECK(r.revision != building_rev); // 陈旧的 build 被丢弃
    }
    rig.session.ProcessApplicationEvents();
    PF_CHECK(rig.session.owner_thread_violations() == 0);

    // 即便是为旧 revision 手工投递的迟到结果，也会被应用线程上的 gate 拒绝。
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

// ================= 场景 2：请求 build 后执行 undo ==========

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

    // Undo 又产生了一个 revision；针对 `after_edit` 的 build 现在
    // 已落后于 document。
    rig.session.Undo();
    const ProjectRevision after_undo = rig.session.current_revision();
    PF_CHECK(after_undo > after_edit);

    rig.compiler.Release();
    PF_CHECK(PumpUntil(rig.session, [&] { return rig.session.preview_state() == PreviewState::Fresh; }));

    for (const auto& r : accepted) {
        PF_CHECK(r.revision != after_edit); // 被 undo 的 revision
        PF_CHECK(r.revision != pre_edit);
    }
    PF_CHECK(rig.session.preview_state() == PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= 场景 3：构建期间切换模板 ============

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

    // IEEE build 启动并阻塞。
    rig.compiler.Hold();
    rig.session.ChangeTemplate("ieee-conference");
    PF_CHECK(rig.session.state().template_selection() == "ieee-conference");
    PF_CHECK(rig.compiler.WaitUntilEntered());
    const ProjectRevision ieee_rev = rig.session.current_revision();

    // 用户切换回 generic 模板。
    rig.session.ChangeTemplate("generic-article");
    PF_CHECK(rig.session.state().template_selection() == "generic-article");
    const ProjectRevision generic_rev = rig.session.current_revision();
    PF_CHECK(generic_rev > ieee_rev);

    rig.compiler.Release();
    PF_CHECK(PumpUntil(rig.session, [&] { return rig.session.preview_state() == PreviewState::Fresh; }));

    for (const auto& r : accepted) {
        PF_CHECK(r.revision != ieee_rev); // 旧模板的 PDF 从未显示
    }
    // 真正落地的预览属于 generic 模板的 revision。
    PF_CHECK(rig.session.preview_state() == PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= 场景 4：构建期间切换项目 ============

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
        // 结果只能代表当前打开的项目。
        PF_CHECK(r.project_id == rig.session.state().id());
        accepted.push_back(r);
    });

    rig.compiler.Hold();
    rig.session.RequestBuild(true);
    PF_CHECK(rig.compiler.WaitUntilEntered());

    // 在 A 的 build 在途时切换到另一个项目。
    rig.session.NewProject(dir_b);
    const ProjectId project_b = rig.session.state().id();
    PF_CHECK(!(project_a == project_b));
    accepted.clear();

    rig.compiler.Release();
    PumpFor(rig.session, 500);
    rig.session.ProcessApplicationEvents();

    // 项目 A 的任何内容都不得进入项目 B 的预览：仅靠 revision 检查
    // 无法发现这一点，项目 id 必须成为 gate 的一部分。
    PF_CHECK(accepted.empty());
    PF_CHECK(rig.session.preview_state() != PreviewState::Fresh);
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir_a);
    std::filesystem::remove_all(dir_b);
}

// ================= 场景 5：保存期间编辑 =================

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

    // 保存 rev20（已排队，由 worker 写入）……
    auto queued = rig.session.Save();
    PF_CHECK(queued.status == SaveResult::Status::Queued);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Saving);
    const ProjectRevision save_rev = queued.saved_revision;

    // ……写入完成前用户继续输入：rev21。
    rig.session.Execute(TitleCmd(rig.session, "Edited During Save"));
    const ProjectRevision edit_rev = rig.session.current_revision();
    PF_CHECK(edit_rev > save_rev);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Dirty);

    // rev20 的保存完成。项目必须保持 Dirty：写出的 snapshot
    // 并非当前状态。
    PF_CHECK(rig.session.FlushSaves().status == SaveResult::Status::Ok);
    PF_CHECK(rig.session.current_revision() == edit_rev);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Dirty);

    // 落盘的是 rev20 的 snapshot，而不是编辑后的 document。
    LoadRequest request;
    request.project_file = dir / "project.paper";
    auto loaded = ProjectPersistence::Load(request);
    PF_CHECK(loaded.project.has_value());
    if (loaded.project) {
        PF_CHECK(loaded.project->revision == save_rev);
        PF_CHECK(TitleOf(*loaded.project) == saved_title);
    }

    // 从同一 revision 再次保存会清除 Dirty。
    PF_CHECK(SaveAndFlush(rig.session).status == SaveResult::Status::Ok);
    PF_CHECK(rig.session.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}

// ================= 场景 6：编辑期间的 autosave =================

PF_TEST(ScenarioAutosaveWritesCompleteSnapshotWhileEditing) {
    auto dir = TempDir("pf-async-autosave-editing");
    SessionRig rig;
    rig.compiler.Open();
    rig.session.NewProject(dir);
    SeedDocument(rig.session);
    PF_CHECK(SaveAndFlush(rig.session).status == SaveResult::Status::Ok);
    Settle(rig.session);

    // document 经历的每个状态，以 revision 为键。autosave 文件必须与
    // 其中某一对完全一致——绝不能是混合体。
    std::map<std::uint64_t, std::string> revision_title;
    revision_title[rig.session.current_revision().value] = CurrentTitle(rig.session);

    int autosaves = 0;
    rig.session.SetSaveResultHandler([&](const SaveResult& result, SaveKind kind) {
        PF_CHECK(rig.session.IsOwnerThread());
        if (kind == SaveKind::Autosave && result.status == SaveResult::Status::Ok) {
            ++autosaves;
        }
    });

    rig.session.StartAutosaveTimer(std::chrono::milliseconds{40});
    for (int i = 0; i < 20; ++i) {
        const std::string title = "Edit " + std::to_string(i);
        rig.session.Execute(TitleCmd(rig.session, title));
        revision_title[rig.session.current_revision().value] = title;
        // 在定时器触发期间持续输入。
        PumpFor(rig.session, 25);
    }
    rig.session.StopAutosaveTimer();
    // 定时器停止前可能刚投递了一个 tick；因此排空两次。
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
        // 该 snapshot 内部一致：它声称的 revision 正是其内容所属的
        // revision。
        const auto it = revision_title.find(loaded.project->revision.value);
        PF_CHECK(it != revision_title.end());
        if (it != revision_title.end()) {
            PF_CHECK(TitleOf(*loaded.project) == it->second);
        }
    }
    PF_CHECK(rig.session.owner_thread_violations() == 0);
    std::filesystem::remove_all(dir);
}

// ================= 场景 7：删除被引用的图 =================

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

    // 一个引用该图的段落。
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

    // 删除之前该引用可以解析。
    const Document& before = rig.session.state().document();
    PF_CHECK(before.ContainsNode(figure_id));

    // 删除段落所指向的图。
    EditCommand del_cmd;
    del_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    del_cmd.project_id = rig.session.state().id();
    del_cmd.base_revision = rig.session.current_revision();
    DeleteBlockPayload del;
    del.node = figure_id;
    del_cmd.payload = del;
    PF_CHECK(rig.session.Execute(del_cmd).status == EditStatus::Applied);

    // document 保持结构合法……
    const Document& doc = rig.session.state().document();
    PF_CHECK(!doc.ContainsNode(figure_id));
    for (const auto& id : doc.CollectNodeIds()) {
        PF_CHECK(!(id == figure_id));
    }
    // ……并且悬空引用会以 diagnostic 形式出现，而不是崩溃。
    ValidationInput input;
    input.document = &doc;
    input.template_id = rig.session.state().template_selection();
    input.revision = rig.session.current_revision();
    input.snapshot_id = "snap-dangling";
    auto validation = Validator().Validate(input);
    bool found = false;
    for (const auto& d : validation.diagnostics) {
        if (d.code == "E-MISSING-XREF-TARGET")
            found = true;
    }
    PF_CHECK(found);
    PF_CHECK(validation.can_render);
    std::filesystem::remove(png);
    std::filesystem::remove_all(dir);
}
