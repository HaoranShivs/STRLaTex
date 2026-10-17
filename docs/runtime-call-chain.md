# PaperForge 运行时调用链路与关键接口说明

> 适用代码：本仓库 `master`（本次审查清理之后）。
> 配套文档：[`code-review-2026-10.md`](code-review-2026-10.md)（审查结论与清理记录）、
> [`editor-design-review.md`](editor-design-review.md)（编辑器设计改进建议）、
> [`architecture-async.md`](architecture-async.md)（M1 线程模型的原始设计）。
>
> 本文只描述**运行时**真正会走的路径。每一个箭头都对应代码里的真实函数调用；
> 文件路径均相对仓库根目录，行号会随修改漂移，因此以**函数名**为准。

---

## 0. 阅读方法

PaperForge 的运行时由四个可独立观察的链路组成：

| 链路 | 触发者 | 终点 | 是否需要后台线程 |
| --- | --- | --- | --- |
| 编辑链路 | 用户在某个 Block 行输入并提交 | `Document` 被修改、`ProjectRevision +1` | 否 |
| 构建链路 | 每次编辑后自动 / 用户点 Build | `project.pdf` + `PreviewUpdate` + `BuildResult` | 是（BuildCoordinator worker） |
| 保存链路 | Ctrl+S / 30s 自动保存 / 关闭守卫 | `project.paper`、`references.bib`、autosave | 是（SaveCoordinator worker） |
| 打开/恢复链路 | Welcome 页或菜单 | `ProjectState` 就绪、视图重建 | 否（同步 IO） |

**先读 §2（线程与所有权），再看各链路**：所有链路的设计都围绕“可变状态只有一个
线程能碰、后台只能拿值对象”这一条规则展开。

---

## 1. 总览

```text
                        Qt GUI 线程（= application thread）
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │ main()                                                                        │
 │  └─ MainWindow                                                                 │
 │      ├─ WelcomePage                         （无项目时的入口）                │
 │      ├─ OutlinePanel / BlockEditor / PdfPreview / ProblemsPanel                │
 │      └─ ProjectController  ── Qt 信号 ──▶ MainWindow 的槽                       │
 │              │                                                                 │
 │              ▼                                                                 │
 │         ProjectSession   ← 应用聚合根：持有全部可变状态                         │
 │           ├─ ProjectState（ProjectId / ProjectRevision / Document / 模板）      │
 │           ├─ EditingSystem（EditCommand → DocumentEditor → UndoHistory）        │
 │           ├─ SnapshotFactory（深拷贝出不可变快照）                              │
 │           ├─ BuildCoordinator  ── worker 线程 ──▶ Validate → Render → Compile   │
 │           ├─ SaveCoordinator   ── worker 线程 ──▶ Serialize → 原子写             │
 │           └─ 事件邮箱（ApplicationEvent）+ autosave 定时线程                    │
 └──────────────────────────────────────────────────────────────────────────────┘
                 ▲ 后台只投递值对象：BuildResultReady / BuildEventReady /
                 │ SaveCompleted / BuildPhaseChanged / AutosaveTick
```

领域层（`src/{core,document,editing,project,persistence,build,render,...}`）对 Qt
一无所知；`src/app/` 是唯一的 Qt 适配层。这个边界在
`ProjectController.h` 的注释与 `src/app/CMakeLists.txt` 的依赖方向中都能验证。

---

## 2. 线程、所有权与异步边界

### 2.1 线程清单

| 线程 | 谁创建 | 职责 | 允许触碰 |
| --- | --- | --- | --- |
| GUI/application 线程 | `QApplication` | 接收输入、修改 `ProjectState`、应用异步事件 | 全部可变状态 |
| BuildCoordinator worker | `BuildCoordinator` 构造函数 | 校验、渲染 LaTeX、驱动编译器 | 仅 `BuildSnapshot`（不可变） |
| SaveCoordinator worker | `SaveCoordinator` 构造函数 | 串行写入 `project.paper` | 仅 `SerializedProject`（深拷贝） |
| autosave 定时线程 | `ProjectSession::StartAutosaveTimer` | 每 30s 投递 `AutosaveTickEvent` | 无（只投递事件） |
| MathRenderService worker | `MathRenderService` 构造函数 | 用 TeX 渲染行内公式位图 | 仅请求值对象 |
| Qt 内部线程 | Qt | 字体、图像、事件循环 | 由 Qt 管理 |

### 2.2 单一所有权规则

`ProjectSession` 在构造时记录 `owner_thread_`；`Execute`、`RequestBuild`、
`AcceptBuildResult`、`ProcessApplicationEvents`、`Save` 等入口调用
`NoteOwnerThreadUse()` 记录越界计数（`owner_thread_violations_`）。回归测试
`TestSessionAsync` 断言该计数为 0。

### 2.3 跨线程只允许值对象

| 方向 | 类型 | 定义位置 |
| --- | --- | --- |
| 应用线程 → build worker | `BuildSnapshot`（含 `shared_ptr<const Document>`） | `build/BuildCoordinator.h` |
| 应用线程 → save worker | `SerializedProject`（深拷贝） | `persistence/ProjectPersistence.h` |
| worker → 应用线程 | `ApplicationEvent`（`std::variant`） | `project/ApplicationEvent.h` |
| worker → GUI 的 Qt 信号 | `pf::BuildResult` / `pf::BuildEvent` / `pf::PreviewUpdate` | `app/ProjectController.h` |

