# 编辑器设计审查：PaperForge 与成熟编辑器实现的差距

> 审查对象：本仓库 `master`（本轮冗余清理之后）。
> 视角：**编辑器软件设计**——文档模型/视图、编辑事务与撤销、输入法、性能、
> 可靠性、可访问性与国际化、扩展性。
> 相关文档：[运行时调用链路](runtime-call-chain.md)、
> [冗余清理记录](redundancy-cleanup.md)、[审查总报告](code-review-2026-10.md)。

---

## 0. 结论先行

PaperForge 的**领域层**成熟度明显高于同类原型：语义文档模型与 LaTeX 渲染彻底
分离、单一所有者 + 不可变快照 + typed event、预览门禁、原子保存与回读校验、
结构化诊断 + SourceMap、真实的 TeX 回归测试，这些都应当保留。

差距集中在**编辑器交互层与运行时效率**，而不是数据模型：

1. **视图是全量重建的**（`BlockEditor::RebuildFromDocument`），可靠性与性能都
   受制于“不要在用户输入时重建”的补丁式约束；
2. **撤销是整篇快照且无上限**，每次提交还要做两次全文深拷贝；
3. **提交发生在失焦/显式调用时**，未提交文本只存在于控件里，崩溃即丢失；
4. **PDF 预览与启动期 TeX 健康检查是同步阻塞 GUI 的**；
5. **文档级派生数据（引用编号/字数/大纲）每次变更全量重算**。

如果只能做三件事：**增量视图 → 撤销/提交模型 → 异步化 PDF 与派生数据**。

---

## 1. 评价维度与现状打分

| 维度 | 成熟编辑器应具备 | PaperForge 现状 | 自评 |
| --- | --- | --- | --- |
| 文档模型 | 结构稳定、可序列化、ID 稳定 | 闭式 schema + `variant` + 强 ID；无 List/Footnote（V1 范围） | ★★★★☆ |
| 模型/视图分离 | 视图增量更新、与模型解耦 | 领域层干净；GUI 全量重建 | ★★☆☆☆ |
| 编辑事务/撤销 | 可合并、可逆、有上限、恢复选区 | 全文前后快照；无合并、无上限 | ★★☆☆☆ |
| 输入可靠性 | 输入即持久、崩溃可恢复 | 失焦才提交；autosave 只含已提交内容 | ★★☆☆☆ |
| 光标/选区 | 跨操作保持、可恢复 | 同一行内保持；重建/撤销后丢失 | ★★☆☆☆ |
| 大文档性能 | 虚拟化、增量布局、异步渲染 | 全量控件、全量遍历；公式异步、PDF 同步 | ★★☆☆☆ |
| 输入法 | 组合输入不打断 | 无显式 IME 处理（Qt 默认） | ★★☆☆☆ |
| 剪贴板/拖放 | 富文本与对象保真 | 自定义 MIME + 标记保留；图片粘贴缺失 | ★★★☆☆ |
| 查找/替换 | 全文查找、正则、跨块 | 无 | ★☆☆☆☆ |
| 拼写/语法 | 拼写检查、词典 | 无 | ☆☆☆☆☆ |
| 可访问性 | 可读名称、键盘可达、高对比 | 无 accessibleName；键盘仅部分 | ★☆☆☆☆ |
| 国际化 | `tr()`、RTL | 0 处 `tr()`，英文硬编码 | ★☆☆☆☆ |
| 主题 | 可切换、集中样式 | 设计 token + 大量内联 QSS | ★★☆☆☆ |
| 可观测性 | 结构化日志/追踪 | BuildEvent 流；无模块日志 | ★★☆☆☆ |
| 扩展性 | 模板/插件边界清晰 | 模板资源 + 固定 C++ adapter | ★★★☆☆ |
| 测试 | 单元/集成/真实链路/无静默跳过 | 覆盖好，但存在环境缺失静默通过 | ★★★☆☆ |

---

## 2. 应当保留的设计（改进时不要破坏）

1. **语义模型与 LaTeX 完全分离**：`Document Core` 不含 `\section` 等知识，
   渲染在 `render/`。
