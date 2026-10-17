# PaperForge (STRLaTex) 代码审查总报告（2026-10）

> 审查对象：`/home/orangepi3/Projects/STRLaTex`，`master`，HEAD `e49430b`
> 加上本次审查的工作树改动。
> 审查范围：`src/`（C++20 + Qt6）、`tests/`、根目录与 `src/` 的 CMake、
> `docs/` 与 `README.md` 的一致性。
> 交付物：本报告 + [运行时调用链路](runtime-call-chain.md) +
> [冗余清理记录](redundancy-cleanup.md) + [编辑器设计改进建议](editor-design-review.md)。

---

## 0. 结论

**一句话：领域层与异步骨架的质量高于一般编辑器原型；本轮主要差距在编辑器
交互/性能层，以及若干“重复实现导致漂移”的死代码。**

本轮完成了三件事：

1. **梳理并记录运行时调用链**：GUI/CLI 入口 → 打开 → 编辑 → 撤销 → 构建 →
   预览 → 保存/恢复 → 模板/文献/资源，关键类、接口、数据结构全部落到
   [`runtime-call-chain.md`](runtime-call-chain.md)。
2. **清理冗余代码**：净删除 **1215 行**（+232 / −1447），删除 9 个文件、
   新增 1 个共享头文件，GUI 组件编译次数 **93 → 16**；未改变任何仍被使用的
   行为，全部测试通过。逐条记录（含“与谁冗余”）见
   [`redundancy-cleanup.md`](redundancy-cleanup.md)。
3. **从标准编辑器设计角度给出改进项**：按 P1/P2/P3 分级，每条含现状证据、
   标准做法、建议与验收方式，见
   [`editor-design-review.md`](editor-design-review.md)。

**缺陷结论**：本轮发现并修复了一个**进程终止级**缺陷——构建 worker 内的
异常没有边界，`LatexPlanner` 抛出的 `std::runtime_error` 会穿透
`std::thread` 导致 `std::terminate`（见 §3.4 F-01）。修复后不再存在已知的
未处理崩溃路径。历史报告（`docs/STRTex_四维审查复核报告.md`，针对更早 commit）
中的主要 P0 在当前代码中已能看到对应修复：关闭/新建/打开统一守卫、项目路径
信任边界、JSON 尺寸与深度限制、异步数学渲染、结构化保存结果。当前最需要投入
的是设计改进文档中的 P1 项（交互/性能/输入可靠性），而不是“救火”。

---

## 1. 审查范围与方法

### 1.1 环境

| 组件 | 版本/状态 |
| --- | --- |
| CMake | 3.16+（本机已配置好 `build/`） |
| 编译器 | GCC 12（C++20） |
| 构建 | Ninja |
| Qt | Qt6 Widgets + Test 可用（GUI 与 4 个 GUI 测试目标参与构建） |
| TeX | `runtime/texlive` 健康；另有 `tools/bin/tectonic` 与 bundle 缓存 |
| 测试 | CTest 6 个目标（GUI 4 + 核心 2） |

### 1.2 方法

1. **调用链阅读**：按运行时顺序阅读入口、适配器、聚合根、编辑协议、渲染、
   构建、持久化与 GUI 控件，函数级记录调用关系与数据所有权。
2. **静态冗余取证**：
   - 全仓 `grep` 每个候选符号（含 `&Class::method`、`std::function` 赋值）；
   - 脚本化检测：标准化后的函数体完全相同、8 行滑窗克隆、匿名命名空间中
     未使用的自由函数；
   - 逐个确认“只写不读的成员 / 只有声明没有定义 / 只有定义没有调用”。
3. **动态验证**：每一批修改后 `cmake --build`；最终跑完整 `ctest`（含真实
   pdfLaTeX 用例）。
4. **文档一致性**：修正 README 与 `docs/structure-semantics.md` 中因删除而
   失真的描述，并新增本次三份专题文档。

### 1.3 工作树说明

审查开始时工作树已有两处**用户数据**改动，本轮**未触碰**：

```text
 M projects/GMVG3/project.paper
?? projects/GMVG3/template.lock.json
```

这两项不属于源码审查范围。

---

## 2. 基线

| 项目 | 数值 |
| --- | --- |
| C++ 源码（`src/` + `tests/`，`.cpp/.h/.hpp`） | 38,260 行 |
| `src/` 文件数 | 95 |
| GUI 目标 | 1 个应用 + 1 个 smoke test + 1 个截图工具 + 4 个 widget/回归测试 |
| 基线测试 | 6/6 通过，real 时间 177.7s |
| 基线构建 | 已是最新（`ninja: no work to do`） |

---

## 3. 主要发现

### 3.1 架构与数据流：良好，且有可执行的约束

值得保留并作为后续改动的边界：

- **领域层真正 Qt-free**：`src/app/` 之外没有 Qt include；CLI 是第二个
  `ProjectSession` 消费者。
- **单一所有者 + 不可变快照**：`ProjectState`/`Document`/`EditingSystem`
  仅应用线程访问；worker 只拿 `BuildSnapshot`/`SerializedProject`。
