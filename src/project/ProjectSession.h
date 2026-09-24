#pragma once
// ProjectSession：应用聚合／工作流协调器（架构 十五）。持有 ProjectState、
// EditingSystem，并把工作委派给 BuildCoordinator／SaveCoordinator／
// AssetManager／BibliographyService。
//
// M1 线程模型——单一所有者的应用状态：
//
//   应用线程                           后台 worker
//   ------------------                 ------------------
//   ProjectSession（本类）             BuildCoordinator worker
//     ProjectState / Document            只读取 BuildSnapshot
//     EditingSystem                      投递 BuildResultReadyEvent
//     PreviewState
//     PersistenceState
//   ---------------------------------  ------------------------------------
//   ProcessApplicationEvents()  <----  SaveCoordinator worker
//     AcceptBuildResult()               只读取 SerializedProject
//     ApplySaveCompletion()             投递 SaveCompletedEvent
//
// 规则：ProjectState / ProjectSession / EditingSystem / Document 只允许由
// 应用线程触碰。worker 只能看到值对象（BuildSnapshot、SerializedProject、
// BuildResult、SaveCompletion），除此之外别无其他。

#include "asset/AssetManager.h"
#include "bibliography/BibliographyService.h"
#include "build/BuildCoordinator.h"
#include "build/Toolchain.h"
#include "core/IoError.h"
#include "editing/EditingSystem.h"
#include "persistence/SaveCoordinator.h"
#include "project/ApplicationEvent.h"
#include "project/PreviewGate.h"
#include "project/PreviewUpdate.h"
#include "project/ProjectState.h"
#include "project/SnapshotFactory.h"
#include "template/TemplateRegistry.h"
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
    std::filesystem::path project_dir;  // MyPaper/
    std::filesystem::path project_file; // MyPaper/project.paper
    std::filesystem::path assets_dir;   // MyPaper/assets/
    std::filesystem::path build_dir;    // MyPaper/.paperforge/build/
    std::filesystem::path autosave_dir; // MyPaper/.paperforge/autosave/
};

class ProjectSession {
  public:
    struct Config {
        std::string tectonic_path = "tectonic";
        // tectonic 的 bundle 缓存；为空则禁用该覆盖。
        std::string tectonic_cache_dir;
        std::filesystem::path workspace_root = "/tmp/paperforge-sessions";
        std::chrono::milliseconds debounce{800};
        // 包含 runtime/texlive 的根目录（方案 §3）。为空表示「从应用安装
        // 根目录推导」。
        std::string install_root;
        // 可选的确定性编译器（测试用）。为空 -> tectonic。
        std::function<std::unique_ptr<ICompiler>()> compiler_factory;
    };

    explicit ProjectSession(Config config);
    ~ProjectSession();

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    // ---- 生命周期 ----
    // P0-03：NewProject 会报告目录创建失败的原因，而不是悄悄假装项目已打开。
    bool NewProject(const std::filesystem::path& project_dir, std::string* error = nullptr);
    bool OpenProject(const std::filesystem::path& project_dir, std::string* error);
    // 打开项目；若存在更新的 autosave／恢复 snapshot，则优先使用它。
    // 当应用了恢复 snapshot 时返回 true（调用方可据此提示用户）。
    // P0-01：同样处理从未保存过的项目——当只存在 autosave.paper 时，
    // 该 snapshot 就是项目本身。
    bool OpenProjectWithRecovery(const std::filesystem::path& project_dir, std::string* error,
                                 bool* recovered = nullptr);
    void CloseProject();

    // ---- 应用线程事件泵（M1）----
    // 线程安全。由后台 worker 调用，把值对象交给应用线程。绝不触碰 ProjectState。
    void PostApplicationEvent(ApplicationEvent event);
    // 线程安全。build worker 的完成路径；测试也通过该接缝注入迟到／外来的结果，
    // 以验证 gate 会拒绝它。结果只会在 ProcessApplicationEvents() 中被*应用*。
    void PostBuildResult(BuildResult result);
    // 线程安全。来自 worker 的单条 build 日志事件（Build Diagnostics
    // 方案 §4）。在 ProcessApplicationEvents() 中应用（若属于已被取代的
    // build，则在此丢弃）。
    void PostBuildEvent(BuildEvent event);

    // 仅限应用线程／所有者线程。排空排队的事件并应用它们
    // （阶段变化、build 结果、保存完成、autosave 滴答）。
    void ProcessApplicationEvents();
    // 阻塞直到至少有一个事件可用，然后排空——供轮询完成状态的非 Qt 驱动
    // （CLI／测试）使用。
    bool WaitForApplicationEvent(std::chrono::milliseconds timeout);

    bool HasPendingApplicationEvents() const;

    // 由宿主（Qt 适配器）安装，使后台投递能唤醒应用事件循环，而不必轮询。
    // 该回调在 worker 线程上调用；其自身必须线程安全。
    void SetWakeHandler(std::function<void()> handler);