`ApplicationEvent` 的五个成员：

```cpp
struct BuildPhaseChangedEvent { BuildPhase previous, current; };
struct BuildResultReadyEvent  { BuildResult result; };
struct BuildEventReadyEvent   { BuildEvent event; };
struct SaveCompletedEvent     { SaveCompletion completion; };
struct AutosaveTickEvent      {};
using ApplicationEvent = std::variant<...>;
```

### 2.4 事件邮箱与唤醒

- `ProjectSession::PostApplicationEvent()`：加锁入队 `pending_events_`，然后
  `notify_all` 并调用宿主的 `wake_handler_`；**不触碰任何可变状态**。
- `ProjectSession::ProcessApplicationEvents()`：只在应用线程调用，交换出事件
  数组后在锁外 `std::visit` 分发。
- GUI 侧 `ProjectController` 构造时安装：

  ```cpp
  session_->SetWakeHandler([this] {
      QMetaObject::invokeMethod(this, [this] { PumpEvents(); }, Qt::QueuedConnection);
  });
  ```

  这条 `QueuedConnection` 保证分发发生在 GUI 线程。另有 100ms 的
  `pump_timer_` 作为漏掉唤醒时的兜底（并非常规路径）。
- CLI/测试没有事件循环，使用 `ProjectSession::WaitForApplicationEvent(timeout)`
  阻塞轮询，语义与 GUI 相同。

---

## 3. 启动链路

### 3.1 GUI

```text
main()                                      src/app/main.cpp
 ├─ QApplication app; setApplicationName/Organization
 ├─ pf::gui::MainWindow window
 │   ├─ QApplication::setFont(theme::AppFont())
 │   ├─ new ProjectController(this)          ← 先建适配器
 │   │   ├─ qRegisterMetaType<PreviewUpdate/BuildResult/BuildEvent>()
 │   │   ├─ ProjectSession::Config{
 │   │   │     install_root = PF_INSTALL_ROOT,
 │   │   │     workspace_root = <tmp>/paperforge-gui-builds,
 │   │   │     debounce = 800ms }
 │   │   ├─ new ProjectSession(config)       ← 领域聚合根
 │   │   ├─ session_->SetWakeHandler(...)
 │   │   ├─ SetPhaseHandler / SetBuildResultHandler / SetBuildEventHandler
 │   │   │  SetPreviewUpdateHandler / SetSaveResultHandler
 │   │   └─ pump_timer_(100ms) → PumpEvents()
 │   ├─ QSettings 读取最近项目
 │   ├─ BuildUi()
 │   │   ├─ QStackedWidget: WelcomePage | workspace
 │   │   └─ workspace = QSplitter(Outline | BlockEditor | QSplitter(PdfPreview, Problems))
 │   ├─ BuildMenus()
 │   ├─ WireEditor()                         ← 编辑器信号 → ProjectController 命令
 │   └─ connect(controller_, ...)            ← 领域信号 → 视图刷新
 └─ window.show(); return app.exec()
```

`ProjectSession` 构造时按固定顺序完成接线：

1. `assets_`/`snapshot_factory_` 初始化；
2. `SaveCoordinator` 完成回调 → `PostApplicationEvent(SaveCompletedEvent)`；
3. `EditingSystem::Host` 接线（`project_id`/`revision`/`bump_revision`/
   `document`/`change_template`）；
4. 编译器：测试可注入 `Config::compiler_factory`；生产默认创建
   `TectonicCompiler`，但**实际构建使用 `BuildCoordinator` 的
   `compiler_provider`**（见 §7.3 与审查文档中的“不可达后端”条目）；
5. `RuntimeManager(install_root).Initialize()` 定位 `runtime/texlive`；
6. `BuildCoordinator::Host` 接线（工作目录、调试转储目录、完成/阶段/日志回调）；
7. 生产路径安装 `compiler_provider`（`CompilerFactory(texlive_root_).Create`）；
8. `set_debounce(config_.debounce)`。

### 3.2 CLI

`src/cli/Main.cpp` 只做命令分发；`src/cli/Commands.cpp` 直接驱动
`ProjectSession`（不需要 Qt）：

- `CmdNew`：`NewProject` → 通过 `Execute(EditCommand)` 写入初始文档 → `Save` +
  `FlushSaves`；
- `CmdBuild`：`OpenProject` → `SetBuildResultHandler` → `RequestBuild(true)` →
  循环 `WaitForApplicationEvent` 等待结果；
- `CmdInfo`：只读 `state().document()` 与注册表；
- `CmdDemo`：`CmdNew` + 公式/引文 + 构建。

CLI 是“领域层不依赖 Qt”的第二个消费者，也是分层是否真实有效的活证据。

---

## 4. 打开项目链路

入口：`MainWindow::OnOpenProject` / `OpenProjectDir`（Welcome 页、最近项目、
File → Open Project 共用）。