- **typed 事件邮箱**：`ApplicationEvent` + `ProcessApplicationEvents()` 是
  异步结果进入领域的唯一接缝。
- **PreviewGate 纯函数**：按 project → revision → snapshot → build 四元组
  拒绝陈旧结果。
- **原子保存 + 回读校验**：`project.paper` 写入后先反序列化校验再 rename。
- **路径信任边界**：项目文件中的相对路径统一经
  `ResolveUntrustedProjectPath`。
- **结构化诊断 + SourceMap**：GUI 不解析编译器日志。

### 3.2 冗余与死代码：已清理 1215 行

规模最大、价值最高的几项：

| 项 | 说明 |
| --- | --- |
| `DocumentIndex` | 每次编辑全量重建、生产零读取；与 `LocateNode` 信息重复 |
| `AnchorResolver` | 生产从未调用；重复实现遍历 |
| `EditorItemKind` | 122 行，四个函数零调用，与 `BlockEditor` 的 kind 表重复 |
| typing transaction | 零调用、分支不可达且语义有缺陷 |
| 枚举 `ToString` × 5 | 仅“声明+定义”两处引用 |
| `PayloadName` 76 行 visitor | 零调用 |
| 6 个 GUI 目标重复源清单 | 同源编译 93 次 → OBJECT 库 16 次 |
| 数学渲染双层缓存 | 两个缓存 + 两套相同 key，保留 service 的 LRU |

完整清单（含每项的行数、冗余对象、验证方式）见
[`redundancy-cleanup.md`](redundancy-cleanup.md)。

### 3.3 编辑器设计差距：集中在交互/性能层

按影响排序（详见 [`editor-design-review.md`](editor-design-review.md)）：

1. **全量视图重建**（`RebuildFromDocument`）导致大文档卡顿、选区/滚动/IME
   状态丢失，只能靠“输入时推迟重建”的规则兜底；
2. **撤销是整篇快照且无上限**，每次提交两次全文深拷贝；
3. **失焦才提交**，未提交文本不在模型里，崩溃会丢失；
4. **PDF 预览同步阻塞 GUI**（`pdfinfo`/`pdftoppm` + `waitForFinished`）；
5. **启动时同步做真实 TeX 健康检查**（`RuntimeManager::Initialize` 会编译
   一个小文档）；
6. **派生数据每次全量重算**（引用编号、字数、大纲、引用条目）；
7. **打开项目失败无用户反馈**（`OpenProjectDir` 静默返回 false）；
8. 公式渲染队列无界/无取消、IME 未显式处理、无查找替换、表格不可编辑、
   可访问性与 `tr()` 缺失。

### 3.4 本轮修复的具体缺陷

| # | 位置 | 问题 | 修复 |
| --- | --- | --- | --- |
| F-01 | `BuildCoordinator::WorkerLoop` | `BuildOne` 未捕获异常；`LatexPlanner` 会 `throw`，异常穿透 worker 线程会 `std::terminate` | 增加异常边界，转成 `Failure/InternalError` + `E-BUILD-INTERNAL` 诊断 |
| F-02 | `TectonicCompiler` 构造 | 从已移动的 `cache_dir` 再次赋值，配置的 bundle 缓存被静默清空 | 删除多余赋值 |
| F-03 | `ProjectController::SetAuthorAffiliation` | 一次操作发两次 `documentChanged`，触发两轮全量刷新 | 删除重复发射 |
| F-04 | `ProjectController::ResolveInsertionPoint` | subsubsection 内的块锚点解析失败 | 改用 `LocateNode` |
| F-05 | `UndoHistory::Push` 事务分支 | 不可达且不清空 redo | 随事务机制删除 |

### 3.5 测试与构建

优点：

- 6 个 CTest 目标覆盖领域协议、持久化、构建、真实 TeX、GUI 输入保护、
  块投影与富文本 widget；
- 真实 pdfLaTeX 用例（字体、四种标记组合）确实执行；
- OBJECT 库改造后构建时间显著下降且未出现 moc 重复符号。

问题（已记录，未在本轮修改）：

- Qt6 缺失时 4 个 GUI 测试**静默消失**（`find_package(Qt6 QUIET)`）；
- 多处环境缺失时 `return` 而非失败（`TestRuntime`、`TestInlinePdf` 等），
  绿色结果不能完全代表真实链路；
- 24 处固定 `/tmp` 路径使 `ctest -j` 可能相互污染；
- 默认未开启 `-Wall -Wextra`。

### 3.6 文档漂移（已修正）

| 文档 | 旧描述 | 现描述 |
| --- | --- | --- |
| `README.md` | 模块列表含 `DocumentIndex`/`AnchorResolver`；“异步插入用 Stable Anchor” | 已按当前代码删除/改写 |
| `README.md` | “遍历逻辑只写一份”列 DocumentIndex | 已去掉 |
| `docs/structure-semantics.md` | `EditorItemKind`、`DocumentIndex` 及其测试 | 已改为 `BlockEditor` 内部 kind 表并删去对应测试行 |
| `docs/STRTex_四维审查复核报告.md` | 历史报告 | **保持原样**（它记录的是旧 commit 的结论，不应改写） |