    // 应用线程：preview gate。当 `result` 是为当前此刻的 project／revision／
    // build 生成时返回 true。过期的结果在这里丢弃，绝不在 worker 中丢弃。
    bool AcceptBuildResult(const BuildResult& result);

    // ---- Autosave（崩溃恢复）----
    // 启动／停止后台定时器（默认 30s）。定时器线程只投递 AutosaveTickEvent；
    // snapshot 在应用线程上捕获。
    // 仅在 dirty 时自动保存，且绝不清除 Clean/Dirty（架构 32）。
    void StartAutosaveTimer(std::chrono::milliseconds interval = std::chrono::milliseconds{30000});
    void StopAutosaveTimer();
    // 当存在比 project.paper 更新的 autosave snapshot 时为 true。
    bool HasRecoverySnapshot() const;
    SaveResult RecoverFromAutosave();

    // ---- 编辑（应用线程）----
    EditResult Execute(const EditCommand& command);
    EditResult Undo();
    EditResult Redo();

    // ---- Build ----
    void RequestBuild(bool manual = false);
    void CancelBuild();
    BuildPhase build_phase() const;
    // 最近一次 build 请求的身份标识（不存在时为空）。
    const std::string& latest_snapshot_id() const noexcept {
        return latest_snapshot_id_;
    }
    const BuildId& latest_build_id() const noexcept {
        return latest_build_id_;
    }

    // ---- Save ----
    // 两者都会把不可变 snapshot 入队给 save worker，并立即以
    // SaveResult::Status::Queued 返回。完成结果在 ProcessApplicationEvents()
    // 中应用。
    SaveResult Save();
    SaveResult Autosave();
    // 应用线程：阻塞直到所有排队的 save 都已写入且其完成结果已被应用。
    // 返回最后一次用户保存的结果。
    SaveResult FlushSaves();
    size_t pending_saves() const {
        return save_coordinator_.pending();
    }
    // 仅测试用的接缝（P0-03）：把 save coordinator 驱动到停止状态，
    // 以便「关闭期间不得伪造 Queued」这一规则可被执行验证。
    // 生产代码通过 ~ProjectSession 到达同一状态。
    void ShutdownSaveCoordinatorForTest() {
        save_coordinator_.Shutdown();
    }
    // 仅测试用的接缝（P0-04）：绕过写文件的导入路径直接安装 bibliography
    // 源字节，以便独立检验保存时的路径边界。
    void ForceBibliographyForTest(std::string bibtex) {
        bibliography_bibtex_ = std::move(bibtex);
    }

    // ---- Template ----
    void ChangeTemplate(const std::string& template_id);

    // ---- Assets ----
    AssetImportResult ImportAsset(const std::filesystem::path& source);
    // 在一次面向用户的操作中完成图片的暂存、注册与插入。
    EditResult InsertFigureFromSource(const std::filesystem::path& source, const NodeId& parent,
                                      std::optional<size_t> index = std::nullopt);

    // ---- Bibliography ----
    BibliographyImportResult ImportBibliography(const std::string& bibtex_text);
    CitationSearchResult SearchCitations(const std::string& query);
    bool HasBibliography() const {
        return !bibliography_bibtex_.empty();
    }

    // ---- 状态访问（仅限应用线程）----
    ProjectState& state() noexcept {
        return state_;
    }
    // 可变 document 访问。优先使用编辑命令；它之所以存在，是因为
    // InlineEditor 会交出已完整构造的 InlineContent，也因为
    // 读取侧辅助函数（遍历、校验）需要非 const 的 document 才能
    // 返回容器指针。
    Document& mutable_document() noexcept {
        return state_.mutable_document();
    }
    const ProjectState& state() const noexcept {
        return state_;
    }
    EditingSystem& editing() noexcept {
        return editing_;
    }
    UndoHistory& history() noexcept {
        return editing_.history();
    }
    DocumentIndex& index() noexcept {
        return editing_.index();
    }
    AssetManager& assets() noexcept {
        return *assets_;
    }
    BibliographyDatabase& bibliography() noexcept {
        return bibliography_db_;
    }

