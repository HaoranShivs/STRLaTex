# 冗余代码清理记录（2026-10）

> 对应代码：本仓库 `master` 本次审查后的工作树。
> 相关文档：[运行时调用链路](runtime-call-chain.md)、
> [编辑器设计改进建议](editor-design-review.md)、[审查总报告](code-review-2026-10.md)。

---

## 0. 结论摘要

| 指标 | 清理前 | 清理后 | 变化 |
| --- | --- | --- | --- |
| `src/` + `tests/` 行数（含 CMake） | — | — | **净 −1215 行**（新增 232 / 删除 1447） |
| 其中 C++ 源码（`.cpp/.h/.hpp`） | 38,260 | 37,150 | −1,110（含新增 `QtConvert.h` 22 行） |
| C++ 文件数 | — | — | 删除 9 个、新增 1 个（`app/QtConvert.h`） |
| GUI 组件源文件编译次数（6 个目标合计） | 93 | 16 | **−77 次** |
| `src/app/CMakeLists.txt` | 204 行 | 104 行 | −100 行 |
| 测试函数 | — | — | −3（随被删组件移除） |
| 构建 | 通过 | 通过 | 无新增警告/错误 |
| 测试 | 6/6 通过 | 6/6 通过 | 见 §5 |

清理只移除了**没有任何调用者、只写不读、或者与另一处实现完全重复**的代码；
没有改变任何仍被使用的行为。唯一的“行为变化”是三处**缺陷修复**（§4），它们
的方向都是“更符合原注释所述的意图”，并且有测试覆盖。

---

## 1. 清理原则与安全网

1. **先证明无调用者**：每个候选都在 `src/` + `tests/` 全仓 grep，并检查
   `&Class::method`、函数指针、`std::function` 赋值等非直接调用形式。
2. **区分“死代码”和“测试接缝”**：测试使用的接口一律保留；只在组件整体
   没有生产消费者且测试只测它自身时才连同测试一起删除。
3. **每一步可验证**：按批次修改后立即 `cmake --build`，最后跑完整
   `ctest`（6 个目标）与真实 TeX 构建用例。
4. **删除记录与冗余对象**：下表的“与谁冗余/为什么多余”列就是删除前的取证。

---

## 2. 删除项明细

### 2.1 GUI 层（`src/app/`）