2. **单一所有者 + 不可变快照 + typed 事件**：`ProjectSession`、`BuildSnapshot`、
   `ApplicationEvent`、`PreviewGate` 这套组合是多线程编辑器里少见的干净实现。
3. **预览携带身份**：`PreviewUpdate{project_id, build_id, revision, pdf}` 让
   “旧结果不能污染新文档”成为类型层面的约束。
4. **原子保存 + 回读校验**：`ProjectPersistence::Save` 的临时文件 → 反序列化
   校验 → rename，比多数编辑器更严格。
5. **信任边界**：`ResolveUntrustedProjectPath` 对所有来自项目文件的相对路径
   统一收口。
6. **结构化诊断 + SourceMap**：GUI 不解析日志文本。
7. **真实 TeX 回归**：`IeeeMarksReachPdfLatex`、`BoldAndItalicReachTheBuiltPdf`
   等用例把“LaTeX 真的编译了吗”变成可执行断言。
8. **输入保护**：有未提交输入时推迟重建，避免输入被销毁。

---

## 3. 高优先级改进（P1）

### P1-01 视图全量重建：`RebuildFromDocument` 是性能与状态的共同瓶颈

**现状**（`app/BlockEditor.cpp`）

- `RebuildFromDocument(doc)` 清空 `host_` 布局并重建**每一个**卡片：
  `MakeCard` → `MakeTextCard`/`MakeEquationCard`/`MakeFigureCard`/
  `MakeTableCard` → 新建 `QWidget`、`InlineEditor`、`QLabel`、`QToolButton`；
- 为抵消重建代价，写入了多条“不要在此时重建”的规则：
  `MainWindow::RefreshDocumentView` 在 `HasUncommittedFocus()` 时设置
  `pending_structural_refresh_`，`RowCommitted` 后再补一次；
- 焦点靠 `row_focus_key` + 光标位置手工恢复，滚动位置不恢复；
- 每次结构变更（插入/删除/移动/章节重命名/撤销）都会触发。

**问题**

- 100 段以上的文档，每次结构变更创建数百个控件，明显卡顿；
- 重建会销毁 `QTextDocument`：行内撤销栈、选区、正在进行的输入法组合全部丢失；
- 规则分散在调用点（`OnUndo` 手动 refresh、`RowCommitted` 补刷新），容易漏；
- “重建 vs 不重建”的判断与领域无关，却由 `MainWindow` 承担。

**成熟做法**

模型-视图分离：以 `NodeId` 为键维护行视图缓存（`NodeId → RowWidget`），结构
变更做增量 diff（新增/删除/移动），文本变更只更新对应行；或者使用
`QListView`/`QAbstractItemModel` + delegate 做虚拟化，只为视口内的行创建控件。

**建议**

1. 引入 `RowRegistry`（`NodeId → 行控件`）与 `ApplyDocumentDiff(oldDoc, newDoc)`；
   首版可只处理“新增/删除/移动”三类结构变化，文本变化走现有 `SetContent`；
2. `RebuildFromDocument` 仅保留给“打开项目/模板切换/Undo”这类全量场景；
3. 焦点/选区/滚动改为由 `RowRegistry` 在 diff 前后自动保存与恢复；
4. 把 `pending_structural_refresh_` 的职责收回编辑器内部。

**验收**

- 500 段文档插入 1 段，新建控件数 ≤ 2，单次操作 < 50ms；
- 输入法组合中触发结构刷新，组合文本不丢失；
- 撤销后滚动位置与选区恢复。

---

### P1-02 撤销模型：整篇快照、无上限、无合并

**现状**（`editing/EditingSystem.cpp`、`editing/UndoHistory.*`）

- 每次成功命令都 `make_shared<const Document>(doc)` 两次（before/after），
  存入 `SnapshotHistoryAction`；
- `UndoHistory` 没有任何深度上限（`std::deque` 只增不减）；
- 曾经存在的 typing transaction API 已在本轮清理中删除（它本就零调用）；
- Undo/Redo 后 `MainWindow::OnUndo` 触发整篇重建。

**问题**

- 内存 = 历史条数 × 文档大小 × 2；一篇 100KB 的论文做 500 次编辑 ≈ 100MB，
  长会话会持续增长；