```text
MainWindow::OpenProjectDir(dir)
 ├─ MaybeSaveBeforeDestructiveNavigation()      ← P0-01 未保存更改守卫
 │   ├─ editor_->CommitFocused()
 │   ├─ session.ProcessApplicationEvents()
 │   └─ 依据 session.persistence_state() 询问 Save/Discard/Cancel
 │        （Dirty: 保存并 WaitForUserSaveCompletion；
 │          Saving: 等待或丢弃；SaveFailed: 重试或丢弃）
 ▼
ProjectController::OpenProjectWithRecovery(dir, &recovered)
 ▼
ProjectSession::OpenProjectWithRecovery(dir, error, recovered)
 ├─ 探测 project.paper / .paperforge/autosave/autosave.paper
 ├─ A. 有 project.paper → OpenProject()
 │     └─ 若 HasRecoverySnapshot()（autosave mtime ≥ project）→ RecoverFromAutosave()
 ├─ B. 只有 autosave → SetProjectPaths + Reset state + RecoverFromAutosave()
 └─ C. 都没有 → 返回错误
 ▼
ProjectSession::OpenProject(dir, error)
 ├─ CloseProject()                       ← 取消 build、排空 save、清空状态
 ├─ SetProjectPaths(dir)                 ← 唯一的路径推导入口（project_file/assets/
 │                                          build/autosave）
 ├─ EnsureDirectories()                  ← 四个目录逐个创建，任一失败即结构化错误
 ├─ ProjectPersistence::Load(project_file)
 │   ├─ 32MB 上限 → 读取 → Json 解析（深度/类型校验）
 │   ├─ ProjectMigrator::MigrateToCurrent（V1/V2/V3 → V4，只在内存中）
 │   └─ SerializedProject
 ├─ state_.Reset(); SetId; SetRevision; document = sp.document;
 │  template/binding/settings; LoadTemplateCatalog(); assets Register
 ├─ 读 bibliography（经 ResolveUntrustedProjectPath 信任边界）
 ├─ lifecycle = Open; persistence = Clean; preview = NoPreview
 └─ RestoreProjectPreview()              ← 若存在 project.pdf，显示为 Stale
 ▼
ProjectController 发出 projectChanged / documentChanged / stateChanged
 ▼
MainWindow::OnProjectChanged
 ├─ 重置 build/preview 按钮与 last_built_revision_
 ├─ RefreshTemplateChoices()
 └─ 若有上次成功 PDF：current_pdf_path_ + RefreshPreview()
MainWindow::RefreshDocumentView
 ├─ ProblemsPanel::SetStale(...)
 ├─ editor_->SetCitationNumbers(controller_->CitationNumbers())
 ├─ 若有未提交输入 / 正在同步刷新 → 只刷新侧栏，推迟重建
 └─ editor_->RebuildFromDocument(doc); RefreshSidePanels()
```

**关键数据**：`SerializedProject`（JSON 的 C++ 镜像）、`SessionPaths`
（五个固定路径）、`ProjectState`（唯一可变聚合根）。

**关键不变式**：

- 只有 `project.paper` 存在且 `EnsureDirectories()` 成功，`lifecycle_state_`
  才能是 `Open`；
- 加载期间**绝不重写磁盘**（migration 只在内存中完成）；
- 恢复出来的内容一定处于 `Dirty`（尚未被用户显式保存）。

---

## 5. 编辑链路（GUI → Document）

### 5.1 输入的三种提交触发器

`BlockEditor` 把文档投影成一组“行”（`Block` 结构，见 `BlockEditor.h`）：

| 行类型 | 控件 | 提交时机 |
| --- | --- | --- |
| Paper Title/Authors/Affiliations/Abstract/Keywords/Section/Subsection/Subsubsection/Equation/Caption | `BlockEdit : QPlainTextEdit` | 失焦、Enter（单行）、`CommitFocused()` |
| Text（段落） | `InlineEditor : QTextEdit` | 失焦、Ctrl+Enter、选择器插入后立即提交、`CommitFocused()` |
| Figure/Table 的图片与表格 | 只读预览 + 题注编辑器 | 题注走普通 `BlockEdit` 规则 |

必须存在 `CommitFocused()` 的调用点（否则 snapshot 会漏掉用户刚敲的内容）：
`MainWindow::OnSave`、`OnBuild`、`OnImportBibliography`、`OnChangeTemplate`、
`OnAddReference`、`OnImportTemplate`、`MaybeSaveBeforeDestructiveNavigation`、
`MainWindow::~MainWindow`。

### 5.2 `BlockEditor::CommitBlock` —— 行级提交的唯一出口

```cpp
void BlockEditor::CommitBlock(Block& block) {
    if (rebuilding_) return;
    if (block.inline_editor) {                    // Text 行
        InlineContent content = block.inline_editor->Content();
        if (content == block.committed_content) { MarkClean; return; }  // 无变化
        block.committed_content = content;
        block.committed_text = InlineToPlainText(content);
        emit ParagraphContentEdited(block.node_id, content);
        emit RowCommitted();
        return;
    }
    QString text = block.editor->toPlainText();
    if (IsProseRole(block.commit_role)) text = ReflowHardWrappedText(text);
    if (text == block.committed_text) { MarkClean; emit RowCommitted(); return; }
    block.committed_text = text;
    emit TitleEdited / AuthorsEdited / ... / CaptionEdited / EquationEdited;
    emit RowCommitted();
}
```