| # | 删除对象 | 位置（删除前） | 行数 | 与谁冗余 / 为什么多余 | 验证 |
| --- | --- | --- | --- | --- | --- |
| G-01 | `EditorItemKind.h/.cpp`（`EditorItemLabel`、`EditorItemKindName`、`EditorItemKindFromName`、`EditorItemKindOfNode`） | `src/app/EditorItemKind.*` | 122 | 四个自由函数零调用；`BlockEditor` 自己维护了 `kind` 字符串表（`DisplayLabelFor`/`RoleForKind`），`EditorItemKind` 是重复的第二套“行类型”枚举 | 全仓 grep 仅命中自身；删除后 6 个 GUI 目标全部重编通过 |
| G-02 | `InlineEditor::PlainText()` | `InlineEditor.h/.cpp` | 3 | `QTextEdit::toPlainText()` 的纯转发；全部调用点直接用基类方法 | grep 零调用 |
| G-03 | `InlineEditor::SetReferenceItems()` + `struct ReferenceItem` + `reference_items_` | `InlineEditor.h/.cpp` | 14 | 选择器条目统一走 `BlockEditor::SetReferenceItems(PopupList::Item)`；该成员只被赋值、从未读取 | grep 零调用；成员只写 |
| G-04 | `InlineEditor::math_generation_` | `InlineEditor.h/.cpp` | 2 | 只自增、从不读取；真正的失效机制是 `MathRenderService` 的 editor/formula generation 与 `formula_id` | 成员只写 |
| G-05 | `MainWindow::last_build_revision_` | `MainWindow.h/.cpp` | 2 | 与 `last_built_revision_`（`ProjectRevision`）表达同一事实；该 `int` 只写不读，过期标记完全由 `last_built_revision_` 决定 | 成员只写；grep 两个成员 |
| G-06 | `MainWindow::focus_mode_` | `MainWindow.h/.cpp` | 2 | 只赋 `= on`，从未读取；专注模式状态由 `focus_button_->isChecked()` 与分隔条可见性承载 | 成员只写 |
| G-07 | `MainWindow::preview_container_` | `MainWindow.h/.cpp` | 3 | `preview_` 的同义别名（`preview_container_ = preview_` 后仅用于 `addWidget`） | 直接替换为 `preview_` |
| G-08 | `PF_TRACE` 调试脚手架（11 处 `if (!qEnvironmentVariable("PF_TRACE")...) fprintf`） | `MainWindow.cpp` | 22 | 一次性启动追踪代码；`PF_TRACE` 全仓仅此处引用，正常构建无输出 | 删除后 `PF_TRACE` 零引用 |
| G-09 | `PdfPreview::SetMessage()`、`PdfPreview::FitPage()` | `PdfPreview.h/.cpp` | 7 | `SetMessage` 只是 `ShowMessage` 的转发；`FitPage` 无调用者（工具栏只提供 Fit Width），其 `whole_page` 分支随之删除 | grep 零调用 |
| G-10 | `PdfPreview::RebuildPageWidgets()`、`UpdatePageSizes()`、`UpdateViewportExtent()` | `PdfPreview.h` | 3 | 只有声明、**从未有定义**的重构残留 | 链接期本就无引用 |
| G-11 | `PdfPreview::zoomChanged` 信号 + `emit` | `PdfPreview.h/.cpp` | 3 | 全仓零连接；删除必须成对（只删 emit 会留下死信号） | grep 零 `connect` |
| G-12 | `ProjectController::InsertFigure()` | `ProjectController.h/.cpp` | 6 | 与 `InsertFigureAfter()` + `ResolveInsertionPoint()` 重复；GUI 只走后者 | grep 零调用 |
| G-13 | `ProjectController::build_in_flight_` | `ProjectController.h` | 1 | 零引用；构建状态由 `BuildEvent`/`buildStatusChanged` 驱动 | 成员只声明 |
| G-14 | `MathRenderService::UnregisterClient()`、`MathRenderService::Generation()` | `MathRenderService.h/.cpp` | 13 | 零调用；客户端生命周期由 `QPointer` 守卫（销毁后回复被丢弃） | grep 零调用 |
| G-15 | `MathRenderCache::ClearForTest()` | `MathRenderService.h/.cpp` | 4 | 零调用（测试直接构造缓存并检查逐条淘汰） | grep 零调用 |
| G-16 | `BlockEditor` 内的死语句：`container_depth`（计算后 `(void)`）、`OpenSlashMenu` 的 `current_group` 空操作、重复的 `more->setVisible(false)`、冗余 `header->setVisible(true)`、只写不读的 `commit_role` property | `BlockEditor.cpp` | 10 | 重构后遗留；`OpenSlashMenu` 的第二次 `blocks_` 扫描与第一处 `row_node` 完全相同 | 构建 + `block_projection` 测试 |
| G-17 | `PopupList::is_active()`、空 `if` 分支、注释掉的 `setFocus` | `PopupList.h/.cpp` | 8 | 零调用；`is_active()` 与 `isVisible()` 重复 | grep 零调用 |
| G-18 | `Theme::kWarningSoft` | `Theme.h` | 1 | 全仓唯一出现处就是定义 | grep |
| G-19 | `ProblemsPanel::events_` | `ProblemsPanel.h` | 4 | 当前只写不读，但它是 Build Log 的文档化模型；**保留**（见 §3 说明，不计入删除） | — |

### 2.2 领域层（core/document/editing/project/build/render/persistence/...）

