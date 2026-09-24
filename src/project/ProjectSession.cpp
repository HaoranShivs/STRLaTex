#include "project/ProjectSession.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "bibliography/BibliographyService.h"
#include "build/CompilerFactory.h"
#include "build/RuntimeManager.h"
#include "core/IdGenerator.h"
#include "core/ProjectPath.h"
#include "document/InlineText.h"

namespace pf {

namespace {
// 以原子方式写入一个小的整文件资源：临时文件 + rename。bibliography 绝不能
// 被观察到半写入状态（也不能以半写入状态挺过崩溃），且写入失败必须保持原文件
// 不变（引用方案 §7）。
// P0-03：用结构化结果代替 bool——调用方可以区分权限被拒、磁盘已满和替换失败，
// UI 也能给出具体提示，而不是笼统的「保存失败」。
[[nodiscard]] Result<void, IoError> WriteFileAtomically(const std::filesystem::path& path,
                                                        const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Unexpected2<IoError>(MakeIoError(ec, IoErrorCode::DirectoryCreateFailed,
                                                "cannot create the file's directory", path.parent_path()));
    }
    const auto temp = path.string() + ".tmp";
    std::filesystem::remove(temp, ec);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Unexpected2<IoError>(
                MakeIoError(ec, IoErrorCode::OpenFailed, "cannot open the temporary file", path));
        }
        out << contents;
        out.flush();
        if (!out.good()) {
            return Unexpected2<IoError>(
                MakeIoError(ec, IoErrorCode::WriteFailed, "writing the temporary file failed", path));
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return Unexpected2<IoError>(MakeIoError(ec, IoErrorCode::ReplaceFailed, "cannot replace the file", path));
    }
    return {};
}
} // namespace

ProjectSession::ProjectSession(Config config)
    : config_(std::move(config)),
      assets_(std::make_unique<AssetManager>(std::filesystem::path("/tmp/paperforge-assets"))),
      snapshot_factory_(assets_.get()),
      // save worker 通过应用事件队列回传完成结果，绝不直接调用 domain。
      save_coordinator_([this](const SaveCompletion& completion) {
          SaveCompletedEvent event;
          event.completion = completion;
          PostApplicationEvent(std::move(event));
      }) {
    // 在成员就绪后再接线编辑系统（lambda 会捕获 `this`）。
    EditingSystem::Host ehost;
    ehost.project_id = [this] { return state_.id(); };
    ehost.revision = [this] { return state_.revision(); };
    ehost.bump_revision = [this] { return state_.BumpRevision(); };
    ehost.document = [this]() -> Document& { return state_.mutable_document(); };
    ehost.on_document_changed = [this](const DocumentChangedEvent& e) {
        if (document_changed_handler_)
            document_changed_handler_(e);
    };
    editing_.SetHost(std::move(ehost));

    // 编译器：使用注入的工厂（测试）或位于配置路径的 tectonic。
    if (config_.compiler_factory) {
        compiler_ = config_.compiler_factory();
    } else {
        compiler_ = std::make_unique<TectonicCompiler>(config_.tectonic_path, config_.tectonic_cache_dir);
    }

    // 生产环境（方案 §3、§17）：随包提供的便携版 TeX Live。其根目录即包含
    // runtime/texlive 的那个目录。
    // 安装根目录即包含 runtime/texlive 的那个目录。源码构建传入仓库根目录；
    // 已安装的应用传入其自身目录。
    std::filesystem::path install_root = config_.install_root;
    if (install_root.empty()) {
        // __FILE__ 位于 <repo>/src/project，因此仓库根目录在其上两级。
        install_root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    }
    texlive_root_ = RuntimeManager(install_root).Initialize().texlive_root;

    BuildCoordinator::Host bhost;
    bhost.project_id = [this] { return state_.id(); };
    // 仅限不可变配置——绝不涉及可变项目状态。
    bhost.workspace_root = [this] { return config_.workspace_root.string(); };
    // TEMP-DEBUG：把生成的 LaTeX 暴露到项目的 build 目录下。
    bhost.debug_dump_dir = [this]() -> std::optional<std::string> {
        if (lifecycle_state_ != LifecycleState::Open)
            return std::nullopt;
        return paths_.build_dir.string();
    };
    // worker 线程：只发布值对象，绝不读取实时 state。
    bhost.on_build_finished = [this](const BuildResult& result) { PostBuildResult(result); };
    // worker 线程：生命周期/流式日志事件走同一个 mailbox。
    bhost.on_build_event = [this](const BuildEvent& event) { PostBuildEvent(event); };
    bhost.on_phase_changed = [this](BuildPhase previous, BuildPhase current) {
        BuildPhaseChangedEvent event;
        event.previous = previous;
        event.current = current;
        PostApplicationEvent(std::move(event));
    };
    // 测试注入过自己的 compiler 时予以保留（确定性测试不应依赖 TeX runtime
    // 是否存在）。
    if (config_.compiler_factory) {
        build_coordinator_ = std::make_unique<BuildCoordinator>(std::move(bhost), compiler_.get());
        return;
    }
    // 生产环境：coordinator 每次 build 都依据 snapshot 的 toolchain 选择
    // compiler（方案 §13）。
    build_coordinator_ = std::make_unique<BuildCoordinator>(
        std::move(bhost), [this](const BuildToolchain& toolchain) -> std::unique_ptr<ICompiler> {
            TemplateDefinition tpl;
            tpl.toolchain.engine = toolchain.engine;
            tpl.toolchain.bibliography_engine = toolchain.bibliography_engine;
            CompilerFactory factory(texlive_root_);
            return factory.Create(tpl);
        });
    build_coordinator_->set_debounce(config_.debounce);
}