    // ---- 单一所有者诊断 ----
    // 当调用方运行在构造本 session 的线程上时为 true。
    bool IsOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner_thread_;
    }
    // 统计从外部线程进入仅限所有者入口的调用次数。正确的 build 会使其
    // 保持为 0；回归测试会对此断言。
    std::size_t owner_thread_violations() const noexcept {
        return owner_thread_violations_.load();
    }

    LifecycleState lifecycle_state() const noexcept {
        return lifecycle_state_;
    }
    PersistenceState persistence_state() const noexcept {
        return persistence_state_;
    }
    PreviewState preview_state() const noexcept {
        return preview_state_;
    }
    ProjectRevision current_revision() const noexcept {
        return state_.revision();
    }
    const SessionPaths& paths() const noexcept {
        return paths_;
    }
    const std::string& bibliography_bibtex() const noexcept {
        return bibliography_bibtex_;
    }

    // ---- 观察者（应用线程）----
    // 只接收被采纳的 build 结果（过期结果绝不会到达这里）。
    void SetBuildResultHandler(std::function<void(const BuildResult&)> handler) {
        build_result_handler_ = std::move(handler);
    }
    // 仅针对*当前* build 的实时 build 日志事件（方案 §35）：build id 不再匹配
    // latest_build_id() 的事件会在此丢弃，因此旧 build 绝不会写入新 build 的日志。
    void SetBuildEventHandler(std::function<void(const BuildEvent&)> handler) {
        build_event_handler_ = std::move(handler);
    }
    // 带类型的 preview 事件：标明是哪个 project／build／revision 生成了该 PDF。
    void SetPreviewUpdateHandler(std::function<void(const PreviewUpdate&)> handler) {
        preview_update_handler_ = std::move(handler);
    }
    void SetSaveResultHandler(std::function<void(const SaveResult&, SaveKind)> handler) {
        save_result_handler_ = std::move(handler);
    }
    void SetPhaseHandler(std::function<void(BuildPhase, BuildPhase)> handler) {
        phase_handler_ = std::move(handler);
    }
    void SetDocumentChangedHandler(std::function<void(const DocumentChangedEvent&)> handler) {
        document_changed_handler_ = std::move(handler);
    }

  private:
    // P0-03：结构化结果；只有当所有目录都创建成功时，
    // 调用方才进入 LifecycleState::Open。
    Result<void, IoError> EnsureDirectories();
    void MarkDirty();
    // 单一所有者规则的烟雾探测器：当从外部线程进入仅限所有者的入口时
    // 递增计数器。
    void NoteOwnerThreadUse() const;
    // 应用线程：把当前状态捕获为不可变 snapshot。
    SaveSnapshot CaptureSaveSnapshot() const;

    // 事件应用（应用线程）。
    void HandleEvent(const BuildPhaseChangedEvent& event);
    void HandleEvent(const BuildResultReadyEvent& event);
    void HandleEvent(const BuildEventReadyEvent& event);
    void HandleEvent(const SaveCompletedEvent& event);
    void HandleEvent(const AutosaveTickEvent& event);
    void ApplySaveCompletion(const SaveCompletion& completion);
    // 最新 build 请求的身份标识；供 preview gate 使用。
    PreviewGateInput CurrentGateInput() const;

    Config config_;
    SessionPaths paths_;
    ProjectState state_;
    std::unique_ptr<AssetManager> assets_;
    BibliographyDatabase bibliography_db_;
    std::string bibliography_bibtex_;
    std::uint64_t bibliography_revision_ = 0;
    // P0-03：当 Save() 中对 references.bib 的原子写入失败时置位。即便
    // project.paper 本身已落盘，该 revision 的完成结果也绝不能把项目
    // 报告为 Clean。
    bool bibliography_write_failed_ = false;
    SnapshotFactory snapshot_factory_;
    EditingSystem editing_;
    std::unique_ptr<ICompiler> compiler_;
    // 随包分发的便携式 TeX Live 运行时的根目录（方案 §17）。
    std::filesystem::path texlive_root_;
    SaveCoordinator save_coordinator_;
    std::unique_ptr<BuildCoordinator> build_coordinator_;

    LifecycleState lifecycle_state_ = LifecycleState::NoProject;
    PersistenceState persistence_state_ = PersistenceState::Clean;
    PreviewState preview_state_ = PreviewState::NoPreview;

    // 拥有下方所有可变成员的线程（M1 单一所有者规则）。
    const std::thread::id owner_thread_ = std::this_thread::get_id();
    mutable std::atomic<std::size_t> owner_thread_violations_{0};

    // 最近一次 build 请求的身份标识（应用线程）。
    std::string latest_snapshot_id_;
    BuildId latest_build_id_;

    // autosave 定时器状态
    std::thread autosave_thread_;
    std::atomic<bool> autosave_stop_{false};
    std::chrono::milliseconds autosave_interval_{30000};

    // 跨线程邮箱：由 worker 投递事件，由所有者排空。
    mutable std::mutex events_mutex_;
    std::condition_variable events_condition_;
    std::vector<ApplicationEvent> pending_events_;
    std::function<void()> wake_handler_;

    // 观察到最近一次用户保存的结果（应用线程）。
    SaveResult last_user_save_result_;

    std::function<void(const BuildResult&)> build_result_handler_;
    std::function<void(const BuildEvent&)> build_event_handler_;
    std::function<void(const PreviewUpdate&)> preview_update_handler_;
    std::function<void(const SaveResult&, SaveKind)> save_result_handler_;
    std::function<void(BuildPhase, BuildPhase)> phase_handler_;
    std::function<void(const DocumentChangedEvent&)> document_changed_handler_;
};

} // namespace pf