| # | 删除对象 | 位置（删除前） | 行数 | 与谁冗余 / 为什么多余 | 验证 |
| --- | --- | --- | --- | --- | --- |
| D-01 | `DocumentIndex` 整类 + `EditingSystem::index_` / `ProjectSession::index()` + 每次编辑/撤销/重做/关闭的 `Rebuild` + 2 个测试 | `document/DocumentIndex.*`、`EditingSystem.*`、`ProjectSession.*` | ~120 | 与 `DocumentTraversal::LocateNode` 提供同一份坐标信息；生产代码**从不 `Find`**，只在每次编辑后做一次全量遍历 + `unordered_map` 构造，纯成本零收益 | grep 全部 `index()`/`Find` 读取仅测试；删除后 `pf_tests` 全绿 |
| D-02 | `AnchorResolver` 整类 + `EditingSystem::anchor_resolver_` + `StableNodeAnchor`/`AnchorBias` + 1 个测试 | `editing/AnchorResolver.*`、`EditCommand.h` | ~130 | 生产代码从未调用 `Resolve`（插入点由 `ProjectController::ResolveInsertionPoint` 基于 `LocateNode` 解析）；该类还重复实现了 `LocateNode` 的遍历 | 同上 |
| D-03 | `UndoHistory` typing-transaction 机制：`BeginTransaction/EndTransaction/InTransaction/CurrentKey/Depth`、`EditingSystem::Begin/EndTypingTransaction`、`Push` 的不可达分支 | `editing/UndoHistory.*`、`EditingSystem.*` | ~45 | 事务 API 零调用（`Push` 的 transaction 分支永不执行），而且该分支遗漏 `redo_.clear()`；实际语义就是“每次操作一条记录” | grep 零调用；`UndoHistoryCanRedo` 等测试仍通过 |
| D-04 | 枚举 `ToString`：`EditOrigin`、`SaveKind`、`SaveOutcome`、`PreviewGateDecision`、`ApplicationEvent` | `EditingSystem.cpp`、`SaveCoordinator.*`、`PreviewGate.*`、`ApplicationEvent.*` | ~75 | 每个函数只有“声明 + 定义”两处引用，无任何日志/诊断消费者 | grep；测试使用的 `ToString(MathState)`/`ToString(RuntimeStatus)` **保留** |
| D-05 | `EditCommand::PayloadName()`（76 行 visitor） | `editing/EditCommand.h`、`EditingSystem.cpp` | 76 | 零调用；命令名可由既有 `BuildEvent`/诊断路径获得 | grep 零调用 |
| D-06 | `DocumentEditor::SetFigureAltText()`、`BlockParents()`、`Throw()`、`SectionPath`、`InsertBlockSpec` | `document/DocumentEditor.*` | ~55 | 全部零调用；`BlockParents` 的“按容器拒绝跨容器移动”逻辑已由 `MoveBlock` 的显式检查覆盖，`Throw` 被 `Result` 取代 | grep；`pf_tests` |
| D-07 | `AssetRegistry::Unregister()` | `asset/AssetManager.*` | 5 | 零调用；资源只在导入时注册，从不移除 | grep |
| D-08 | `CitationNumberResolver::IsKnown()` | `numbering/CitationNumberResolver.*` | 4 | 零调用；`Find()` 内部已经用 `known_keys_` 完成同一判断 | grep |
| D-09 | `CompilerFactory::ToolchainFor()` | `build/CompilerFactory.*` | 8 | 零调用；toolchain 由 `ProjectSession::RequestBuild` 直接赋值，工厂只负责 `Create` | grep |
| D-10 | `ProjectMigrator::CurrentVersion()` | `persistence/ProjectMigrator.*` | 4 | 零调用；版本常量 `kSchemaVersion` 已直接可用 | grep |
| D-11 | `CompilerConfig::keep_logs` | `build/Compiler.h` | 1 | 只被赋 `true`、从未读取；日志保留行为由编译器实现固定 | grep |
| D-12 | `ImportedAssetCandidate::staged_path` | `asset/AssetManager.*` | 2 | 只赋值、从未读取；路径由 `metadata.relative_path` 表达 | grep |
| D-13 | `BuildPackage::bibliography_files` / `template_files` | `render/BuildPackage.h`、`LatexPlanner.cpp` | 4 | 只 `push_back`、从未读取；实际打包文件在 `BuildPackage::files` | grep |
| D-14 | `ProjectSession::HasBibliography()`、`SetDocumentChangedHandler()` + `document_changed_handler_` + 构造接线 | `project/ProjectSession.*` | 12 | 零调用；GUI 的刷新由 `ProjectController` 在命令返回后同步触发 | grep |
| D-15 | `PdfArtifact::clear()`、`JsonValue::is_null()`、`VisitSections()`、`BlocksFromJson()` | `project/PreviewUpdate.h`、`core/Json.h`、`document/DocumentTraversal.*`、`persistence/ProjectPersistence.cpp` | ~25 | 均为零调用；`BlocksFromJson` 是 `BlocksFromJsonDetailed` 的无诊断旧版 | grep |
| D-16 | 空 TU：`core/IdGenerator.cpp`、`project/ProjectState.cpp`、`project/ApplicationEvent.cpp` | 三个 `.cpp` | 37 | 只为“让目标非空”而存在；`IdGenerator`/`ProjectState` 是 header-only，`ApplicationEvent.cpp` 只含已删除的 `ToString` | 删除文件 + CMake 条目后构建通过 |
| D-17 | `RuntimeProblem` 结构、`RuntimeInfo::runtime_version/texlive_version`、`RuntimeManager::TexLiveRoot/status/info` | `build/RuntimeManager.*` | ~20 | 全部零读取；`Initialize()` 的返回值 `RuntimeInfo` 只使用 `texlive_root`/`status`/`problems` | grep |
| D-18 | `ValidationProfile`、`ValidationInput::has_bibliography`、`ValidationInput::asset_paths` | `validation/Validator.h`、`BuildCoordinator.cpp`、测试 | ~15 | `ValidationProfile` 零使用；`has_bibliography`/`asset_paths` 被填充但 `Validator` 从不读取（引用键列表 `bibliography_keys` 才是判据） | grep + 修改两处测试输入 |
| D-19 | CLI `CmdOpen` 声明、`PF_TECTONIC_PATH` extern | `cli/Commands.h`、`cli/Main.cpp` | 5 | `CmdOpen` 无定义无调用；`PF_TECTONIC_PATH` 无定义无引用 | 构建 |
| D-20 | 死分支/无效语句：`InlineText` 空 `if`、`ProjectSession::ImportAsset` 空 `if`、`BuildCoordinator` debug dump 重复空判断、`LatexPlanner` 被覆盖的 audit 占位、`MathValidator` 未用 `command_start` | 各自文件 | ~15 | 条件恒真/恒假或结果被下一句覆盖 | 构建 + 全测试 |