- `committed_content` / `committed_text` 是“文档已经知道的值”，用于丢弃
  重复提交——这正是防止 `edit → rebuild → edit` 循环的关键；
- `rebuilding_` 期间禁止提交（析构/重建回调里的失焦提交会被丢弃）；
- `CommitInlineRow` 现在直接复用 `CommitBlock`，两条提交路径只有一份实现。

### 5.3 信号 → 命令（`MainWindow::WireEditor`）

```text
ParagraphContentEdited(node, InlineContent) → ProjectController::EditParagraphRich
EquationEdited(node, math, numbered, label) → EditEquation
SectionRenamed/SubsectionRenamed/SubsubsectionRenamed → RenameSection/Subsection/Subsubsection
Title/Authors/Affiliations/Abstract/Keywords → SetXxx
CaptionEdited → EditCaption；FigureSpanChanged → EditFigureSpan
InsertBlockRequested / DeleteBlockRequested / MoveBlockRequested
MoveBlockToRequested → MoveNodeAfter
AuthorAffiliationToggled → SetAuthorAffiliation
```

`ProjectController` 是 Qt 侧唯一的命令构造器：

```cpp
EditCommand ProjectController::MakeCmd(FullEditPayload payload) const {
    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id   = session_->state().id();
    cmd.base_revision = session_->current_revision();
    cmd.origin = EditOrigin::User;
    cmd.payload = std::move(payload);
    return cmd;
}
```

`ExecuteAndNotify(payload)` = `session_->Execute(MakeCmd(...))` +
成功时 `EmitDocumentChanged()`。**所有成功提交都走这一条路径**，因此
“命令构造、revision 绑定、视图通知”不会各写一份。

### 5.4 命令信封与载荷

`editing/EditCommand.h`：

```cpp
struct EditCommand {
    OperationId operation_id;
    ProjectId project_id;
    ProjectRevision base_revision;   // 乐观并发检查
    EditOrigin origin;               // User/Undo/Redo/Recovery/System
    FullEditPayload payload;         // std::variant<... 34 种>
};
```

- `FullEditPayload` = 文档载荷 + `ChangeTemplatePayload`；
  `EditPayload` = 去掉模板变更后的子集（`DocumentEditor` 只接受后者）。
- 结果 `EditResult{ status, new_revision, created_node, failure, detail }`，
  失败原因枚举 `FailureReason{StaleOperation, WrongProject, InvalidTarget,
  ConstraintViolation, UnknownPayload, None}`。

**并发契约**：`EditingSystem::Apply` 先比较 `command.project_id` 与
`base_revision`；不匹配直接 `StaleOperation`/`WrongProject`，**绝不半应用**。
GUI 总是在发出命令前读取当前 revision，因此正常路径不会触发；该检查保护的
是异步/延迟到达的命令。

### 5.5 `EditingSystem::Apply`

```text
Apply(command)
 ├─ 校验 project_id / base_revision
 └─ ApplyFullPayload(command)
     ├─ ChangeTemplatePayload：
     │    host_.change_template(payload) → ResolveTemplate + SetTemplateBinding
     │    → bump_revision + Notify + 写入 TemplateHistoryAction
     └─ 其他载荷：
          before = make_shared<const Document>(doc)      ← 整篇深拷贝
          ApplyDocumentPayload(cmd, payload)
            └─ DocumentEditor editor(doc); 按 variant 分派
               finish/finishVoid/finishMeta:
                 host_.bump_revision() + Notify(DocumentChangedEvent)
          after = make_shared<const Document>(doc)
          history_.Push(SnapshotHistoryAction{before, after})
```

`Notify` 构造 `DocumentChangedEvent{project_id, old/new revision, origin,
affected_nodes, change_kinds}`；`ProjectSession` 目前不再安装
`on_document_changed`（GUI 的刷新由 `ProjectController` 在命令返回后触发），
该回调仍保留给测试宿主使用。

### 5.6 `DocumentEditor` —— 结构约束的唯一执行者

`DocumentEditor` 是 `Document` 的友元，负责：

- 前置信息：标题/摘要/关键词/作者/机构；
- 结构：Section/Subsection/Subsubsection 的插入、删除、移动、重命名；
- 块：`InsertBlock`/`DeleteBlock`/`MoveBlock`、段落内容、题注、公式源码、
  图片、表格；
- 表格矩形不变量：`MakeTable` + 行列增删，`IsRectangular()` 是写入后必须
  成立的条件；
- 每次成功变更调用 `document_.BumpVersion()`（`DocumentVersion` 单调递增）。

它**不**知道 undo、build、save、PDF、Qt（头文件注释与 include 列表都体现）。

### 5.7 变更通知与视图刷新

```text
ProjectController::ExecuteAndNotify
 └─ EmitDocumentChanged() → emit documentChanged(); EmitProjectStateChanged();
     └─ MainWindow::RefreshDocumentView()
         ├─ has_built_ ? ProblemsPanel::SetStale(revision != last_built_revision_)
         ├─ editor_->SetCitationNumbers(controller_->CitationNumbers())
         ├─ 若 editor_change_in_progress_   → RefreshSidePanels(); return
         ├─ 若 editor_->HasUncommittedFocus() → pending_structural_refresh_ = true; 侧栏; return
         └─ editor_->RebuildFromDocument(doc); RefreshSidePanels()
```