- 每次提交的深拷贝是 O(n)，在 P1-01 之前是第二大性能热点；
- 连续输入（如果未来改为输入即提交）无法合并成一条撤销记录。

**成熟做法**

命令模式 + 逆操作（或增量 patch）；按时间/输入连续性合并；历史栈设上限
（例如 200 条或内存预算）；撤销时恢复选区/滚动。

**建议**

1. 短期：给 `UndoHistory` 加容量上限与内存预算，超限丢弃最旧条目；
2. 中期：把“文本类”操作改为增量（段落级 before/after `InlineContent`），
   结构性操作保留整篇快照；
3. 恢复 typing transaction 语义（同 `NodeId` + 1.5s 内合并），它已经有
   `ChangeKind`/`origin` 等基础设施；
4. 在 `EditResult`/`DocumentChangedEvent` 中携带 `affected_nodes` 用于选区恢复
   （字段已存在，GUI 目前忽略）。

**验收**

- 历史占用有硬上限（测试构造 1000 次编辑，内存不增长）；
- 连续输入 10 个字符产生 1 条撤销记录；
- 撤销后光标回到变更前位置。

---

### P1-03 提交时机：失焦提交让“未提交文本”成为一等风险

**现状**

- `BlockEditor::CommitBlock` 只在失焦/Enter/Ctrl+Enter/`CommitFocused()` 时执行；
- `MainWindow` 在保存、构建、导入、换模板、关闭、破坏性导航前显式
  `CommitFocused()`（调用点分散且容易漏）；
- 自动保存抓取的是**文档**快照，因此正在输入但未提交的文本不会被保存；
- `BlockEdit`/`InlineEditor` 的 `IsDirty()` 是判断依据。

**问题**

- 崩溃/断电时，最后一段输入（可能是一整段文字）丢失；
- 新增一个会读取文档的功能时，必须记得加 `CommitFocused()`，否则 snapshot
  与用户所见不一致（项目历史上已出现过“刚选的引用没进 PDF”的缺陷）；
- `undo` 的边界由提交时机决定，语义不直观。

**成熟做法**

输入即写入内存模型（或在 300–500ms 空闲后写入），撤销按事务合并；持久化与
渲染只读模型，不存在“控件里有、模型里没有”的中间态。

**建议**

1. 引入 `DocumentDraft` 或“段落级实时写回”：`textChanged` 时把富内容写回
   `ProjectState`（不触发视图重建、不触发 build），提交时只合并撤销记录；
2. 若担心改动过大，先让 `CommitFocused()` 成为 `ProjectController` 的统一前置
   守卫（所有命令构造前自动提交），消除调用点纪律；
3. autosave 前提交聚焦行（当前只保存已提交内容）。

**验收**

- 输入 500ms 后直接杀进程，重开可恢复该文本；
- 任何读取文档的 GUI 操作都不需要手动 `CommitFocused()`。

---

### P1-04 PDF 预览在 GUI 线程同步光栅化

**现状**（`app/PdfPreview.cpp`）

- `QueryPageCount` 用 `pdfinfo`、`RenderPageToPixmap` 用 `pdftoppm`，都是
  `QProcess::waitForFinished(15–30s)`；
- 调用点是 GUI 线程：`SetDocument`、`resizeEvent`、缩放/滚动触发的
  `RenderAtCurrentZoom`、`EnsureVisiblePages`；
- 已有 140ms 去抖与 ±2 页惰性释放，但没有异步化。

**问题**

- 大 PDF、慢磁盘或 poppler 首次加载时界面冻结；
- 与项目已经实现的“异步数学渲染”形成不一致的设计语言；
- 300 DPI 下每页位图约 30MB，±2 页 + 缩放瞬间可能上百 MB。

**成熟做法**

光栅化放到专用 worker（或进程池），结果以“页号 + 缩放 generation”回传；
GUI 先显示占位/低清图；取消过期请求；总内存预算 + LRU。

**建议**

1. 仿 `MathRenderService` 建 `PdfRenderService`：请求队列、generation、
   `QPointer` 客户端守卫；