ProjectSession::~ProjectSession() {
    StopAutosaveTimer();
    // 在它们所发布的状态仍然存活时停止生产者，并给排队的 save 一个落盘的机会。
    build_coordinator_.reset();
    save_coordinator_.Shutdown();
}

// ---------------- 应用事件泵 ----------------

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
    if (wake)
        wake();
}

void ProjectSession::PostBuildResult(BuildResult result) {
    BuildResultReadyEvent event;
    event.result = std::move(result);
    PostApplicationEvent(std::move(event));
}

void ProjectSession::PostBuildEvent(BuildEvent event) {
    BuildEventReadyEvent ready;
    ready.event = std::move(event);
    PostApplicationEvent(std::move(ready));
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
    // 在锁外应用：handler 可能继续发布新事件。
    for (const auto& event : events) {
        std::visit([this](const auto& typed) { HandleEvent(typed); }, event);
    }
}

bool ProjectSession::WaitForApplicationEvent(std::chrono::milliseconds timeout) {
    {
        std::unique_lock<std::mutex> lock(events_mutex_);
        events_condition_.wait_for(lock, timeout, [this] { return !pending_events_.empty(); });
        if (pending_events_.empty())
            return false;
    }
    ProcessApplicationEvents();
    return true;
}

void ProjectSession::HandleEvent(const BuildPhaseChangedEvent& event) {
    if (phase_handler_)
        phase_handler_(event.previous, event.current);
}

void ProjectSession::HandleEvent(const BuildResultReadyEvent& event) {
    AcceptBuildResult(event.result);
}

void ProjectSession::HandleEvent(const BuildEventReadyEvent& event) {
    // 旧 build 隔离（Build Diagnostics 方案 §35）：该判定运行在持有当前 build
    // 身份标识的线程上。来自被取代尝试的迟到事件，绝不能追加进用户正在查看的
    // 日志。
    if (!latest_build_id_.empty() && event.event.build_id != latest_build_id_) {
        return;
    }
    if (build_event_handler_)
        build_event_handler_(event.event);
}

void ProjectSession::HandleEvent(const SaveCompletedEvent& event) {
    ApplySaveCompletion(event.completion);
}