`RefreshSidePanels()` = Outline 重建 + 必填提示 + 引用条目 + 字数统计。
`BlockEditor::RebuildFromDocument` 会：

1. 记录当前聚焦行的 `row_focus_key` 与光标位置（`focus_node_`/`focus_pos_`）；
2. 清空并重建所有卡片（FrontMatter 行 + `CreateBlockCard`/`AppendBlocks`）；
3. 若之前有焦点，恢复对应行与光标位置。

**输入保护**：只要焦点行有未提交内容，就推迟重建（`pending_structural_refresh_`），
待 `RowCommitted` 到达后再刷新；否则重建会销毁正在输入的行。

---

## 6. 撤销 / 重做链路

```text
MainWindow::OnUndo/OnRedo（Edit 菜单/Ctrl+Z/Ctrl+Shift+Z）
 └─ ProjectController::Undo/Redo → ApplyHistory(bool redo)
     ├─ template_before = state.template_selection()
     ├─ session_->Undo()/Redo()
     │   └─ ProjectSession::Undo/Redo → FinalizeEdit(editing_.Undo/Redo())
     │       └─ EditingSystem::Undo/Redo → ApplyHistoryEntry(entry, origin)
     │           ├─ SnapshotHistoryAction：doc = *before（undo）或 *after（redo）
     │           │    + bump_revision + Notify
     │           └─ TemplateHistoryAction：重建 ChangeTemplatePayload 并 ApplyFullPayload
     ├─ 模板选择变化 → emit templateChanged
     └─ EmitDocumentChanged() → 视图整篇重建
```

- `UndoHistory` 只保存**整篇文档前后快照**（`shared_ptr<const Document>`），
  因此删除 Section 等破坏性操作也能完整恢复；
- Undo/Redo 本身也产生新的 `ProjectRevision`（不是回退 revision）；
- 编辑器刷新会重建行（`OnUndo` 里显式调用 `RefreshDocumentView()`），
  因此撤销后光标位置不保留（设计改进项之一）。

---

## 7. 构建链路

### 7.1 触发、去抖与 latest-wins

- **自动**：每次命令成功 → `ProjectSession::Execute` → `FinalizeEdit` →
  `MarkDirty()` + `RequestBuild(false)`；
- **手动**：`MainWindow::OnBuild`（Ctrl+B）→ `editor_->CommitFocused()` →
  `RequestBuild(true)`；构建中再点一次 → `CancelBuild()`。

```text
ProjectSession::RequestBuild(manual)
 ├─ lifecycle != Open ? return
 ├─ snapshot = SnapshotFactory::CreateBuildSnapshot(state_, bibliography_bibtex_)
 │   · shared_ptr<const Document> 深拷贝
 │   · asset_files / asset_sources 映射
 │   · snapshot_id / build_id（IdGenerator）
 ├─ snapshot.resolved_template = ResolveTemplate(template_selection)
 ├─ toolchain = {engine, bibliography_engine}
 ├─ 模板绑定校验（external template → VerifyTemplateBinding）
 ├─ latest_snapshot_id_ / latest_build_id_ ← 本次请求的身份
 └─ BuildCoordinator::RequestBuild(snapshot, manual)
```

`BuildCoordinator` 只有 `optional<BuildSnapshot> pending_`，**没有队列**：

- 若 worker 正在构建，新请求替换 `pending_`（latest-wins）；
- `manual == false` 时 `due_ = now + debounce(800ms)`；手动构建立即执行；
- 被替换的请求永远不会产生 `BuildResult`（其 build id 也不会被接受）。

### 7.2 worker：`BuildCoordinator::BuildOne`

```text
SetPhase(Rendering)
 ├─ Validator::Validate(ValidationInput)
 │    ├─ 结构/语义/模板诊断（Diagnostic）
 │    └─ can_render == false → 直接 Failure（不进入编译）
 ├─ LatexRenderer::Render(RenderRequest)
 │    ├─ LatexPlanner::Plan → LatexGenerationPlan
 │    │    · LatexNode IR（Literal/Text/Identifier/AssetPath/InlineMath/
 │    │      DisplayMath/Command/Environment）
 │    │    · BuildPackage（main.tex、references.bib、模板资源、toolchain）
 │    │    · plan audit JSON（渲染计划的可审计导出）
 │    └─ LatexEmitter::Emit(plan) → RenderResult{package, SourceMap, diagnostics}
 ├─ debug_dump_dir 存在时把 package 与 asset 复制到 <project>/.paperforge/build
SetPhase(Compiling)
 ├─ CompileRequest{package, workspace, asset_sources, toolchain,
 │                 on_output = 实时日志分片}
 ├─ ICompiler::Compile（生产：TexLiveCompiler → latexmk + 自带 TeX Live）
 │    ├─ 写工作区、设置 PATH/HOME/TEXMF* 隔离用户 TeX
 │    ├─ 子进程流式输出 → EmitEvent(BuildingLog)
 │    ├─ 取消轮询 / 超时
 │    └─ CompileResult{pdf, log, messages, failure_kind, exit_code}
 ├─ DiagnosticMapper + SourceMap：编译器行号 → NodeId → Diagnostic
 └─ BuildResult{outcome, failure_kind, project/snapshot/build/revision,
                pdf_path, log, diagnostics, started/finished_ms, exit_code}
     └─ PostApplicationEvent(BuildResultReadyEvent)
```