2. 页面位图按“页号 + dpi”缓存，统一内存预算（例如 256MB），超限按 LRU 释放；
3. 首页探针也异步化，先显示“正在准备预览”。

**验收**

- 打开 100 页 PDF 时事件循环单次阻塞 < 50ms；
- 连续缩放 10 次只保留最终分辨率的 3 页位图。

---

### P1-05 启动期同步 TeX 健康检查

**现状**

- `ProjectSession` 构造函数中 `RuntimeManager(install_root).Initialize()`；
- `Initialize()` 会 `VerifyExecutables` + **`VerifyCompile`**（真实编译一个最小
  文档），全部同步执行；
- GUI 在 `MainWindow` 构造时创建 `ProjectController` → `ProjectSession`。

**问题**

- 冷启动要等一次真实 TeX 编译；机器慢时窗口迟迟不出现；
- 每次 `ProjectSession` 构造都重复验证（CLI 每个命令一次）。

**成熟做法**

启动只做轻量存在性检查；完整健康检查放到后台，结果通过事件/状态展示；
把结果缓存在用户配置目录并按 runtime mtime 失效。

**建议**

1. `Initialize()` 拆成 `Probe()`（文件存在性）与 `VerifyCompile()`（后台）；
2. GUI 先显示窗口，构建前若健康检查未完成则等待或提示；
3. 缓存验证结果（runtime 目录 hash/mtime）。

**验收**

- 窗口显示时间与 TeX runtime 无关（< 200ms）；
- runtime 损坏时构建前给出 `E-RUNTIME`，而不是启动失败。

---

### P1-06 文档级派生数据每次刷新全量重算

**现状**（`app/MainWindow.cpp`、`app/ProjectController.cpp`）

- 每次 `documentChanged`：
  - `ProjectController::CitationNumbers()` **新建**一个 `CitationNumberResolver`
    （遍历全文 inline content）；
  - `BlockEditor::SetCitationNumbers` 因 `shared_ptr` 身份不同，逐行调用
    `InlineEditor::SetCitationNumbers` → `RefreshObjectDisplays()`（再遍历每行
    `QTextDocument`）；
  - `RefreshSidePanels()` 重建 Outline、引用条目、必填提示、字数统计；
- `CountWords()` 自己手写三级遍历并按空白切分。

**问题**

- 单次提交的成本 = O(行数 × 行内文本) + O(文档)；
- 长论文中每次失焦提交都可能造成可感知卡顿；
- 字数对中文按空白切分几乎恒为 1，统计失真。

**成熟做法**

按文档版本/参考文献 revision 缓存派生结果；比较内容而非指针；字数增量维护；
大纲只更新变化的节点。

**建议**

1. `CitationNumberResolver` 缓存到 `ProjectSession`（`document version` +
   `bibliography_revision` 为 key），未变化时返回同一 `shared_ptr`；
2. `BlockEditor::SetCitationNumbers` 比较 resolver 的 `numbers()` 内容（或
   revision），而不是 `shared_ptr` 身份；
3. 字数统计改为遍历 `VisitInlineContent`，按 Unicode 词边界统计（CJK 按字符
   或分词），并缓存结果；
4. Outline/References 面板采用增量更新（至少避免每次重建全部树项）。

**验收**

- 1000 段文档单次提交的派生数据计算 < 16ms；
- 中文摘要的字数统计不再恒为 1。

---

### P1-07 打开项目失败时没有任何用户反馈

**现状**（`app/MainWindow.cpp::OpenProjectDir`）

```cpp
if (!controller_->OpenProjectWithRecovery(dir, &recovered)) {
    return false;
}
```

`ProjectController::OpenProjectWithRecovery` 内部吞掉了 `std::string error`；
`OpenProjectDir` 直接返回 `false`，不弹窗、不写状态栏。最近项目列表点击一个
已损坏/已移动的项目时，用户看到的是“什么都没发生”。

**问题**

- 破坏性导航守卫已经问了用户“是否保存”，随后打开失败却无解释；
- 与 `OnNewProject`（有 `QMessageBox::critical`）不一致；
- 恢复失败（autosave 损坏）同样是静默路径。

**建议**