### 2.3 测试随组件删除

| # | 删除内容 | 位置 | 原因 |
| --- | --- | --- | --- |
| T-01 | `DocumentIndexRebuild` | `tests/TestDocument.cpp` | 被测类已删除（D-01） |
| T-02 | `DocumentIndexCoversThirdLevel` | `tests/TestStructure.cpp` | 同上 |
| T-03 | `AnchorResolverAfterBlock` | `tests/TestEditing.cpp` | 被测类已删除（D-02） |

> 其余测试（含 `pf_tests` 的内核协议、真实 TeX 构建、GUI 输入保护、块投影、
> 富文本 widget）全部保留并通过。

---

## 3. 合并项明细（重复实现 → 单一实现）

| # | 重复点 | 重复位置（清理前） | 合并后 | 减少行数 |
| --- | --- | --- | --- | --- |
| M-01 | `ToQ`/`ToStd` 各有 5/2 份逐字节相同的实现 | `MainWindow.cpp`、`BlockEditor.cpp`、`OutlinePanel.cpp`、`ProblemsPanel.cpp`、`ProjectController.cpp` | 新增 `app/QtConvert.h`（inline，唯一实现） | ~18 |
| M-02 | “执行命令 + 成功即发 documentChanged + 返回结果”重复 15 次 | `ProjectController.cpp` 各编辑方法 | 全部改为 `return ExecuteAndNotify(std::move(p))`（该方法本就存在） | ~60 |
| M-03 | `SetAuthorAffiliation` 在 `ExecuteAndNotify` 之后又发一次 `documentChanged` | `ProjectController.cpp` | 删除多余一次发射（这原本会导致一次操作触发两轮全量视图刷新） | 2 |
| M-04 | `Undo()`/`Redo()` 函数体 34 行逐行相同 | `EditingSystem.cpp` | `ApplyHistoryEntry(entry, origin)` 单一实现 | ~30 |
| M-05 | 三处项目路径推导（`project_dir` / `project_file` / `assets` / `build` / `autosave`） | `ProjectSession::NewProject/OpenProject/OpenProjectWithRecovery` | `SetProjectPaths(project_dir)` | ~12 |
| M-06 | `Execute`/`Undo`/`Redo` 都重复 “Applied → MarkDirty + RequestBuild” | `ProjectSession.cpp` | `FinalizeEdit(result)` | ~8 |
| M-07 | 富文本行提交逻辑在 `CommitInlineRow` 与 `CommitBlock` 各一份 | `BlockEditor.cpp` | `CommitInlineRow` 找到对应行后调用 `CommitBlock` | ~12 |
| M-08 | `InsertSectionAfter` 手写三级嵌套遍历找所属 section | `ProjectController.cpp` | `LocateNode` + `address->section`（与 `DeleteNode` 等一致） | ~22 |
| M-09 | `ResolveInsertionPoint` 手写三级遍历（且漏掉 subsubsection 内的块锚点） | `ProjectController.cpp` | `LocateNode` + 明确的父容器判定（顺带修复上述漏解析） | ~20 |
| M-10 | `Undo`/`Redo` GUI 包装体重复 | `ProjectController.cpp` | `ApplyHistory(bool redo)` | ~10 |
| M-11 | 最近项目列表更新逻辑重复两处 | `MainWindow.cpp` | `RememberRecentProject(dir)` | ~7 |
| M-12 | 两套数学渲染缓存：`MathPreviewRenderer` 的全局 `QHash`（满 256 项**整体清空**）与 `MathRenderService` 的 LRU 缓存，key 构造函数逐字节相同 | `MathPreviewRenderer.cpp`、`MathRenderService.cpp` | 只保留 service 的 LRU 缓存；渲染函数变为纯渲染 | ~30 |
| M-13 | 6 个 GUI 目标重复枚举同一批 16 个源文件（同源编译 93 次） | `src/app/CMakeLists.txt` | `pf_gui_objects` OBJECT 库 + 每个可执行目标只保留自己的 main/test TU（AUTOMOC 只在库上开启，避免重复 metaobject） | ~100（CMake） |
| M-14 | `tests/CMakeLists.txt` 中重复的 `enable_testing()`（根目录已经调用） | `tests/CMakeLists.txt` | 保留根目录一处，改为注释说明 | — |