阶段事件 `BuildPhaseChangedEvent` 与日志事件 `BuildEventReadyEvent` 走同一个
邮箱；GUI 侧 `MainWindow::OnBuildEvent` 用 `BuildStarted/Succeeded/Failed/
Cancelled` 驱动按钮的 Cancel 态与 Problems 面板。

### 7.3 结果回传与预览门禁

```text
ProjectController::PumpEvents（wake handler / 100ms timer）
 └─ ProjectSession::ProcessApplicationEvents()
     └─ HandleEvent(BuildResultReadyEvent)
         └─ AcceptBuildResult(result)
             ├─ PreviewGate: EvaluatePreviewGate(CurrentGateInput(), result)
             │    project_id → revision → snapshot_id → build_id 全匹配才 Accept
             ├─ Success:
             │    preview_pdf_ = {path, build_id, revision}
             │    ProjectPdf::Save(project_dir, pdf)   ← 原子复制为 project.pdf
             │    preview_state_ = Fresh
             ├─ Failure: preview_state_ = Stale（保留上次成功 PDF）
             └─ 回调：build_result_handler_ → GUI buildCompleted
                       preview_update_handler_ → GUI previewUpdated
```

`PreviewGate` 是纯函数（`project/PreviewGate.h`），判定结果
`Accept/NoProject/ForeignProject/StaleRevision/StaleSnapshot/StaleBuild`。
过期结果在这里被丢弃，**绝不进入** `PreviewState` 或 GUI。

### 7.4 预览显示（`PdfPreview`）

```text
MainWindow::OnPreviewUpdated(update)
 ├─ 再次校验 project_id / revision（防御性，gate 已经校验过）
 ├─ 成功 → Build ✓；更新 current_pdf_path_；RefreshPreview()
 └─ 取消 / 失败 → 只改按钮文案，不动 PDF
PdfPreview::SetDocument(pdf)
 ├─ pdfinfo → 页数；pdftoppm 渲染首页探针 → 页面尺寸（pt）
 ├─ 为每页创建 QLabel 占位
 ├─ FitWidth()（默认跟随窗格宽度）
 └─ 惰性渲染：只为视口 ±2 页保留位图；缩放后防抖 140ms 重新光栅化
```

**注意**：`pdfinfo`/`pdftoppm` 通过 `QProcess::waitForFinished` **同步**执行，
而调用发生在 GUI 线程（`SetDocument`、`resizeEvent`、滚动/缩放重渲染）。
这是当前实现的一个可感知卡顿点，列入设计改进建议。

---

## 8. 保存、自动保存与恢复

### 8.1 手动保存

```text
MainWindow::OnSave（Ctrl+S）
 ├─ editor_->CommitFocused()
 ├─ ProjectSession::Save()
 │   ├─ CaptureSaveSnapshot() → SnapshotFactory::CreateSaveSnapshot(state_)
 │   ├─ save_coordinator_.Enqueue(serialized, project_file, SaveKind::User)
 │   │    · 返回 nullopt（正在 Shutdown）→ 如实报错，绝不伪造 Queued
 │   ├─ persistence_state_ = Saving
 │   └─ bibliography side-car：ResolveUntrustedProjectPath + 原子写 references.bib
 │        （失败 → bibliography_write_failed_，完成时不得报 Clean）
 └─ 入队失败 → 状态栏错误；否则 RenderProjectState()
```

SaveCoordinator worker：

```text
WorkerLoop（FIFO，单线程）
 └─ RunTask → ProjectPersistence::Save
     ├─ Serialize(SerializedProject) → JSON
     ├─ 写 <dest>.tmp-<save_id>
     ├─ 读回 + Deserialize 往返校验（失败即删除临时文件）
     ├─ rename 覆盖目标（原子替换）
     └─ 若目标是 project.paper 且有模板绑定 → 写 template.lock.json side-car
 → SaveCompletion{save_id, project_id, revision, kind, outcome, result}
 → 回调 → PostApplicationEvent(SaveCompletedEvent)
 → ApplySaveCompletion：
     · project 不匹配 / 已关闭 → 只转发结果
     · User：last_user_save_result_；Saved 且无 bib 失败且
       completion.revision == state_.revision() → Clean
     · Superseded → 不动状态（由更新的保存决定）
     · 失败 → 当前 revision 匹配才 SaveFailed
```

**为什么 revision 要参与判定**：save rev20 → 用户继续编辑到 rev21 → rev20
完成时不能把项目标成 Clean；只有写入的 revision 等于当前 revision 才允许。

### 8.2 自动保存

```text
ProjectSession::StartAutosaveTimer(30s)
 └─ 定时线程（分 100ms 片睡眠）
     └─ PostApplicationEvent(AutosaveTickEvent)
         └─ HandleEvent → lifecycle Open && persistence Dirty ? Autosave() : 忽略
             └─ 同 Save，但写入 .paperforge/autosave/autosave.paper，kind=Autosave
```

自动保存**从不**改变 `Clean/Dirty`（架构 32）。

### 8.3 崩溃恢复

`OpenProjectWithRecovery` 比较 `autosave.paper` 与 `project.paper` 的 mtime：