---

## 4. 交付物

| 文件 | 内容 |
| --- | --- |
| `docs/runtime-call-chain.md` | 运行时调用链、线程模型、关键类/接口/数据、契约与不变式 |
| `docs/redundancy-cleanup.md` | 逐项冗余清理记录（删除/合并/保留候选/验证） |
| `docs/editor-design-review.md` | 标准编辑器视角的设计审查与改进路线 |
| `docs/feature-gaps.md` | 面向科研论文/技术文档的功能缺口与交互改进清单（引用选择器、数学面板、图表、交叉引用、中文/矢量图等） |
| `docs/feature-implementation-design.md` | 上述缺口的接口级实现方案（WP 拆分、头文件草案、命令载荷、schema v4、迁移与测试） |
| `docs/code-review-2026-10.md` | 本报告 |

代码侧改动集中在 `src/` 与 `tests/`（74 个文件状态变化，含 9 个删除）；
`projects/GMVG3/` 的用户数据未修改。

---

## 5. 验证记录

### 5.1 基线

```text
cmake --build build          → ninja: no work to do
cd build && ctest --output-on-failure
   6/6 passed, Total Test time (real) = 177.70 sec
```

### 5.2 清理后

```text
cmake -S . -B build -G Ninja      → 配置成功
cmake --build build -j$(nproc)    → EXIT 0
cd build && ctest --output-on-failure
   6/6 passed
```

最终结果（本次工作树）：

| 测试 | 结果 | 时间 |
| --- | --- | --- |
| `gui_smoke` | Passed | 1.78 s |
| `gui_input_persistence` | Passed | 21.10 s |
| `inline_editor` | Passed | 102.17 s |
| `block_projection` | Passed | 0.59 s |
| `pf_tests` | Passed | 43.09 s |
| `pf_template_tests` | Passed | 9.09 s |
| **合计** | **6/6** | **177.81 s** |

> 上述数字来自最终代码的 `build/_review/ctest-final.log`（`build/` 为
> 本地忽略目录，不随仓库提交）。

### 5.3 清理专项验证

- 每个删除符号在 `src/` + `tests/` 全仓零调用；
- 测试使用的 `ToString(MathState)`、`ToString(RuntimeStatus)` 等保留；
- 删除 `EditorItemKind` 后 6 个 GUI 目标全部通过；
- OBJECT 库改造后无重复 moc 符号、无 `undefined reference`；
- 真实 TeX 用例（`TestRuntime`、`TestInlinePdf`、`pf_template_tests`）执行通过。

---

## 6. 残余风险与建议

| 风险 | 级别 | 建议 |
| --- | --- | --- |
| `TectonicCompiler` 不可达、仓库仍携带 ~98MB tectonic 缓存 | P1（产品决策） | 要么让配置真正选择 tectonic 后端，要么整体下线并清理工具链 |
| 输入未提交时崩溃会丢失最后一段文本 | P1 | 见设计文档 P1-03 |
| PDF 预览/启动健康检查阻塞 GUI | P1 | 见 P1-04、P1-05 |
| 全量重建 + 无上限撤销在长文档下退化 | P1 | 见 P1-01、P1-02 |
| 打开项目失败静默 | P1 | 见 P1-07 |
| 环境缺失时测试静默通过、固定 `/tmp` 路径 | P1（工程） | CI 强制环境、显式 SKIP、`ScopedTempDir` |
| 表格不可编辑、无查找替换、IME/可访问性缺失 | P2 | 见设计文档 P2 |
| `TEMP-DEBUG` 转储始终开启 | P2 | 改成显式调试选项 |
| `references.bib` 与 `project.paper` 非同一事务 | P2 | 见 P2-07 |

---

## 7. 复现命令

```bash
# 配置 + 构建
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)

# 全部测试（会执行真实 pdfLaTeX 用例，约 3 分钟）
cd build && ctest --output-on-failure

# 只跑核心测试（约 43s）
./tests/pf_tests

# 只跑 GUI 输入保护
QT_QPA_PLATFORM=offscreen ./src/app/paperforge-gui-persistence-test

# 冗余取证（本轮使用的临时脚本位置，build/ 被忽略，不随仓库提交）
ls build/_review/
```

---

## 8. 与历史报告的关系

`docs/STRTex_四维审查复核报告.md` 针对更早的 commit（`a816d01`），其中：

- 已过期：无 Qt6/无 runtime 环境导致的“真实链路无法证明”；
- 已在当前代码中修复：未保存变更守卫、路径逃逸、JSON 数值/深度崩溃、
  保存部分成功、行内公式同步渲染、Subsection 内 Figure/Table 不渲染；
- 仍然成立并被本轮再次确认：`DocumentIndex` 生产零消费者（本轮已删除）、
  CMake 源清单重复（本轮已用 OBJECT 库消除）、测试静默通过（记录于设计文档
  P3-03）。

本报告与专题文档描述的是**当前工作树**，不修改历史报告。