---

## 4. 顺带修复的缺陷（不是删除，但由清理暴露）

| # | 位置 | 问题 | 修复 |
| --- | --- | --- | --- |
| F-01 | `TectonicCompiler::TectonicCompiler`（`build/Compiler.cpp`） | 先 `(void)cache_dir_`，随后 `cache_dir_ = std::move(cache_dir)` 从**已移动**的参数再赋值，导致配置的 bundle 缓存目录被静默清空 | 删除这两行，成员初始化列表中的值直接生效 |
| F-02 | `ProjectController::SetAuthorAffiliation` | 一次操作发射两次 `documentChanged`，触发两轮 `RebuildFromDocument` | 见 M-03 |
| F-03 | `ProjectController::ResolveInsertionPoint` | 锚点位于 subsubsection 的块之后时解析失败（“insertion anchor not found”） | 改用 `LocateNode`，覆盖三级容器 |
| F-04 | `UndoHistory::Push`（事务分支） | 事务内 push 不清空 redo，形成不可达但错误的 stale-redo 分支 | 随事务机制一起删除；`Push` 现在只有“入栈 + 清空 redo” |

> F-01 是真实缺陷；F-02/F-03 是“重复实现导致两份逻辑漂移”的典型后果；F-04
> 属于未启用代码的缺陷，随死代码一并移除。

---

## 5. 验证记录