- 仅存在 autosave（项目从未保存）→ 直接以 autosave 作为项目内容（P0-01 修复）；
- autosave 更新 → 先正常打开，再 `RecoverFromAutosave()` 覆盖内存状态；
- 恢复后 `persistence_state_ = Dirty`，GUI 弹窗告知用户。

---

## 9. 模板链路

```text
ChangeTemplate（顶栏下拉框）
 └─ ProjectController::ChangeTemplate → ProjectSession::ChangeTemplate
     └─ Execute(ChangeTemplatePayload{template_id, binding})
         └─ EditingSystem → host_.change_template
             ├─ ResolveTemplate(id)（内置 + 项目导入目录）
             ├─ binding 必须与模板定义一致
             └─ state_.SetTemplateBinding(binding)
 → TemplateHistoryAction 入 undo；DocumentVersion 不变、ProjectRevision +1
 → RequestBuild(false)（编译引擎由新模板决定）
```

导入外部模板：

```text
File → Import Template (.zip)
 └─ TemplateImportDialog（后台解析/安装，进度与错误展示）
     ├─ InspectTemplateArchive：路径、类型、大小、CRC 全部通过后才解包
     ├─ 选择 profile → 安装到项目模板目录（已存在的包只校验、不覆盖）
     └─ ProjectSession::InstallVerifiedTemplate → LoadTemplateCatalog
```

构建时 `RequestBuild` 会对 `external` 模板执行 `VerifyTemplateBinding`
（检测磁盘上的包是否被改动），失败写入 `snapshot.template_error`。

---

## 10. 参考文献链路

| 操作 | 入口 | 领域路径 |
| --- | --- | --- |
| 导入 .bib | `MainWindow::OnImportBibliography` | `ProjectSession::ImportBibliography` → `StoreBibliography`（解析 → 信任边界路径 → 原子写 → 提交数据库/字节/settings；`BumpRevision`；`MarkDirty`；`RequestBuild`） |
| 逐条新增 | `AddReferenceDialog` | `ProjectSession::AddBibliographyEntry`（序列化单条 → 查重 → 追加到现有 .bib → `StoreBibliography`） |
| 引用插入 | Text 行工具栏或 `@` 菜单 | `InlineEditor::InsertCitationObject` → 提交 → `EditParagraphRich`（唯一数据路径） |
| 编号显示 | 每次 `RefreshDocumentView` | `ProjectController::CitationNumbers()` → `CitationNumberResolver::Build` → 各行的 citation pill |

`CitationNumberResolver` 按文档阅读顺序给首次引用的 key 编号，复现
`unsrtnat`/`IEEEtran` 的 citation-order 结果；未知 key 显示 `[?]` 且不占号。

---

## 11. 图片资源链路

```text
Insert Figure（块间 + / 块菜单）
 └─ MainWindow::InsertBlockRequested → QFileDialog
     └─ ProjectController::InsertFigureAfter → ProjectSession::InsertFigureFromSource
         ├─ ImportAsset → AssetManager::Stage
         │    · 校验类型（仅位图）→ 复制到 project/assets/ → 内容哈希/尺寸嗅探
         ├─ AssetManager::Register（暂存 → 注册分离）
         ├─ 重新解析插入锚点（当前 revision）
         └─ Execute(InsertFigurePayload) → InsertFigure
```

`AssetManager` 只保证文件在项目内、元数据可序列化；文档只保存 `AssetId`，
不保存绝对路径。

---

## 12. 关键数据结构索引

| 数据结构 | 位置 | 作用 |
| --- | --- | --- |
| `StrongId<Tag>` / `NodeId`/`ProjectId`/`AssetId`/`AffiliationId`/`BuildId`/`SaveId`/`OperationId` | `core/StrongId.h` | 强类型 ID，防串用 |
| `ProjectRevision` / `DocumentVersion` | `core/StrongId.h` / `document/Document.h` | 单调版本；revision 管项目、version 管文档 |
| `Result<T,E>` / `Unexpected` | `core/Result.h` | 领域错误返回 |
| `Diagnostic` | `core/Diagnostic.h` | 结构化诊断（严重级别、节点/文件位置、code、message） |
| `Document` / `FrontMatter` / `Body` / `Section` / `Subsection` / `Subsubsection` | `document/Document.h` | 闭式语义 schema |
| `Block = variant<Paragraph,Figure,Table,EquationBlock>` | 同上 | 四类块 |
| `InlineNode = variant<TextRun,InlineMath,Citation,CrossReference>` | 同上 | 行内语义节点 |
| `NodeAddress` | `document/DocumentTraversal.h` | 节点坐标（section/subsection/subsubsection/block 下标） |
| `EditCommand` / `FullEditPayload` / `EditResult` | `editing/EditCommand.h` | 编辑协议信封 |
| `HistoryEntry = variant<SnapshotHistoryAction,TemplateHistoryAction>` | 同上 | undo 记录 |
| `DocumentChangedEvent` | `editing/EditingSystem.h` | 变更通知（revision 前后、affected nodes） |
| `ProjectState` | `project/ProjectState.h` | 可变聚合根 |
| `BuildSnapshot` / `BuildResult` / `BuildPhase` | `build/BuildCoordinator.h` | 构建输入/输出/阶段 |
| `CompileRequest` / `CompileResult` / `CompilerMessage` | `build/Compiler.h` | 编译器接口 |
| `BuildPackage` / `LatexNode` / `SourceMap` | `render/*.h` | 渲染产物与行→节点映射 |
| `SerializedProject` / `SaveRequest` / `LoadResult` | `persistence/ProjectPersistence.h` | 磁盘格式 |
| `SaveTask` / `SaveCompletion` / `SaveOutcome` | `persistence/SaveCoordinator.h` | 保存作业与结果 |
| `ApplicationEvent` | `project/ApplicationEvent.h` | 跨线程邮箱 |
| `PreviewGateInput` / `PreviewGateDecision` | `project/PreviewGate.h` | 陈旧结果门禁 |
| `PreviewUpdate` / `PdfArtifact` | `project/PreviewUpdate.h` | 带身份的预览事件 |
| `TemplateDefinition` / `TemplateBinding` | `template/TemplateRegistry.h` / `TemplateBinding.h` | 模板能力与固定绑定 |
| `MathRenderResult` / `MathRenderStyle` | `app/MathPreviewRenderer.h` | 公式渲染值对象 |

