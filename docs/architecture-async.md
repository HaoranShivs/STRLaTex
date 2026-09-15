# M1：Single-owner Application State + Immutable Async Pipeline

这份文档固定了 PaperForge 的线程模型。它不是"设计愿景"，而是当前代码里实际执行的
约束——每一条都有对应的回归测试（`tests/TestSessionAsync.cpp`）。

## 一句话规则

> **`ProjectSession` / `ProjectState` / `EditingSystem` / `Document` 只允许
> Application（UI）线程访问。后台线程只能看到不可变值对象，并且只能通过
> `ApplicationEvent` 把结果交回应用线程。**

## 数据流

```
                         Application / UI thread
                         ────────────────────────────────────────────────
  EditCommand ──▶ ProjectSession ──▶ ProjectState (mutable, single owner)
                     │  │  │           Document / EditingSystem / Undo
                     │  │  │
        capture      │  │  │            PreviewState / PersistenceState
        immutable    │  │  │
        snapshot     │  │  │
                     ▼  ▼  ▼
              BuildSnapshot   SaveSnapshot / SerializedProject
                     │              │
   ──────────────────┼──────────────┼──────────────────────────────────
                     ▼              ▼            background workers
              BuildCoordinator   SaveCoordinator
              (worker thread)    (worker thread)
                · Validator        · ProjectPersistence::Save
                · LatexRenderer    · 原子写（temp → 校验 → rename）
                · Tectonic
                     │              │
                     └──▶ BuildResult / SaveCompletion ──▶ PostApplicationEvent
                     │              │
   ──────────────────┼──────────────┼──────────────────────────────────
                     ▼              ▼
        ProcessApplicationEvents()   ← 只有应用线程调用
          · BuildPhaseChangedEvent
          · BuildResultReadyEvent ──▶ AcceptBuildResult() ──▶ PreviewGate
          · SaveCompletedEvent    ──▶ ApplySaveCompletion()
          · AutosaveTickEvent     ──▶ Autosave()（在本线程抓快照）
                     │
                     ▼
        build_result_handler_ / preview_update_handler_ / save_result_handler_
                     │
                     ▼
        ProjectController（Qt adapter）──▶ previewUpdated / saveFinished 信号
```

## 为什么这样切

| 问题 | 旧实现 | 现在 |
| --- | --- | --- |
| Build 完成回调读 live state | worker 线程进入 `OnBuildFinished`，与 UI 的 `BumpRevision()` 竞争 | worker 只 `PostBuildResult()`；`AcceptBuildResult()` 在应用线程做 stale 判定 |
| Autosave 线程遍历 Document | timer 线程直接 `CaptureProjectSnapshot()` | timer 线程只发 `AutosaveTickEvent`；快照在应用线程抓取 |
| 陈旧结果判断 | `result.revision != state_.revision()`（在工作线程读） | `PreviewGate` 纯函数，在应用线程对 project id + revision + snapshot id + build id 四元组判定 |
| Save 阻塞 UI 且无法表达"保存中又编辑" | 同步写在 UI 线程 | 不可变 Snapshot → Save worker；完成事件带回 `revision`，只有等于当前 revision 才进入 `Clean` |
| `buildFinished(bool, pdf_path)` 丢失身份 | PDF 路径裸传，无法回答"这是哪个 project/build/revision" | `PreviewUpdate{ project_id, build_id, revision, pdf }` |

## 事件协议

`src/project/ApplicationEvent.h`：

```cpp
struct BuildPhaseChangedEvent { BuildPhase previous, current; };
struct BuildResultReadyEvent  { BuildResult result; };
struct SaveCompletedEvent     { SaveCompletion completion; };
struct AutosaveTickEvent      {};

using ApplicationEvent = std::variant<...>;
```