- `ProjectController::OpenProjectWithRecovery` 返回 `Result<void, std::string>`
  或接收 `std::string* error`；
- `MainWindow` 在失败时显示具体原因（文件缺失/JSON 损坏/目录不可写/恢复失败）；
- 最近项目条目在打开失败后给出“移除该记录”入口。

**验收**

- 打开一个被截断的 `project.paper`，必须出现包含路径与原因的对话框；
- 打开不存在的最近项目，状态栏与对话框均有提示。

---

## 4. 中优先级改进（P2）

### P2-01 公式渲染队列无界、无取消

**现状**：`MathRenderService::Request` 把每个请求 push 进 `std::deque`，worker
逐个执行 `RenderMathPreviewImage`（真实 TeX，超时 12s）；generation 只用于
`ApplyReply` 时丢弃过期回复，**不会跳过已经开始或排队的渲染**。

**问题**：快速输入/缩放时积累大量过期任务，最新公式被旧任务阻塞；每个任务可能
占用一个子进程。

**建议**：入队时按 `editor_id + formula_id` 去重（用最新 generation 覆盖排队
任务）；队列设上限；worker 在开始前检查该 formula 的 generation 是否仍是最新；
对 `latex + style` 相同的请求合并。

**验收**：连续输入 50 个字符后，`renders_completed()` 增量 ≤ 2。

---

### P2-02 输入法（IME）组合输入未显式处理

**现状**：`InlineEditor::keyPressEvent` 在 Backspace/Delete 时用
`toPlainText()` 探测 token；`Ctrl+Enter` 直接提交；没有 `QInputMethodEvent`
处理，也没有 preedit 状态跟踪。

**问题**：中文/日文输入组合期间，preedit 文本尚未进入文档；此时的 Backspace
可能被误判为对 token 的删除；`Ctrl+Enter` 可能在组合中提交半成品。

**建议**：跟踪 `inputMethodEvent` 的 preedit 区间，组合期间禁用 token 删除与
提交类快捷键；或至少在 `keyPressEvent` 中检查
`QGuiApplication::inputMethod()->isVisible()` / 活动 preedit。

**验收**：中文输入“论文”组合中按 Backspace 不删除 token；组合中 Ctrl+Enter
不提交。

---

### P2-03 缺少查找/替换与表格单元格编辑

**现状**：

- 无查找/替换（全仓无 `findText`/替换 UI）；
- Table 卡片只有网格预览 + 题注编辑器，**不能编辑单元格**
  （`MakeTableCard` 仅创建 caption 控件）；
- 图片不能通过粘贴导入（只支持文件对话框插入）。

**影响**：这两项是学术写作的高频需求；表格可插入不可编辑会被用户视为缺陷。

**建议**：

1. 查找/替换：在当前文档上实现 `QTextDocument::find` + 跨行导航，结果高亮；
2. 表格编辑：单元格进入编辑态时复用 `InlineEditor`，通过
   `SetTableCell`/`InsertTableRow` 等已有命令提交（领域层已具备 API）；
3. 图片粘贴：`canInsertFromMimeData` 识别 `image/*` → `AssetManager::Stage` →
   `InsertFigureFromSource`。

**验收**：查找能跨块跳转并高亮；表格单元格可编辑并进入 undo。

---

### P2-04 可访问性与国际化

**现状**：`grep accessibleName/tr(` 均为 0；界面字符串英文硬编码；没有
`QAccessible` 角色；块间导航依赖鼠标与 Tab 的默认行为。

**建议**：

1. 为编辑行、工具栏、Problems 条目、模板选择器设置 `accessibleName`/
   `accessibleDescription`；
2. 建立键盘导航（Tab/Shift+Tab 在块间移动、Ctrl+↑/↓ 在标题间跳转）；
3. 用户可见字符串统一走 `tr()`（至少集中到一处字符串表，便于后续翻译）；
4. 高对比主题与字号设置。

---

### P2-05 主题与样式分散

**现状**：`Theme.h` 提供颜色/字号/间距 token，但大量 QSS 字符串直接写在控件
构造函数中（header、chip、page label、focus button 等），合计数十处。