---

## 13. 关键接口契约（可直接作为测试断言）

1. **revision 单调**：`ProjectRevision` 只能通过 `ProjectState::BumpRevision()`
   增长；Undo/Redo/模板切换/参考文献变更都产生新 revision。
2. **陈旧命令拒绝**：`EditCommand.base_revision != 当前 revision` →
   `EditStatus::Rejected + FailureReason::StaleOperation`，文档不变。
3. **模板切换不改文档**：`DocumentVersion` 不变，`ProjectRevision +1`。
4. **后台只读值对象**：worker 线程不存在对 `ProjectState`/`Document` 的引用。
5. **latest-wins**：仍在 `pending_` 的旧请求不会开始构建；已经在 worker 上
   运行的旧请求即使跑完，其结果也会被 `request_generation_` 与
   `PreviewGate` 丢弃，绝不进入 UI。
6. **保存不覆盖新内容**：save worker 串行，旧 revision 完成时不得把项目
   置为 Clean；`Superseded` 不是错误。
7. **原子写**：`project.paper` / `references.bib` / `project.pdf` 都通过
   “临时文件 → 校验/rename”替换；失败保留旧文件。
8. **autosave 不改状态**：只写 `.paperforge/autosave/`，Clean/Dirty 不变。
9. **信任边界**：所有来自 `project.paper` 的相对路径都必须经
   `ResolveUntrustedProjectPath` 解析后才能读写。
10. **渲染器无文档知识反向依赖**：`Document Core` 不包含 `\section`/`\label`
    等字符串；全部在 `render/` 生成。

---

## 14. 诊断与错误流

```text
Validator / DiagnosticMapper / Persistence
   → Diagnostic{source, severity, code, location, message, build_id, revision}
   → BuildResult.diagnostics
   → ProblemsPanel（severity 分组、过滤、Build Log Tab）
   → 双击 Diagnostic → MainWindow::OnProblemActivated
       ├─ 有 block 定位且节点仍存在 → editor_->RevealNode
       ├─ 节点已被删除 → 状态栏提示，不崩溃
       └─ 否则 → ProblemsPanel::ShowBuildLog()
```

GUI **从不解析日志文本**；Problems/Build Log 都消费结构化值对象。
保存错误通过 `SaveResult.status/detail` + `SaveOutcome` 区分
“被取代/IO 错误/序列化错误/关闭中”。

---

## 15. 文件索引（按链路顺序）

| 链路 | 主要文件 |
| --- | --- |
| 启动 | `app/main.cpp`、`app/MainWindow.cpp`、`app/ProjectController.cpp`、`project/ProjectSession.cpp` |
| 编辑 | `app/BlockEditor.cpp`、`app/InlineEditor.cpp`、`project/ProjectController.cpp`、`editing/EditingSystem.cpp`、`document/DocumentEditor.cpp` |
| 遍历/渲染 | `document/DocumentTraversal.cpp`、`render/LatexRenderer.cpp`、`render/LatexPlanner.cpp`、`render/LatexEmitter.cpp`、`render/SourceMap.cpp` |
| 构建 | `build/BuildCoordinator.cpp`、`build/Compiler.cpp`、`build/CompilerFactory.cpp`、`build/DiagnosticMapper.cpp`、`build/RuntimeManager.cpp` |
| 预览 | `project/PreviewGate.cpp`、`app/PdfPreview.cpp`、`persistence/ProjectPdf.cpp` |
| 保存/恢复 | `project/SnapshotFactory.cpp`、`persistence/ProjectPersistence.cpp`、`persistence/SaveCoordinator.cpp`、`persistence/ProjectMigrator.cpp` |
| 模板 | `template/TemplateRegistry.cpp`、`template/TemplatePackage.cpp`、`app/TemplateImportDialog.cpp` |
| 文献 | `bibliography/BibTeX.cpp`、`bibliography/BibliographyService.cpp`、`numbering/CitationNumberResolver.cpp` |
| 资源 | `asset/AssetManager.cpp` |
| CLI | `cli/Main.cpp`、`cli/Commands.cpp` |