* `PostApplicationEvent()` 线程安全，只做入队 + 唤醒，绝不触碰 `ProjectState`。
* `ProcessApplicationEvents()` 只能在应用线程调用，负责 `std::visit` 分发。
* 事件队列是唯一的异步入口：后台线程没有任何其它回调可以进入 domain。
* `SetWakeHandler()` 由宿主安装（Qt 用 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`）；
  CLI / 测试直接用 `WaitForApplicationEvent()` 轮询，语义相同。

## PreviewGate

`src/project/PreviewGate.h` 是一个纯函数，把"陈旧结果"变成可单测的规则：

```cpp
PreviewGateInput current {
    has_project, project_id, revision, snapshot_id, build_id
};
EvaluatePreviewGate(current, result) ->
    NoProject | ForeignProject | StaleRevision | StaleSnapshot | StaleBuild | Accept
```

判定顺序是有意的：**先 project，再 revision，再 snapshot/build**。切换项目时
revision 会从 0 重新开始，只比 revision 会把 A 项目的 PDF 放进 B 项目的预览。

## Save 语义

* `Save()` / `Autosave()` 抓取不可变快照后立即返回 `SaveResult::Status::Queued`。
* `FlushSaves()` 阻塞到队列写完并应用完成事件（CLI / 测试 / 关闭项目时使用）。
* 完成时只有 `completion.revision == state_.revision()` 才把状态从 `Saving` 置为
  `Clean`。**保存期间继续编辑 → 当前状态必须保持 Dirty**。
* Autosave 永不改变 Clean/Dirty（architecture 32）。
* 关闭 / 切换项目前先 `Flush()`，用户最后一次 `Ctrl+S` 不会丢。
* Worker 按 FIFO 串行写；比已写入 revision 更旧的 user save 会被拒绝（rule 5）。

## 单线程所有权哨兵

`ProjectSession` 在构造时记录 `owner_thread_`，并在
`ProcessApplicationEvents` / `AcceptBuildResult` / `RequestBuild` / `Execute` /
`Save` / `Autosave` 入口调用 `NoteOwnerThreadUse()`。任何后台线程误入都会让
`owner_thread_violations()` 计数增加；回归测试断言它始终为 0，并在结果回调里断言
`IsOwnerThread()`。

## 回归测试（可执行架构约束）

| 场景 | 测试 |
| --- | --- |
| 编译中继续输入 → 旧 build 不得更新预览 | `ScenarioEditDuringBuildDropsStalePreview` |
| Undo 后旧 build 返回 → 丢弃 | `ScenarioUndoDuringBuildDiscardsResult` |
| 切换模板时旧 build 返回 → 不得进入预览 | `ScenarioTemplateSwitchWhileBuildingDropsOldPdf` |
| A 项目 build 未结束就切到 B → 必须按 project id 丢弃 | `ScenarioProjectSwitchDiscardsOtherProjectBuild` |
| 保存期间继续编辑 → 当前状态保持 Dirty | `ScenarioEditDuringSaveStaysDirty` |
| 自动保存与编辑并发 → 永远保存某一个完整 snapshot | `ScenarioAutosaveWritesCompleteSnapshotWhileEditing` |
| 删除被引用的 Figure → 文档仍有效 + dangling 诊断 | `ScenarioDeleteReferencedFigureEmitsDanglingDiagnostic` |
| gate 规则本身（含 ForeignProject 优先级） | `PreviewGateAcceptsOnlyTheCurrentBuild` |
| PreviewUpdate 携带完整 artifact 身份 | `PreviewUpdateCarriesArtifactIdentity` |

## 用 ThreadSanitizer 验证

上面这些测试同时也是数据竞争的检测器。在 TSan 下跑一遍（跳过需要 tectonic 的
端到端用例）应当零警告：

```bash
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1 -DPF_SKIP_TECTONIC_TESTS"
cmake --build build-tsan --target pf_tests
TSAN_OPTIONS=halt_on_error=0 ./build-tsan/tests/pf_tests
```

第一次跑 TSan 时它抓出了 `Validator.cpp` 里一个从 build worker 递增的全局诊断
计数器（`g_counter`）——现在已经是 `std::atomic`。这类"后台线程共享全局状态"正是
本里程碑要消灭的东西，所以 TSan 是这一层的常备验证手段。