### 5.1 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)     # EXIT 0
```

- 6 个 GUI 目标 + `pf_tests` + `pf_template_tests` + CLI 全部链接成功；
- OBJECT 库改造后没有出现 `multiple definition`（AUTOMOC 只在
  `pf_gui_objects` 上开启，消费目标关闭 AUTOMOC）；
- GUI 组件的编译单元从 93 降到 16；OBJECT 库改造后的重建只产生 41 个
  ninja 步骤（改造前同等工作量需要重复编译全部 GUI 源）。

### 5.2 测试

```bash
cd build && ctest --output-on-failure
```

结果见 [审查总报告 §5](code-review-2026-10.md#5-验证记录)：6/6 通过，包含：

- `inline_editor`（widget + 真实 pdfLaTeX 断言）；
- `gui_input_persistence`（86 项输入保护/布局检查）；
- `gui_smoke`、`block_projection`；
- `pf_tests`（Document/Editing/Renderer/Persistence/Build/真实 TeX/场景回归）；
- `pf_template_tests`。

### 5.3 人工检查

- `git diff` 逐文件复核，确认删除项没有改变仍被使用的语义；
- `grep` 复核每个删除符号在 `src/` + `tests/` 中零残留（测试用到的
  `ToString(MathState)`/`ToString(RuntimeStatus)` 已按需保留）；
- README 与 `docs/structure-semantics.md` 中指向已删组件的描述已同步更新。

---

## 6. 已同步的文档

| 文档 | 修改 |
| --- | --- |
| `README.md` | 模块列表去掉 `DocumentIndex`/`AnchorResolver`；“遍历逻辑只写一份”不再提 DocumentIndex；删除“异步插入用 Stable Anchor”旧声明，改为按当前文档解析插入锚点；测试覆盖描述去掉 DocumentIndex；新增本组文档的链接 |
| `docs/structure-semantics.md` | 删除 `EditorItemKind` 引用（改为 `BlockEditor` 内部 kind 表）；删除 `DocumentIndex` 引用与对应测试行 |
| `docs/runtime-call-chain.md` | 新增，按当前实现记录全部调用链 |
| `docs/editor-design-review.md` | 新增，记录设计改进建议 |

---

## 7. 保留未删的候选（及原因）

这些项**确实**有冗余或死代码特征，但删除会改变产品能力、测试结构或触及高风险
路径，因此只记录、不在本轮删除：

| # | 候选 | 规模 | 为什么暂不删除 | 建议 |
| --- | --- | --- | --- | --- |
| K-01 | `TectonicCompiler` 不可达：`ProjectSession` 默认构造它，但生产构建始终走 `compiler_provider`（TexLive） | ~95 行 + 配置字段 | 它是文档化的 tectonic 后端，仓库仍随包 98MB 的 `tools/tectonic-cache`；下线需要产品决策并同步工具链文档 | 要么让 `Config::tectonic_path` 真正选择后端，要么整体下线 |
| K-02 | `TEMP-DEBUG` 把生成的 LaTeX/PDF 转储到 `.paperforge/build` | ~54 行 | 被 `TestTemplates` 覆盖，且是用户排障能力；删除需改测试 | 改名为正式的 debug 选项并文档化 |
| K-03 | 三套原子写（ProjectSession / ProjectPersistence / ProjectPdf） | ~40 行重复 | 语义不同：持久化需要“写后回读校验”，PDF 是文件复制；合并需设计带校验回调的公共 helper | P2，抽出 `core/AtomicFile` |
| K-04 | `ProjectPersistence` 的三段 block 校验循环（section/subsection/subsubsection 各一份） | ~65 行 | 位于反序列化安全边界，且三处校验覆盖并不完全一致；改成 `VisitBlocks` 是正确方向但需补测试 | P2，先补“subsection 内 alt_text/latex 校验”测试再合并 |
| K-05 | `DocumentEditor` 三个 `MoveSection/MoveSubsection/MoveSubsubsection` 与 5 处 table 查找 lambda | ~45 行 | 模板化会改变错误返回顺序，风险/收益一般 | P2 |
| K-06 | 测试辅助重复：`BodyOf`×8、`EnsureQApplication`×6、`Spin`×5、`Settle`×2、`order()`×2 | ~100 行 | 属于测试基础设施；抽取会同时改 10+ 个测试文件，收益仅是行数 | P2，抽到 `tests/TestHelpers.hpp` |
| K-07 | 固定 `/tmp` 临时路径（24 处） | — | 不是冗余而是并行测试污染风险 | P1，统一用 `ScopedTempDir` |
| K-08 | `ProblemsPanel::events_` 只写不读 | 4 行 | 是 Build Log 的文档化模型，删除属设计变更 | 保留，后续接成真正的日志模型 |
| K-09 | 多个 `ToString(enum)` switch（NodeKind/EditError/LatexEngine 等） | ~60 行 | 仍在生产/测试使用（如 `Validator`、`TestRuntime`） | 保留 |

---

## 8. 统计口径说明

- “行数”来自 `git diff --numstat -- src tests`（不含 `docs/` 新增文档与
  README/既有文档修改）；C++ 源码的绝对行数按 `src/` + `tests/` 下所有
  `.cpp/.h/.hpp` 统计（38,260 → 37,150）；
- 删除的 9 个文件是：`EditorItemKind.h/.cpp`、`DocumentIndex.h/.cpp`、
  `AnchorResolver.h/.cpp`、`core/IdGenerator.cpp`、`project/ProjectState.cpp`、
  `project/ApplicationEvent.cpp`；新增 `src/app/QtConvert.h`；
- 目录 `src/app/EditorItemKind.*` 删除后，CMake 的 6 处引用同步移除；
- GUI 编译次数按“每个目标枚举的 app 源文件数”求和（改造前
  17+8+17+17+17+17 = 93；改造后 16 个组件只编译一次）。