void ProjectSession::HandleEvent(const AutosaveTickEvent&) {
    // 定时线程只是敲了一下铃；snapshot 在这里、在持有 Document 的线程上捕获。
    if (lifecycle_state_ == LifecycleState::Open && persistence_state_ == PersistenceState::Dirty) {
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
    // （架构 47）：过期判定属于持有 revision 的那个线程的职责，worker 绝不
    // 做此判定。
    const PreviewGateDecision decision = EvaluatePreviewGate(CurrentGateInput(), result);
    if (decision != PreviewGateDecision::Accept)
        return false;

    if (result.outcome == BuildResult::Outcome::Success) {
        preview_state_ = PreviewState::Fresh;
    } else if (result.outcome == BuildResult::Outcome::Failure) {
        // 保留磁盘上最后一次成功的 PDF；预览已过期。
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
    if (build_result_handler_)
        build_result_handler_(result);
    if (preview_update_handler_)
        preview_update_handler_(update);
    return true;
}

// ---------------- 生命周期 ----------------

// P0-03：每个目录都通过自己的 error_code 创建，首次失败即以携带精确路径的
// 结构化错误中止。目录创建失败的项目绝不能进入 LifecycleState::Open（旧代码
// 忽略所有 error_code，无论如何都报告成功）。
Result<void, IoError> ProjectSession::EnsureDirectories() {
    struct Target {
        std::filesystem::path path;
        const char* what;
    };
    const Target targets[] = {
        {paths_.project_dir, "project directory"},
        {paths_.assets_dir, "assets directory"},
        {paths_.build_dir, "build directory"},
        {paths_.autosave_dir, "autosave directory"},
    };
    for (const Target& target : targets) {
        std::error_code ec;
        std::filesystem::create_directories(target.path, ec);
        // create_directories 把目录已存在的情况视为成功（不设 error_code）；
        // 其他任何情况都意味着该项目无法使用。
        if (ec) {
            return Unexpected2<IoError>(MakeIoError(ec, IoErrorCode::DirectoryCreateFailed,
                                                    std::string("cannot create the ") + target.what, target.path));
        }
    }
    // 把 asset manager 重新指向本项目的 assets 目录。
    assets_ = std::make_unique<AssetManager>(paths_.assets_dir);
    snapshot_factory_ = SnapshotFactory(assets_.get());
    return {};
}

bool ProjectSession::NewProject(const std::filesystem::path& project_dir, std::string* error) {
    CloseProject();
    paths_.project_dir = project_dir;
    paths_.project_file = project_dir / "project.paper";
    paths_.assets_dir = project_dir / "assets";
    paths_.build_dir = project_dir / ".paperforge" / "build";
    paths_.autosave_dir = project_dir / ".paperforge" / "autosave";
    // P0-03：只有在每个目录都真实存在之后才进入 Open。
    if (auto created = EnsureDirectories(); !created.ok()) {
        if (error)
            *error = created.error().ToString();
        lifecycle_state_ = LifecycleState::NoProject;
        return false;
    }

    state_.Reset();
    state_.SetId(ProjectId(IdGenerator::NewProjectId()));
    state_.mutable_settings().name = project_dir.filename().string();
    lifecycle_state_ = LifecycleState::Open;
    persistence_state_ = PersistenceState::Dirty; // 尚未保存
    preview_state_ = PreviewState::NoPreview;
    return true;
}

bool ProjectSession::OpenProject(const std::filesystem::path& project_dir, std::string* error) {
    CloseProject();
    paths_.project_dir = project_dir;
    paths_.project_file = project_dir / "project.paper";
    paths_.assets_dir = project_dir / "assets";
    paths_.build_dir = project_dir / ".paperforge" / "build";
    paths_.autosave_dir = project_dir / ".paperforge" / "autosave";
    if (auto created = EnsureDirectories(); !created.ok()) {
        if (error)
            *error = created.error().ToString();
        lifecycle_state_ = LifecycleState::NoProject;
        return false;
    }

    LoadRequest request;
    request.project_file = paths_.project_file;
    auto result = ProjectPersistence::Load(request);
    if (!result.project) {
        if (error)
            *error = result.detail;
        lifecycle_state_ = LifecycleState::NoProject;
        return false;
    }
    const auto& sp = *result.project;
    state_.Reset();
    state_.SetId(ProjectId(sp.project_id.empty() ? IdGenerator::NewProjectId() : sp.project_id));
    state_.SetRevision(ProjectRevision{sp.revision});
    state_.mutable_document() = sp.document;
    state_.mutable_template() = sp.template_id.empty() ? "generic-article" : sp.template_id;
    state_.mutable_settings().bibliography_path = sp.bibliography_path;
    for (const auto& meta : sp.assets) {
        assets_->registry().Register(meta);
    }
    // 若存在则加载 bibliography 文件。P0-04（二次校验）：该路径来自不可信
    // 文件——必须在读取前通过信任边界解析，这样手工改过的 project.paper 就
    // 永远无法让此操作打开项目目录之外的文件。
    auto bib_path = ResolveUntrustedProjectPath(paths_.project_dir, sp.bibliography_path);
    if (bib_path.ok() && std::filesystem::exists(bib_path.value())) {
        BibliographyService service(bibliography_db_);
        auto import = service.ImportFile(bib_path.value().string());
        if (import.status == BibliographyImportResult::Status::Ok) {
            std::ifstream in(bib_path.value(), std::ios::binary);
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
    if (lifecycle_state_ == LifecycleState::NoProject)
        return;
    StopAutosaveTimer();
    build_coordinator_->Cancel();
    // 排队的 save 是针对正在关闭的项目的快照：让它们先落盘，再重置 state，
    // 否则用户最后一次 Ctrl+S 可能丢失。
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

bool ProjectSession::OpenProjectWithRecovery(const std::filesystem::path& project_dir, std::string* error,
                                             bool* recovered) {
    if (recovered)
        *recovered = false;

    // P0-01：三种情况，对应整改方案 §P0-01。
    //
    //   A. project.paper 存在     -> 打开它，若存在更新的 autosave 则再从中
    //                                恢复。
    //   B. 只有 autosave.paper    -> 一个从未保存过的项目。直接加载 autosave：
    //                                旧代码要求必须先打开 project.paper，导致
    //                                这条恢复路径不可达，工作成果就此丢失。
    //   C. 两者都没有             -> NotFound。
    const bool has_project_file = std::filesystem::exists(project_dir / "project.paper");
    const bool has_autosave = std::filesystem::exists(project_dir / ".paperforge" / "autosave" / "autosave.paper");

    if (!has_project_file && !has_autosave) {
        if (error)
            *error = "no project.paper or autosave.paper in " + project_dir.string();
        return false;
    }

    if (has_project_file) {
        // 情况 A。
        if (!OpenProject(project_dir, error))
            return false;
        if (HasRecoverySnapshot()) {
            auto result = RecoverFromAutosave();
            if (result.status == SaveResult::Status::Ok && recovered) {
                *recovered = true;
                // 恢复出来的内容尚未被用户保存。
                persistence_state_ = PersistenceState::Dirty;
            }
        }
        return true;
    }

    // 情况 B：只有 autosave 幸存。像项目已打开那样初始化路径和 state，再把
    // autosave snapshot 载入其中。
    CloseProject();
    paths_.project_dir = project_dir;
    paths_.project_file = project_dir / "project.paper";
    paths_.assets_dir = project_dir / "assets";
    paths_.build_dir = project_dir / ".paperforge" / "build";
    paths_.autosave_dir = project_dir / ".paperforge" / "autosave";
    if (auto created = EnsureDirectories(); !created.ok()) {
        if (error)
            *error = created.error().ToString();
        lifecycle_state_ = LifecycleState::NoProject;
        return false;
    }

    state_.Reset();
    state_.SetId(ProjectId(IdGenerator::NewProjectId()));
    state_.mutable_settings().name = project_dir.filename().string();
    lifecycle_state_ = LifecycleState::Open;
    persistence_state_ = PersistenceState::Dirty;
    preview_state_ = PreviewState::NoPreview;

    auto result = RecoverFromAutosave();
    if (result.status != SaveResult::Status::Ok) {
        // autosave 存在却读不出来：如实报告，而不是留下一个伪装成「已恢复」的
        // 半打开空项目。
        if (error)
            *error = result.detail.empty() ? std::string("autosave could not be recovered") : result.detail;
        lifecycle_state_ = LifecycleState::NoProject;
        state_.Reset();
        return false;
    }
    if (recovered)
        *recovered = true;
    // 恢复出来的内容尚未被用户保存。
    persistence_state_ = PersistenceState::Dirty;
    return true;
}

bool ProjectSession::HasRecoverySnapshot() const {
    auto autosave_file = paths_.autosave_dir / "autosave.paper";
    if (!std::filesystem::exists(autosave_file))
        return false;
    // 比 project.paper 更新吗？
    auto autosave_time = std::filesystem::last_write_time(autosave_file);
    if (!std::filesystem::exists(paths_.project_file))
        return true;
    auto project_time = std::filesystem::last_write_time(paths_.project_file);
    return autosave_time >= project_time;
}

SaveResult ProjectSession::RecoverFromAutosave() {
    SaveResult fail;
    fail.status = SaveResult::Status::IoError;
    fail.detail = "no recovery snapshot";
    if (lifecycle_state_ != LifecycleState::Open)
        return fail;
    auto autosave_file = paths_.autosave_dir / "autosave.paper";
    if (!std::filesystem::exists(autosave_file))
        return fail;

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
    state_.mutable_template() = sp.template_id.empty() ? "generic-article" : sp.template_id;
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
            // 分小步睡眠，以便 Stop 能及时响应。
            for (std::chrono::milliseconds waited{0}; waited < autosave_interval_ && !autosave_stop_.load();
                 waited += std::chrono::milliseconds{100}) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (autosave_stop_.load())
                break;
            // 只敲铃。从本线程读取 Document 会与用户的编辑产生 race；snapshot
            // 由应用线程捕获。
            PostApplicationEvent(AutosaveTickEvent{});
        }
    });
}

void ProjectSession::StopAutosaveTimer() {
    autosave_stop_.store(true);
    if (autosave_thread_.joinable())
        autosave_thread_.join();
}

void ProjectSession::MarkDirty() {
    persistence_state_ = PersistenceState::Dirty;
    preview_state_ = PreviewState::Stale;
}

// ---------------- 编辑 ----------------

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
    if (lifecycle_state_ != LifecycleState::Open)
        return;
    auto snapshot = snapshot_factory_.CreateBuildSnapshot(state_, bibliography_bibtex_);
    // 模板决定引擎（方案 §14）：在此处、在应用线程上解析，并随请求一起携带。
    if (const TemplateDefinition* tpl = TemplateRegistry::Instance().Find(state_.template_selection())) {
        snapshot.toolchain.engine = tpl->toolchain.engine;
        snapshot.toolchain.bibliography_engine = tpl->toolchain.bibliography_engine;
    }
    // 在持有线程上记住这次请求的身份标识；只有与之匹配的结果才会被接受。
    latest_snapshot_id_ = snapshot.snapshot_id;
    latest_build_id_ = snapshot.build_id;
    build_coordinator_->RequestBuild(std::move(snapshot), manual);
}

void ProjectSession::CancelBuild() {
    build_coordinator_->Cancel();
}

BuildPhase ProjectSession::build_phase() const {
    return build_coordinator_->phase();
}

// ---------------- 保存 ----------------

SaveSnapshot ProjectSession::CaptureSaveSnapshot() const {
    // 应用线程：深拷贝可变状态，交给 worker。
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
    queued.saved_revision = snapshot.revision;
    // P0-03：只有 coordinator 真正接受了任务，这次 save 才算入队；关闭过程中
    // Enqueue 返回 nullopt，调用方绝不能以为有写入正在进行。
    auto save_id = save_coordinator_.Enqueue(std::move(snapshot.serialized), paths_.project_file, SaveKind::User);
    if (!save_id) {
        queued.status = SaveResult::Status::IoError;
        queued.detail = "save queue is shutting down; the project was not saved";
        return queued;
    }
    queued.status = SaveResult::Status::Queued;
    queued.save_id = save_id->value();
    persistence_state_ = PersistenceState::Saving;
    // Bibliography side-car：一个小文件，在持有者线程上写入。原子写入
    // （temp + rename），因此崩溃绝不会留下截断的 references.bib；且写入
    // *配置的* 项目相对路径，这样 OpenProject 读回的内容与 Save 写入的完全
    // 一致（引用方案 §7）。
    // P0-03：写入失败会被记住——当 side-car 未能持久化时，完成处理函数拒绝
    // 报告 Clean。
    if (!bibliography_bibtex_.empty()) {
        const std::string rel = state_.settings().bibliography_path.empty() ? std::string("references.bib")
                                                                            : state_.settings().bibliography_path;
        // P0-04（二次校验）：写入是最危险的操作——存储路径中的目录穿越可能
        // 覆盖项目之外的文件。通过边界解析；解析失败就拒绝写入。
        auto target = ResolveUntrustedProjectPath(paths_.project_dir, rel);
        if (!target.ok() || !WriteFileAtomically(target.value(), bibliography_bibtex_)) {
            bibliography_write_failed_ = true;
        } else {
            bibliography_write_failed_ = false;
        }
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
    queued.saved_revision = snapshot.revision;
    auto save_id = save_coordinator_.Enqueue(std::move(snapshot.serialized), paths_.autosave_dir / "autosave.paper",
                                             SaveKind::Autosave);
    if (!save_id) {
        // P0-03：关闭进行中——不要假装 autosave 已入队。
        queued.status = SaveResult::Status::IoError;
        queued.detail = "save queue is shutting down; autosave skipped";
        return queued;
    }
    queued.status = SaveResult::Status::Queued;
    queued.save_id = save_id->value();
    // Autosave 不改变 Clean/Dirty 状态（架构 32）。
    return queued;
}

SaveResult ProjectSession::FlushSaves() {
    save_coordinator_.Flush();
    // 完成结果以事件形式发布；应用它们，使调用方能观察到最终的持久化状态。
    ProcessApplicationEvents();
    return last_user_save_result_;
}

void ProjectSession::ApplySaveCompletion(const SaveCompletion& completion) {
    // 在项目已切换之后才完成的 save，不能代表新项目说话。
    if (completion.project_id != state_.id() || lifecycle_state_ != LifecycleState::Open) {
        if (save_result_handler_) {
            save_result_handler_(completion.result, completion.kind);
        }
        return;
    }
    if (completion.kind == SaveKind::User) {
        last_user_save_result_ = completion.result;
        const bool actually_saved = completion.outcome == SaveOutcome::Saved && !bibliography_write_failed_;
        if (actually_saved) {
            // 只有当 snapshot 写入期间没有任何改动时才算 Clean：save rev20 ->
            // edit rev21 -> save20 完成，必须让项目保持 Dirty。
            if (completion.revision == state_.revision()) {
                persistence_state_ = PersistenceState::Clean;
            }
        } else if (completion.outcome == SaveOutcome::Superseded) {
            // P0-03：被取代的 save 不算失败。由更新的那次 save 决定最终结果；
            // 不要因此把状态降级为 SaveFailed。
            if (persistence_state_ == PersistenceState::Saving) {
                // 仍在等待更新的那次 save——保持 Saving。
            }
        } else if (completion.revision == state_.revision()) {
            persistence_state_ = PersistenceState::SaveFailed;
        } else {
            // 较旧的 snapshot 失败，而较新的仍在进行中；交给较新的 save 决定。
            if (persistence_state_ != PersistenceState::Saving) {
                persistence_state_ = PersistenceState::SaveFailed;
            }
        }
    }
    if (save_result_handler_)
        save_result_handler_(completion.result, completion.kind);
}

// ---------------- 模板 ----------------

void ProjectSession::ChangeTemplate(const std::string& template_id) {
    const auto* def = TemplateRegistry::Instance().Find(template_id);
    if (!def)
        return;
    std::string old_id = state_.template_selection();
    if (old_id == template_id)
        return;
    state_.mutable_template() = template_id;
    // 模板变更：ProjectRevision +1，DocumentVersion 不变（架构补充 8）。
    // 压入一条模板历史动作。
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

// ---------------- 资源 ----------------

AssetImportResult ProjectSession::ImportAsset(const std::filesystem::path& source) {
    AssetImportRequest request;
    request.source_path = source;
    request.project_id = state_.id();
    auto result = assets_->Stage(request);
    if (result.status == AssetImportResult::Status::Ok) {
        // 只有在 asset 真正被使用时才注册（架构补充规则 3）——
        // InsertFigureFromSource 负责注册。
    }
    return result;
}

EditResult ProjectSession::InsertFigureFromSource(const std::filesystem::path& source, const NodeId& parent,
                                                  std::optional<size_t> index) {
    auto imported = ImportAsset(source);
    if (imported.status != AssetImportResult::Status::Ok) {
        EditResult r;
        r.status = EditStatus::Rejected;
        r.failure = FailureReason::InvalidTarget;
        r.detail = "asset import failed: " + imported.detail;
        return r;
    }
    // 既然已被使用，现在就注册。
    assets_->Register(assets_->ToCandidate(imported));

    // 针对当前 revision 重新解析锚点（架构补充规则 4）。
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

// ---------------- 参考文献 ----------------

BibliographyImportResult ProjectSession::ImportBibliography(const std::string& bibtex_text) {
    // P0-03：导入是一个小事务。先解析并暂存文件；只有在字节确实落盘之后，
    // 内存中的数据库才提交。替换之前的任何失败都不会触碰旧数据库、旧的
    // references.bib、revision 和 dirty 状态。
    BibliographyDatabase staged_db;
    BibliographyService staged_service(staged_db);
    auto result = staged_service.ImportText(bibtex_text);
    if (result.status != BibliographyImportResult::Status::Ok) {
        return result;
    }

    const std::filesystem::path bib_file = paths_.project_dir / "references.bib";
    if (!WriteFileAtomically(bib_file, bibtex_text)) {
        BibliographyImportResult io_fail;
        io_fail.status = BibliographyImportResult::Status::ParseError;
        // 通过 detail 区分 I/O 失败与解析失败；status 枚举没有 IoError 槽位
        // （为兼容 Qt 信号而保留）。
        io_fail.detail = "cannot write references.bib; bibliography unchanged";
        return io_fail;
    }

    // 提交：数据库、缓存的源文本、设置、revision、dirty、重建。
    bibliography_db_ = std::move(staged_db);
    bibliography_bibtex_ = bibtex_text;
    bibliography_revision_ = result.bibliography_revision;
    state_.mutable_settings().bibliography_path = "references.bib";
    bibliography_write_failed_ = false;
    // Bibliography 变更：ProjectRevision +1（架构 40）。
    state_.BumpRevision();
    MarkDirty();
    RequestBuild(false);
    return result;
}

CitationSearchResult ProjectSession::SearchCitations(const std::string& query) {
    CitationSearchRequest request;
    request.query = query;
    BibliographyService service(bibliography_db_);
    return service.Search(request);
}

} // namespace pf