**建议**：把 QSS 生成收敛到 `theme::` 下的函数（按 objectName 选择器），控件只
设置 `objectName`；支持 palette 切换与暗色主题。

---

### P2-06 日志与可观测性

**现状**：无统一日志；调试脚手架（`PF_TRACE`）已在本轮清理中删除；唯一的
结构化流是 `BuildEvent`。`std::fprintf(stderr)` 只出现在 `main.cpp` 的致命
异常屏障。

**建议**：引入 `QLoggingCategory`（按模块：gui/domain/build/persistence），
支持 `QT_LOGGING_RULES`；把最近 N 条日志附在 Problems 面板的“导出诊断”里；
构建失败时一并导出 snapshot 摘要与 runtime 信息。

---

### P2-07 自动保存与 side-car 的事务性

**现状**：

- autosave 固定 30s，只在 Dirty 时写；不做自适应；
- `project.paper` 由 save worker 原子写；`references.bib` 在应用线程先写；
  两者不在同一事务中，崩溃可能造成条目与正文不一致；
- `template.lock.json` 是 save 完成后的派生 side-car，写入失败被忽略。

**建议**：

1. autosave 按“编辑量 + 空闲时间”自适应（例如 5s 空闲或 50 次编辑后）；
2. 把 `references.bib` 的写入并入 save 作业（同一个临时目录 + 提交顺序：
   bib → project.paper），失败时保持旧文件；
3. side-car 失败至少上报 Warning 诊断。

---

### P2-08 内存预算

**现状**：`MathRenderCache` 上限 256 项（每项位图）；`PdfPreview` 视口 ±2 页、
最高 300 DPI；两者都没有统一预算。

**建议**：引入进程级 `MemoryBudget`（例如数学 32MB、PDF 256MB），缓存按预算
淘汰；在状态栏/设置中暴露缓存占用。

---

## 5. 低优先级 / 工程化（P3）

| # | 问题 | 证据 | 建议 |
| --- | --- | --- | --- |
| P3-01 | Qt6 缺失时 4 个 GUI 测试静默消失 | `src/CMakeLists.txt` 的 `find_package(Qt6 QUIET)` + `if(Qt6_FOUND)` | CI 强制 Qt6，缺失即失败 |
| P3-02 | 默认不开启警告 | 根 `CMakeLists.txt` 只有 `PF_WARNINGS_AS_ERRORS=ON` 时才加 `-Wall -Wextra` | CI 默认开 `-Wall -Wextra`，逐步清零 |
| P3-03 | 测试在环境缺失时静默通过 | `TestRuntime.cpp` 多处 “runtime not healthy; skipping”；`TestInlinePdf.cpp` 失败路径直接 return | 用显式 `PF_SKIP` 统计并在 CI 汇总；关键断言改为失败 |
| P3-04 | 固定 `/tmp` 路径 | 24 处（GuiSmokeTest、EditorPersistenceTest、TestInlinePdf 等） | 统一 `ScopedTempDir`，允许 `ctest -j` |
| P3-05 | `install_root` 的 `__FILE__` 回退 | `ProjectSession.cpp` 用源码路径推导安装根 | 安装版必须显式传入；保留回退仅用于源码构建 |
| P3-06 | IdGenerator 进程内计数器 | `core/IdGenerator.h` | 打开项目时已做 observe/heal；跨进程协作场景可改 UUID |
| P3-07 | 临时调试转储始终开启 | `BuildCoordinator` host 的 `debug_dump_dir` 在项目打开时总是返回目录 | 改成显式调试开关，默认关闭 |
| P3-08 | 模板 adapter 是字符串匹配 | `LatexPlanner` 的 `frontmatter` 分支 | 显式注册表 + 能力声明 + 校验器 |
| P3-09 | 保存期间的重建/刷新规则分散 | `MainWindow` 的 `pending_structural_refresh_`、`editor_change_in_progress_`、`shutting_down_` | 收敛为编辑器生命周期对象 |
| P3-10 | 文档/模板/诊断字符串混用中英文 | 代码注释中文、UI 英文 | 统一 UI 英文 + `tr()`，注释保持现状 |

---

## 6. 本轮已修复的可靠性缺陷

| # | 位置 | 问题 | 修复 |
| --- | --- | --- | --- |
| F-01 | `BuildCoordinator::WorkerLoop` | `BuildOne` 未捕获异常；`LatexPlanner` 对不安全输入 `throw std::runtime_error`，异常穿透 worker 线程会 `std::terminate` | 增加异常边界：转换为 `BuildResult{Failure, InternalError}` + `E-BUILD-INTERNAL` 诊断，并发出 `BuildFailed` 事件 |
| F-02 | `TectonicCompiler` 构造 | 从已移动的 `cache_dir` 再赋值，bundle 缓存配置被静默清空 | 删除多余赋值（详见冗余清理记录 F-01） |
| F-03 | `ProjectController::SetAuthorAffiliation` | 一次操作发两次 `documentChanged`，两轮全量视图刷新 | 删除重复发射 |
| F-04 | `ProjectController::ResolveInsertionPoint` | subsubsection 内的块锚点无法解析 | 改用 `LocateNode` |

---

## 7. 建议的实施顺序

| 阶段 | 内容 | 预期收益 |
| --- | --- | --- |
| 第 1 步（1–2 周） | P1-04 PDF 异步化、P1-05 启动检查异步化、P1-06 派生数据缓存 | 立即可感知的流畅度 |
| 第 2 步（2–4 周） | P1-03 输入即写模型（或统一 CommitFocused 守卫）、P1-02 撤销上限与合并 | 数据安全 + 长时间写作可用 |
| 第 3 步（4–8 周） | P1-01 增量视图（RowRegistry + diff） | 大文档结构与撤销性能 |
| 第 4 步 | P1-07 打开失败反馈、P2-01 公式队列、P2-03 查找/表格编辑 | 产品完整性 |
| 第 5 步 | P2-02 IME、P2-04 可访问性/i18n、P2-05 主题、P2-06 日志 | 成熟度与可维护性 |
| 持续 | P3 工程化项（CI 门禁、并行测试、警告） | 防回归 |

---

## 8. 可直接写成测试的验收清单

1. **视图增量**：500 段文档插入 1 段，新建 `InlineEditor`/`QWidget` 数 ≤ 2；
2. **撤销上限**：连续 1000 次编辑后历史内存不超过配置上限；
3. **撤销选区**：撤销一段文本修改后，光标回到修改前偏移；
4. **输入可靠性**：模拟“输入后 500ms 崩溃”，重开项目能恢复该文本；
5. **PDF 异步**：打开 100 页 PDF，GUI 线程最长阻塞 < 50ms；
6. **派生数据**：1000 段文档单次提交的 `CitationNumberResolver::Build` 调用次数
   为 0（命中缓存）；
7. **中文计数**：`CountWords("这是一段中文摘要") > 1`；
8. **打开失败**：截断的 `project.paper` 弹出含路径/原因的对话框；
9. **IME**：组合输入中 Backspace 不删除 token、Ctrl+Enter 不提交；
10. **公式队列**：连续 50 次请求只渲染最新一次；
11. **表格编辑**：单元格修改进入 undo 并出现在保存后的 JSON；
12. **异常边界**：构造一个会让 `LatexPlanner` 抛异常的快照，构建返回
    `Failure/InternalError` 而不是进程退出。

---

## 9. 参考：标准编辑器的对应实现思路

| 需求 | 常见实现 |
| --- | --- |
| 增量视图 | Qt `QAbstractItemModel` + delegate；或按 NodeId 的 widget 回收池 |
| 撤销合并 | 事务键 + 时间窗；或 OT/CRDT 的 operation log（本场景不需要） |
| 输入即保存 | 模型层实时写 + 事务合并；文档快照按 revision 定时落盘 |
| 异步光栅化 | 工作线程 + generation + LRU 位图缓存 |
| 派生数据 | 观察者 + revision key 缓存；增量索引 |
| IME | `QInputMethodEvent` 的 preedit 跟踪；组合期间屏蔽自定义快捷键 |
| 可访问性 | `QAccessibleInterface`/accessibleName + 键盘导航 + 高对比主题 |
| 日志 | `QLoggingCategory` + 结构化事件（本项目的 `BuildEvent` 已是雏形） |
