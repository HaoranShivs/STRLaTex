# STRTex（STRLaTex）四维审查复核报告

> 审查对象：`/home/orangepi3/Projects/STRLaTex`
> 分支：`master`　HEAD：`a816d012a98b341d36ff5a5a2048ab269fee8db3`（`feat(ui): refine editor typography and figures`）
> 工作树：干净（`git status --short` 为空）
> 审查维度：代码规范与工程质量、架构与数据流、功能设计与交互、构建测试与稳定性
> 参考基线：《STRTex 全面代码与功能设计审查报告》（下称"旧报告"）
> 方法：**本轮具备完整工具链，因此以动态验证为主、静态取证为辅**，并对旧报告逐条做时效性复核

---

## 0. 审查结论

**一句话结论：架构骨架是真实的、可运行的，测试数量也是真实的；但这仍不是一个可以稳定交付的版本。旧报告指出的四个 P0 全部仍然成立，而本轮又发现了若干同等严重、旧报告未覆盖的问题。**

本轮相对旧报告最重要的三个变化：

1. **旧报告的动态结论已经过期，且是被"环境"误导的。** 旧报告在无 CMake/Qt6、无 `runtime/texlive` 的环境下得出"140 测试 1 失败、真实 TeX 链路无法证明"。本轮 `runtime/texlive`（301 MB）已就位，完整工具链（CMake 4.3.3 / Qt6 / Ninja / GCC 12.3）可用，**实测 `pf_tests` 140/140 通过、运行时健康、4 个真实 TeX 用例确实执行**（详见 §2）。旧报告关于 `RuntimeIsHealthy` 失败的归因是环境缺失导致的，不是代码缺陷。
2. **但"测试可假绿"这一结构性隐患依然存在**，而且比旧报告描述更广：`TestInlinePdf.cpp` 有 6 处"失败即 `return` 仍报 OK"，`TestRuntime.cpp` 4 处环境缺失即通过，`EndToEndTectonicBuild` 构建失败无任何断言，`paperforge-gui-smoke`（39 个断言）与 `paperforge-ui-verify` **从未注册进 CTest**。**干净 clone 上 `ctest` 依然全绿**——这意味着旧报告所担忧的"绿灯不能证明真实链路可用"今天仍然成立，只是触发条件被掩盖了。
3. **旧报告的 P0-05 病因判断是错的，但结论方向对。** 旧报告称 `as_string()/as_array()/as_int()` 类型不匹配会触发 variant 访问异常——**经复核不成立**，这些访问器全是安全的全函数。真正的崩溃面是 **`Json.cpp` 未保护的 `std::stod` 异常穿透** 与 **解析递归无深度上限**，本轮已用最小输入独立复现出异常穿透与 SIGSEGV（详见 §8.1、§8.2）。

**发布阻断问题清单（P0）**：

| 编号 | 问题 | 状态 |
|---|---|---|
| P0-1 | 未保存变更零保护（无 `closeEvent`，新建/打开项目不检查 Dirty），且**未保存过的新项目 autosave 恢复通道不可达** | 旧报告 P0-01 成立，本轮补强 |
| P0-2 | 行内公式渲染同步阻塞 GUI 线程（最坏 ~28 s），无异步生命周期模型 | 旧报告 P0-02 成立 |
| P0-3 | 项目内相对路径无信任边界，可路径逃逸读写项目外文件 | 旧报告 P0-03 成立 |
| P0-4 | 保存链路"部分成功"与 I/O 错误被吞，UI 可谎报已保存 | 旧报告 P0-04 成立 |
| P0-5 | **反序列化对损坏输入可致进程崩溃**：`1e999` 触发未捕获异常穿透至 GUI；深嵌套 `[[[[…` 触发栈溢出 SIGSEGV | **旧报告病因错、结论对；本轮独立复现** |
| P0-6 | **Subsection/Subsubsection 内的 Figure/Table 在编辑器中完全不渲染**（内容存在、编辑器假装不存在） | **旧报告完全漏报** |

**最值得肯定的部分**（不应在修复中被破坏）：领域层真正 Qt-free；`PreviewGate` 是干净纯函数；worker 输入是 `shared_ptr<const Document>` 不可变快照；类型化跨线程事件邮箱；保存 latest-wins 语义正确；"先序列化→临时文件→回读反序列化校验→rename"的落盘设计；强类型 ID 体系；SourceMap/DiagnosticMapper 让 GUI 不解析日志文本；以及 **CLI 作为第二个 `ProjectSession` 驱动**，实证了分层收益。

---

## 1. 审查基线与方法

### 1.1 基线与规模

| 项目 | 数值 |
|---|---|
| 跟踪文件总数 | 864 |
| C++ 源码（src） | 23,196 行 / 95 文件 |
| C++ 测试（tests + src/app 测试） | 约 7,400 行 / 23 文件 |
| 受跟踪仓库体积 | 约 134.9 MB（其中二进制/缓存 98.4%） |
| 模块（CMake target） | 13 个静态库 + CLI + 5 个 GUI 可执行 |

### 1.2 方法

1. **动态验证（本轮核心增量）**：全新 out-of-tree 构建 `build-audit/`，跑通全部 target 与 CTest。
2. **独立复现**：对高危结论编写最小复现程序（JSON 崩溃），不依赖推断。
3. **四维专项并行取证**：代码规范 / 架构 / 功能交互 / 构建测试四个方向分别做全仓量化（grep 计数、`git ls-files`、`awk`、逐行复核）。
4. **旧报告逐条时效性复核**：明确区分"仍成立 / 已过期 / 旧报告错误"。

### 1.3 审查环境

| 组件 | 版本 |
|---|---|
| CMake | 4.3.3 |
| GCC | 12.3.0（Ubuntu 22.04） |
| Ninja | 可用 |
| Qt6 | Widgets + Test 均可用 |
| Clang / ASan 运行环境 | **不可用**（`clang++` 缺失）——因此旧报告的 `double free` 崩溃**本轮无法关闭** |
| TeX Live | `runtime/texlive` 301 MB，6 个必需可执行文件齐全 |

> **诚实声明**：旧报告记录的用户崩溃 `double free or corruption (out)`（第二次双击编辑行内公式）需要一个内存安全工具链才能闭环。本环境无 clang、无 ASan 配置，**该问题本轮既未复现也未排除，仍应视为未关闭的发布阻断项**。

---

## 2. 动态验证结果

### 2.1 构建

```
cmake -S . -B build-audit -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   → 配置成功，Qt6 found
cmake --build build-audit -j4                                          → [BUILD EXIT: 0]  155/155
```

全部 155 个构建步骤成功，**包括 5 个 Qt6 GUI target**。无编译错误，也无任何警告输出——因为工程里根本没有开启任何警告开关（见 §7.1）。

### 2.2 测试矩阵（干净串行运行）

| 目标 | 结果 | 说明 |
|---|---|---|
| `pf_tests`（核心） | **140 tests, 0 failures** | 运行时 Healthy；4 个真实 TeX 用例确实执行 |
| `paperforge-inline-editor-test` | **51 tests, 0 failures** | Qt widget + 真实 pdfLaTeX |
| `paperforge-gui-persistence-test` | **86 checks 全过** | 未提交输入持久化回归 |
| `ctest` 汇总 | **3/3 passed（60.9 s）** | — |
| `paperforge-gui-smoke` | 构建成功但 **未被 CTest 注册** | 39 个断言从不执行 |
| `paperforge-ui-verify` | 构建成功但 **未被 CTest 注册** | 截图工具 |

关键运行时证据：

```
runtime: Healthy
[ OK  ] RuntimeIsHealthy (181 ms)
[ OK  ] FontTestBuildsWithoutSubstitution (343 ms)     ← 真实字体编译
[ OK  ] IeeeMarksReachPdfLatex (484 ms)                ← 真实 IEEE 模板构建
[ OK  ] CompileStreamsOutputLiveToSink (323 ms)        ← 真实流式输出
NO FAILURES
```

**这直接推翻旧报告 §3 的动态结论**：`runtime/texlive` 存在且健康，真实 TeX 编译、字体、流式输出链路**已被动态证明可用**。

### 2.3 一个必须披露的插曲：并发运行会互相污染

本轮的**第一次** `pf_tests` 运行得到 `140 tests, 1 failures`，失败项正是 `RuntimeIsHealthy`，报错为：

```
runtime: Corrupted | runtime health-check compile failed |
  I can't find file `main.aux'. | Emergency stop.
```

排查后发现：当时有另一个构建/测试进程在并行运行，而**测试套件大量使用固定共享路径**，两者互相破坏。串行重跑即 140/140 全绿。

这不是偶然，而是一个真实缺陷，见 §7.4：`RuntimeManager` 把健康检查的编译产物写进 **runtime 安装目录内**的 `<runtime>/texlive/texmf-var/health`（`src/build/RuntimeManager.cpp:122`），且**只在成功路径清理**（`:148`）；`ProjectSession` 构造函数每次都会同步执行这次编译（`src/project/ProjectSession.cpp:84-85`）。同时全仓测试使用约 40 处硬编码 `/tmp` 固定名（`/tmp/pf-font-test`、`/tmp/pf-ieee-marks`、`/tmp/pf-e2e-*` 等），并非 RAII 随机目录。

**结论：该测试套件不可并行，且在只读 runtime 安装上会误报 `Corrupted`。** 这既是稳定性缺陷，也解释了旧报告为何会得到环境性失败。

### 2.4 无警告开关的真实代价

工程未开启任何 `-Wall/-Wextra`。补加 `-Wall -Wextra -Wpedantic -Wshadow` 重新编译全部 130 个 TU，得到 **94 条警告、0 错误**，其中包含真实缺陷：

- `src/editing/EditingSystem.cpp:124` — `tpl` 绑定后从未使用
- `src/cli/Commands.cpp:44` — `rev` set but not used

---

## 3. 与参考报告的差异复核

### 3.1 已过期（旧报告结论在当前 commit 不再成立）

| 旧报告条目 | 旧结论 | 当前事实 |
|---|---|---|
| §3 动态验证 | "审查环境没有 CMake 和 Qt6""`runtime/texlive` 不存在，`RuntimeIsHealthy` 失败（139/140）" | **已过期。** 完整工具链可用；`runtime/texlive`（301 MB）存在，6 个必需可执行文件齐全；实测 **140/140**，运行时 Healthy，真实 TeX 测试全部执行 |
| P0-02 隐含判断 | "缺少超时" | **不准确。** 超时确实存在（`kTexTimeoutMs=12000`、`kRasterTimeoutMs=8000`，`MathPreviewRenderer.cpp:47-48`）；真正问题是**同步阻塞 GUI 线程**，且最坏约 28 s |
| P1-05 第 3/4 点 | "runtime 作为普通源码散落提交" | **不准确。** `runtime/` 从未被跟踪，且 `.gitignore` 已明确忽略并说明可由 `tools/build-runtime/` 重建。真正未清理的是 22 个 CMake 生成物与 `tools/` 下 673 个缓存文件 |

### 3.2 旧报告错误（方向对、病因错）

| 旧报告 | 复核结论 |
|---|---|
| **P0-05**：`as_string()/as_array()/as_int()` 仅判断字段存在就调用，**类型不匹配可能触发 variant 访问异常** | **病因不成立。** `src/core/Json.h:40-60` 六个访问器全部是全函数：类型不符返回默认值或函数内 `static const` 空容器，`find()` 也先判 `is_object()`，**不存在抛异常路径**。风险真实但性质完全不同：**静默降级**（`revision` 若为字符串会静默变 0）+ **`std::stod` 异常穿透** + **递归无深度上限**。后两者本轮已复现为可崩溃缺陷（§8.1、§8.2） |

### 3.3 旧报告不准确或夸大

| 旧报告 | 复核结论 |
|---|---|
| "Welcome 页最近项目入口没有先提交当前行，也没有检查 Dirty 状态"列为 P0 同级风险 | **夸大。** `ShowWorkspace(false)` 只在主窗口构造函数中调用（`MainWindow.cpp:89`），进入工作区后**没有任何路径能回到 Welcome 页**，因此该入口在当前版本几乎不可触达。它是真实代码事实，但不构成 P0 同级风险（反过来暴露了"无法关闭项目/返回欢迎页"的产品缺口） |
| 以 "`@` 引用列表缺少 Subsection/Subsubsection/…" 为框架讨论引用完整性 | **机制已不存在。** `@` 引用菜单已被删除（`BlockEditor.cpp:432-434` 有明确注释）。真实缺陷应表述为：**工具栏 Reference/Citation 选择器的候选集不完整**（`MainWindow.cpp:541-564`） |
| README "同时写 106 个、100 个测试" | 陈旧程度被低估：README 还写 inline-editor-test "15 个"，实测 **51** 个；核心实测 **140** 个。且 README 仍在宣传已删除的 `@` 菜单 |

### 3.4 旧报告仍然成立的部分（本轮独立复核确认）

以下条目行号精确命中，**在当前 commit 全部成立**，旧报告在此判断准确：

- **P0-01** 未保存变更保护缺失（`MainWindow.cpp:776-788`、`:598-626`、`:636-657`；`ProjectSession.cpp:371-388`）。本轮补强：全仓 `closeEvent` **零命中**。
- **P0-03** 路径信任边界缺失（`ProjectPersistence.cpp:466-468`、`:576-577`；`ProjectSession.cpp:351`；`SnapshotFactory.cpp:24-28`；`Compiler.cpp:45,55-60`）。
- **P0-04** I/O 错误吞掉（`ProjectSession.cpp:290-299`、`:301-316`、`:572`、`:723`；`ProjectPersistence.cpp:660`）。
- **P1-01** GUI 手写二级遍历（`ProjectController.cpp:144-176`、`MainWindow.cpp:541-564,568-594`）。
- **P1-02** Figure `\label` 被 alt 门控（`LatexRenderer.cpp:193-202`）。
- **P1-03** `"Import Table…"` 名不符实（`BlockEditor.cpp:134,257`；`ProjectController.cpp:631-637`）。
- **P1-06** 工程门禁缺失、**P1-07** 超大 GUI 文件与 CMake 源清单重复、**P1-08** 品牌与文档漂移、**P1-09** 各确定性边界。

---

## 4. 维度一：代码规范与工程质量

### 4.1 结论

工程规范**地基不差，但没有任何自动门禁**。命名体系基本统一（类型 PascalCase、私有成员 `snake_case_`、常量 `kPascalCase`、`#pragma once` 57/57、TODO/FIXME/HACK 全仓 0 处、raw `new` 全部 Qt parent 化无泄漏）。问题集中在：**零门禁、品牌四套命名、格式大面积不统一、错误吞掉、重复逻辑、文档漂移与仓库卫生**。

### 4.2 发现

#### S-1 [P1] 工程门禁文件全部缺失

`.clang-format`、`.clang-tidy`、`.editorconfig`、`.gitattributes`、`LICENSE`、`CONTRIBUTING.md`、`CHANGELOG.md`、`.pre-commit-config.yaml` **全部不存在**；无 `.github/`、无任何 CI。同时 0 个源文件带 `Copyright`/`SPDX`，却捆绑了 37 MB 的 `tools/bin/tectonic` 与 301 MB TeX Live。

**影响**：格式/命名/警告全靠口头约定；"clean checkout 能否构建、测试是否真跑"无人验证——这正是旧报告环境性误判得以长期存在的土壤；公开仓库缺 LICENSE 与第三方许可清单是合规缺口。

#### S-2 [P1] CMake 无 warning policy（实测 94 条警告）

全仓 CMake 中 `Wall|Wextra|Werror` **零命中**。补加后在 130 个 TU 上产生 **94 条警告**（`-Wshadow` 90、`-Wmissing-field-initializers` 2、未使用变量 2），其中 2 条是真实缺陷。

#### S-3 [P1] 品牌四套命名并存

用户可见处是 `PaperForge`（`MainWindow.cpp:87` 窗口标题、`main.cpp:7-8` 应用名、`WelcomePage.cpp:17`）；构建名是 `paperforge`；错误文案用 `STRLaTex`（`Compiler.cpp:509`、`BuildCoordinator.cpp:298,302`）；注释/文档用 `STRTeX/STRTex`（`MathValidator.cpp:143,175`、`docs/REPOSITORY.md:9`）；用户目录是第四套 `.paperforge`。计量：`PaperForge|paperforge` 40 处、`STRTeX|STRTex` 6 处、`STRLaTex` 5 处。

#### S-4 [P1] 错误处理双轨 + I/O 失败被吞

`Result<T,E>`（`src/core/Result.h`）与 `bool + std::string* error` 并存（后者 9 处）；`WriteFileAtomically` 返回 `bool` 但**未标 `[[nodiscard]]`**，调用点两处全部丢弃；`catch(...)` 5 处（含 `Compiler.cpp:148` 空 catch、`MathPreviewRenderer.cpp:1503` 统一降级为字面回退）；16 个 `E-*` 错误码以散落字符串字面量存在；GUI 自行拼接核心错误串 4 处。

#### S-5 [P1] Windows 分支丢弃取消、超时与实时日志

```cpp
// src/build/Compiler.cpp:210-217
#ifdef _WIN32
  (void)cancel_requested; (void)timeout; (void)combined;
  (void)stderr_text;      (void)on_output;
  return std::system(command.c_str());
#else
  // fork + setpgid + 双管道 + deadline + 进程组 kill
```

Windows 下编译**无法取消、无超时、无流式日志**，直接破坏 README 宣称的 Build 状态机与 `docs/toolchain.md` 的流式契约，而 README 把跨平台列为目标。

#### S-6 [P2] 格式大面积不统一（量化）

| 指标 | 数值 |
|---|---|
| 一级缩进 2 空格 vs 4 空格 | 13 : 79（123 文件）；**同文件内混用已确认 ≥3 处**（`BlockEditor.cpp`、`ProjectPersistence.cpp`、`ProjectSession.h`） |
| 单语句 `if/for/while` 无大括号 | **406 处** |
| 指针绑定 `Type* name` vs `Type *name` | 415 : 491 |
| 引用绑定 `Type& name` vs `Type &name` | 576 : 611 |
| >100 字符行 / 最长 | 34 行 / 131 字符（`MathPreviewRenderer.cpp:1600`） |
| 标准库头排在项目头之后 | 37 处 / 13 文件 |
| 命名空间收尾注释 双空格 vs 单空格 | ~90 : 22 |

#### S-7 [P2] 死代码、调试残留与死抽象

- `TEMP-DEBUG` 4 处 / 3 文件，且在生产路径**无条件开启**（`ProjectSession.cpp:91-96`）。
- 生产路径无条件构造**永不使用**的 `TectonicCompiler`（`ProjectSession.cpp:66-72`），实际走 `CompilerFactory → TexLiveCompiler`（`:120-129`）。
- `BuildResult::Outcome::Superseded` 从未被赋值；`BuildCoordinator::Host::project_id` 已接线但从未被调用。
- `ProjectController::stateChanged` 是**死信号**（见 §6.3）。

#### S-8 [P2] 重复逻辑与测试框架重复

`ToQ(const std::string&)` 在 **5 个 TU 各定义一份**；`src/app/EditorPersistenceTest.cpp` 另起一套 `Check()`+`failures` 而非复用 `tests/TestMain.hpp`；CMake 源清单在同一批 GUI 源上重复 4–5 次（见 §7.1）。

#### S-9 [P2] 超大文件与超长函数

| 文件 | 行数 | 最长函数 |
|---|---|---|
| `src/app/BlockEditor.cpp` | 2,345 | `RebuildFromDocument` **420 行** |
| `src/app/MathPreviewRenderer.cpp` | 1,806 | ~1700 行匿名命名空间，6 类职责 |
| `src/app/ProjectController.cpp` | 1,014 | 57 个 out-of-line 定义 |
| `src/app/MainWindow.cpp` | 1,002 | 30 个方法横跨 6 类职责 |

前 5 大文件占全 `src` 的 **29.8%**、占 GUI 的 **58.3%**。`RebuildFromDocument` 同时承担重建、焦点恢复、拖拽重建、布局项删除，也是公式崩溃场景的高风险区。

#### S-10 [P2] 命名与类型约定的小缺口

public 聚合字段不带 `_` 而私有成员带——该区别未成文；33 个 `enum class` 中 6 个未写 `: std::uint8_t`；裸 `size_t` 271 处 vs `std::size_t` 2 处；`tests/TestMain.hpp` 是唯一既无 `#pragma once` 也无 include guard 的头文件。

### 4.3 文档漂移

| README 声称 | 实际 |
|---|---|
| `README.md:54` "106 个单元" | 核心 **140** |
| `README.md:219` "# 100 个测试" | 核心 **140** |
| `README.md:243` inline-editor-test "15 个" | **51** |
| `README.md:81` Subsubsection "全链路可用" vs `README.md:277` "V1 范围外：Subsubsection" | **自相矛盾**，且第三级已实现 |
| `README.md:159` 输入 `@` 弹出分组菜单 | 该菜单**已删除** |
| `README.md:192` `PAPERFORGE_TECTONIC_CACHE` | **代码中不存在**（幽灵标识符） |
| `README.md:76` 报 `E-RUNTIME` | **代码中不存在**，实际是 `CompileFailureKind::RuntimeMissing` |
| `README.md:191-193` 以 Tectonic 为生产路径 | 生产链是 Portable TeX Live + pdfLaTeX |
| README 宣称"真实构建测试" | 未说明 runtime 缺失时部分测试会静默跳过 |

**建议**：README 改用"能力 + 代码位置 + 测试名"的可验证形式，不手写测试数量（由 CI 产出），并统一产品名（建议定名 `STRLaTex`，`.paperforge` 目录改名需配 schema 迁移，可暂缓但须写入 ADR）。

---

## 5. 维度二：架构与数据流

### 5.1 分层与依赖（真实图景）

CMake 把核心拆成 13 个静态库，方向正确，且**领域层确实 Qt-free**（对 13 个非 GUI 模块全量扫描 `<Q...>`/`"Q...` **零命中**；域内出现的 "Qt" 全部是断言独立的注释）。

但"分层由 CMake 强制"**不成立**：

- `pf_core` 的 `PUBLIC` include 目录是 `src/`（`src/CMakeLists.txt:8`），于是**每个 target 都能 include `src/` 下任意头文件**，跨层包含只能靠链接期兜底。
- 存在一条真实反向边 `template → build`（`src/template/TemplateRegistry.h:9` include `build/Toolchain.h`），与 `build → render → template` 构成模块图唯一环。`src/build/Toolchain.h:1-11` 的注释自认是为了规避环。
- 链接图与真实包含图不一致：`pf_render` 声明 `pf_validation` 但 `src/render` 零引用；`pf_persistence` 声明 `pf_bibliography` 但零引用；`pf_build` 实际使用 `validation/Validator.h` 却未声明。

### 5.2 核心数据流

```
GUI 输入 → BlockEditor/InlineEditor 提交完整 InlineContent
  → ProjectController::MakeCmd(FullEditPayload)（带 project_id / base_revision）
  → ProjectSession::Execute → EditingSystem::Apply（校验 project_id、base_revision）
      ├─ before = make_shared<const Document>(doc)   ← 整份深拷贝
      ├─ DocumentEditor 就地修改 + BumpVersion()
      ├─ after  = make_shared<const Document>(doc)   ← 整份深拷贝
      ├─ history_.Push(before, after)
      └─ index_.Rebuild(doc)                         ← 全量重建
  → MarkDirty() + RequestBuild(false)

worker 线程：BuildCoordinator（debounce 800ms，latest-wins，generation 校验）
  → Validator → LatexRenderer → StagePackage → Compiler（流式 EmitEvent）
  → PostApplicationEvent（锁 + condvar + wake_handler 唤醒 GUI 线程）
GUI 线程：ProcessApplicationEvents → AcceptBuildResult → EvaluatePreviewGate
  （has_project → project_id → revision → snapshot_id → build_id）
```

**设计优点**：worker 输入是 `shared_ptr<const Document>` 不可变快照；事件是 `std::variant` 类型化协议；`PreviewGate` 先比 `project_id` 再比 `revision`（防止切换项目后 revision 从 0 重来导致串台）；保存 latest-wins 只在 `completion.revision == state_.revision()` 时置 Clean，并有对应测试（`TestSessionAsync.cpp` 的 `ScenarioEditDuringSaveStaysDirty`）；保存采用"序列化→临时文件→回读反序列化校验→rename"，能拦住"序列化产出不可解析文件"。

### 5.3 发现

#### A-1 [P0] 保存指示器可谎报"已保存"

`ProjectController.cpp:97-99` 把 `SaveResult` 压成 `bool`，**丢弃 `saved_revision`**；而 `ProjectSession.cpp:617-623` 只在 revision 相等时才置 `Clean`。于是"保存 rev20 → 编辑到 rev21 → rev20 写完"这一异步管线的基本场景下，**UI 显示 ✓ Saved 而 session 仍是 Dirty**。旧报告只讲到"保存链路部分成功"，未发现 UI 与 session 的状态背离。

#### A-2 [P1] `DocumentChangedEvent` 协议生产零消费者 → 只能全量重建

`ProjectSession::SetDocumentChangedHandler` 全仓**零调用者**；`EditingSystem::Notify()` 每次编辑都构造 `affected_nodes` / `change_kinds`，**无人接收**。GUI 侧发出的是**无载荷**的 `documentChanged()`，直接连到 `RefreshDocumentView` → `RebuildFromDocument`（420 行，销毁并重建全部行 widget）。

**这正是旧报告 F-03"增量式 GUI 更新"缺失的前置条件**：载荷已经被构造出来了，只是没接线。任何编辑都触发整树重建，焦点/选区/滚动靠字符串 key 恢复。

#### A-3 [P1] 单所有者线程规则被 worker 侧 getter 突破，哨兵有盲区

规则明示（`ProjectSession.h:20-22`）：`ProjectState/ProjectSession/Document` 只属于应用线程。但 `ProjectSession.cpp:91-96` 把 `debug_dump_dir` 实现为读 `lifecycle_state_` 与 `paths_` 的 lambda，而 `BuildCoordinator.cpp:233,237,239,251,320,321` **在 worker 线程调用它**，与 `OpenProject`/`CloseProject` 的写操作构成数据竞争。哨兵 `NoteOwnerThreadUse()` 覆盖点不含该 lambda，因此测试中 `owner_thread_violations()==0` 的断言会**掩盖**此违反（`Undo()`/`Redo()` 同样缺哨兵）。

#### A-4 [P1] `UndoHistory` 事务合并是空操作，热路径每次编辑两次整份深拷贝

`src/editing/UndoHistory.cpp:5-17` 的"事务"分支与普通分支**代码完全相同**，注释自认"no destructive merge"。后果：连续输入 100 字符 → **200 次整份 Document 深拷贝 + 100 次索引全量重建 + 100 个 undo 条目**，与 `EditingSystem.h` 声称的"合并连续输入为一条 undo"矛盾。文档越大越慢。

#### A-5 [P1] `DocumentIndex` 每次编辑全量重建，生产零消费者

重建点 `EditingSystem.cpp:161,427,462`；`src/app` 内**零命中**。核心层精心维护的查找结构在 GUI 侧从未使用，纯热路径开销。

#### A-6 [P1] 模板变更绕过 `EditingSystem`，两条并行实现

`ProjectSession::ChangeTemplate`（`:640-661`）直接改状态、BumpRevision、手工 Push 历史、**不 Notify**；而 `EditingSystem.cpp:124-131` 内另有一条 `ChangeTemplatePayload` 分支做同样的事，且其构造点只在 Undo/Redo 内部——主分支在生产不可达。GUI 被迫在 `ProjectController.cpp:975-979` 手工补 `EmitDocumentChanged()`。

#### A-7 [P1] 失效保存被报成磁盘错误；停止后 `Enqueue` 静默丢任务却返回 `Queued`

`SaveCoordinator.cpp:106-117` 把"被更新的保存取代"归类为 `IoError`（"stale save rejected"），UI 当保存失败展示，误导用户与诊断；`:41-47` 在 `stopping_` 时**不入队但仍返回 save_id**，而 `ProjectSession.cpp:556-563` 无条件置 `Saving` 并返回 `Queued`——关机边界上"保存成功返回但什么都没写"。

#### A-8 [P2] 其他架构债

- `ProjectState::Reset()` 不清 `id_`/`settings_`（`ProjectState.h:39-43`），削弱 `ApplySaveCompletion` 的 project_id 守卫语义。
- `ProjectSession::mutable_document()` 是公共可变逃生舱，**生产零调用者**，仅测试在用。
- `BlockEditor` 持有裸 `const Document*` 并跨项目切换复用，两处解引用**未判空**。
- `ProjectState` 的 `DocumentVersion` 在撤销后**回退**且无生产消费者，与单调递增的 `ProjectRevision` 已解耦——一旦有人用它做构建缓存键即变成"接受陈旧结果"。
- `SourceMap` 逐行展开 + 线性扫描，诊断映射近似 O(D×N)。
- 公式渲染缓存是函数内 `static QHash`，打满 256 条即**整体 clear**，造成抖动且无法测试隔离。
- `TemplateRegistry` 是全局可变单例，8 处直接取用，模板定义仍硬编码。

---

## 6. 维度三：功能设计与交互

### 6.1 三级标题：核心层可用，GUI 投影层半残

这是本轮最严重的功能缺口集群，且**旧报告只覆盖了其中一部分**。

#### U-1 [P0] Subsection/Subsubsection 内的 Figure/Table 在编辑器中完全不渲染（旧报告漏报）

`BlockEditor.cpp` 的层级重建循环中：

- **Section 层**（`:2002-2013`）处理 `Paragraph`、`EquationBlock`、`Figure`（`:2013`）、`Table`（`:2080`）。
- **Subsection 层**（`:2137-2149`）：**只有 `Paragraph` 与 `EquationBlock`**。
- **Subsubsection 层**（`:2155-2168`）：**同样只有 `Paragraph` 与 `EquationBlock`**。

而插入菜单在 Subsection/Subsubsection 里**照样列出 `Figure` 与 `Import Table…`**。后果：用户插入后卡片立刻消失，**无法查看、改标题、移动或删除**，但它照样进 PDF。这是"内容仍存在、编辑器假装不存在"的最坏形态。动态实测：文档含 8 个块（Subsection 层含 1 图 1 表、Subsubsection 层含 1 图），编辑器实际只画出 1 个 `QTableWidget` + 1 张图片。

#### U-2 [P1] Subsubsection 内的插入与上移/下移确定性失败，菜单却仍提供

`ProjectController::ResolveInsertionPoint()`（`:144-176`）只遍历 `sections` / `section.blocks` / `subsections` / `subsection.blocks`，**从不进入 `subsubsections`**。动态实测，在 Subsubsection 内的块上：

```
InsertParagraphAfter → status=1 detail="insertion anchor not found"
InsertEquationAfter  → status=1 detail="insertion anchor not found"
InsertTableAfter     → status=1 detail="insertion anchor not found"
InsertFigureAfter    → status=1 detail="insertion anchor not found"
MoveNode(+1 / -1)    → status=1 detail="node not found"
InsertParagraphAfter(Subsection 内) → status=0 成功      ← 上一层正常
```

**对比证据**：拖拽用的 `MoveNodeAfter()` 走 `LocateNode` 并正确处理 `subsubsections`（`:763-797`）——**同一个重排语义，两条路径行为不一致**。附带根因：`ContainerKindFor()`（`BlockEditor.cpp:264-285`）对**任何块**都返回 `NodeKind::Section`，使 `in_subsubsection` 守卫对块永远失效。

#### U-3 [P1] 交叉引用候选集不完整

`MainWindow::RefreshReferenceItems()`（`:541-564`）只手写两级：Section 标题 + Section 直属 Figure。缺 Subsection/Subsubsection、嵌套 Figure、Table、Equation。

#### U-4 [P2] 字数统计少计三级内容

`MainWindow::CountWords()`（`:568-594`）统计 section 标题与 blocks、subsection 的 blocks，**漏掉 subsection 标题、subsubsection 标题与全部 subsubsection 正文**。动态实测：界面显示 "7 words"，真实 13。

**（正向对照：Outline 面板三级齐全、引用编号走 `VisitInlineContent` 正确——说明缺陷只在 GUI 手写循环，不在核心抽象。）**

### 6.2 未保存变更保护

#### U-5 [P0] 零保护，且未保存过的新项目恢复通道不可达

- 全仓 **`closeEvent` 零命中**；析构只 `CommitFocused()` + `StopAutosave()`（`MainWindow.cpp:776-788`）。
- `OnNewProject`（`:598-626`）提交焦点行后**不检查 Dirty** 直接切项目；`OnOpenProject`/`OpenProjectDir`（`:628-657`）**连 `CommitFocused()` 都没有**。
- `CloseProject()`（`ProjectSession.cpp:371-388`）只 `Flush()` 已入队保存，**不为当前 Dirty 创建新保存**。
- **本轮新增**：`OpenProjectWithRecovery()`（`:390-406`）先要求 `OpenProject()` 成功，而 `OpenProject`（`:319-337`）硬性要求 `project.paper` 可加载。因此**从未保存过的新项目**，即使 `autosave.paper` 已落盘且含内容，恢复逻辑**永远走不到**。

动态实测：

```
新建→编辑→关窗：project.paper exists=0, autosave.paper exists=0, reopen result=0  → 全部丢失
强制一次 autosave：project.paper exists=0, autosave.paper exists=1（含编辑内容）
                 reopen-with-recovery ok=0 recovered=0    ← 快照在盘上却永远无法恢复
对照（已保存过的项目）：ok=1 recovered=1                    ← 恢复可用
```

#### U-6 [P1] 保存失败后 autosave 永久停摆

`HandleEvent(AutosaveTickEvent)`（`ProjectSession.cpp:234-241`）只在 `persistence_state_ == Dirty` 时自动保存；一次保存失败会置 `SaveFailed`（`:624-625`），此后**只有用户再敲一次键**才回到 Dirty。动态实测：SaveFailed 期间 tick 后 autosave 字节数 `568 → 568`（未运行）。

**后果**：磁盘满/权限/占用导致一次保存失败后，用户去清理磁盘而不再输入时，**崩溃恢复安全网同时消失**，而 UI 只有状态栏一行小字，无重试入口。

### 6.3 状态显示与交互一致性

#### U-7 [P1] 保存状态指示器与实际状态不一致

保存标签由各信号各自调用 `mark_unsaved()` 手工维护（全仓 38 处），而非读取权威状态。`OnChangeTemplate()`（`:766-772`）与 `OnImportBibliography()`（`:720-764`）**都未刷新标签**，但底层都会 `MarkDirty()`。

权威信号 `ProjectController::stateChanged`（声明 `ProjectController.h:139`，发射 `:100`、`:244`）**全仓无任何 connect** —— 它携带的 `persistence_state`/`preview_state`/`revision` 从未到达 UI。这也是"预览陈旧状态"不可见的原因。

动态实测：

```
新建项目后         state=Dirty  label="Saved | …"        ← 不实（此时 project.paper 不存在）
Ctrl+S 后          state=Clean  label="✓ Saved | …"
切换模板后         state=Dirty  label="✓ Saved | …"      ← 说谎
导入 bib 后        state=Dirty  label="✓ Saved | …"      ← 说谎
```

#### U-8 [P1] Ctrl+B 快捷键冲突：Build 抢走 Bold

`BlockEditor.cpp:730` 的 tooltip 写着 `"Bold (Ctrl+B)"`，`InlineEditor.cpp:596-597` 也确实在 `keyPressEvent` 里处理 `Ctrl+B`；但 `MainWindow.cpp:290` 把**同一组合**绑到 Build QAction（tooltip `MainWindow.cpp:242` 写 "Build document (Ctrl+B)"）。窗口级 QAction 快捷键先于 keyPress 解析。动态实测：按下 Ctrl+B 后构建按钮变为 "Building ◌"，**粗体未生效**。

#### U-9 [P1] Problems 双击对多数校验诊断无响应

`OnProblemActivated`（`MainWindow.cpp:947-969`）只处理 `has_block_location()` 或 `has_file_location()||!raw_message.empty()`，两者皆假则**静默返回**。而 `Validator` 产生的 11 类诊断（缺标题、缺作者、引用键不存在、未导入 bib 等）全部是 `ForProject()`/`ForCitationKey()`，**都没有定位信息**。用户双击毫无反应，以为面板坏了。

#### U-10 [P2] 其他交互缺口

| 问题 | 证据 |
|---|---|
| 窗口标题恒为 "PaperForge"，无项目名、无 `setWindowModified` | `MainWindow.cpp:87` |
| 无法返回 Welcome 页 / 无 Close Project / 无导出或外部打开 PDF | `ShowWorkspace` 调用点仅 `89/110/624/654` |
| Undo/Redo 菜单从不 `setEnabled`，空历史点击静默无效 | `MainWindow.cpp:285-286` |
| 构建失败后预览面板无陈旧标记 | `preview_state` 只 emit 无 connect |
| Problems 过滤 chip 无互斥、取消勾选不更新 `filter_` | `ProblemsPanel.cpp:81-104` |
| bib 文件打不开时静默 `return` | `MainWindow.cpp:730-732` |
| 机构上标 GUI 侧截断为 5 且重复 `⁵`（解析支持 10、渲染支持 9） | `BlockEditor.cpp:1960-1961` |
| `tr()` 全仓 **0 次**；无 `QTranslator`/`QLocale`；约 55 处硬编码英文 | grep 结果 |
| `setAccessibleName`/`setStatusTip`/`setWhatsThis` 全仓 0 | grep 结果 |
| Reference 空态提示提到 equation，但 equations 从不进入候选 | `BlockEditor.cpp:885-889` vs `MainWindow.cpp:541-564` |
| 公式渲染失败静默 `return`，无占位或提示 | `InlineEditor.cpp:384` |

### 6.4 行内公式（P0）

#### U-11 [P0] 同步阻塞 GUI + 无异步生命周期模型

- `kTexTimeoutMs = 12000`、`kRasterTimeoutMs = 8000`（`MathPreviewRenderer.cpp:47-48`）；`RunProcess` 用 `waitForStarted(2000)` + `waitForFinished(timeout_ms)`（`:1553-1573`）。
- `RenderMathPreview` 是**自由函数**，无 `QThread`/`QtConcurrent`/generation/QPointer；`InlineEditor.cpp:383` 在 GUI 线程直接调用，`MathEditorDialog.cpp` 去抖后同样同步渲染，`BlockEditor.cpp` 显示公式卡片亦然。
- 超时**存在**（修正旧报告），但最坏 12 s + 8 s + 8 s ≈ **28 s 界面无响应**。
- 旧报告记录的 `double free or corruption (out)` 崩溃在本环境**无法用 ASan 闭环**（无 clang/ASan），**仍未关闭**。

---

## 7. 维度四：构建、测试与稳定性

### 7.1 构建系统

#### B-1 [P1] GUI 源清单重复 4–6 次，同名源被反复编译

`src/app/CMakeLists.txt` 中，同一批 GUI 源在 5 个 target 上重复枚举：

| 源文件 | 重复次数 |
|---|---|
| `InlineEditor.cpp` | **6** |
| `EditorItemKind/MathPreviewRenderer/ProjectController/InlineMathObjectRenderer/CitationObjectRenderer/MathEditorDialog/ProblemsPanel.cpp` | 5 |
| `MainWindow/BlockEditor/PopupList/PdfPreview/OutlinePanel/WelcomePage.cpp` | 4 |

即：`InlineEditor.cpp` 被编译 6 次，整棵 GUI widget 树被编译 5 遍。这直接导致构建时间与 `build/` 体积（实测 765 MB）成倍增长，且新增一个文件必须手工同步 5 处，极易漂移成"某 target 用旧代码"。旧报告 P1-07 建议的 `pf_gui_components` 库**未落地**。

#### B-2 [P2] 无 install/package 规则，二进制不可重定位

全仓 `install(`/`CPack`/`export(` **零命中**。GUI 靠编译期宏 `PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}"` 定位 runtime（`src/app/CMakeLists.txt:30,45,69,123,133`），因此**二进制不可重定位、无法打包分发**。CLI 更严重：`ProjectSession.cpp:78-83` 用 `__FILE__` 反推 install_root，**构建机绝对路径被编进二进制**（实测 `strings src/paperforge` 命中该路径 2 次），而 CLI 目标从未定义 `PF_INSTALL_ROOT`。

#### B-3 [P2] `find_program` 无 FOUND 校验

`CMakeLists.txt:22-26` 未检查 `PF_TECTONIC_EXE` 是否找到，`PF_TECTONIC_EXE-NOTFOUND` 会被原样编译进二进制；全仓无 `FATAL_ERROR` 拦截。`find_package(Qt6 QUIET COMPONENTS Widgets Test)` 也未校验组件完整性。

### 7.2 测试体系

#### B-4 [P1] 假绿通道（比旧报告更广）

| 位置 | 行为 |
|---|---|
| `tests/TestInlinePdf.cpp:73,85,96,112,125,132` | 6 处"失败即裸 `return`"，**仍报 `[ OK ]`**（含 `MARKS LOST IN DOCUMENT`、构建失败、构建超时） |
| `tests/TestRuntime.cpp:85,141,223,270` | 4 个测试在 runtime 不健康时打印 "skipping" 并 `return` → **通过** |
| `tests/TestEndToEnd.cpp:274-290` | `EndToEndTectonicBuild` **对构建是否成功没有任何断言**，失败仅打印诊断后照常通过 |
| `tests/TestCitationGui.cpp:398-401` | `pdftotext` 空输出时打印 "skipping text match" 并 `return` |

框架仅按 `CurrentFailed()` 判定，所以这些路径**在干净 clone（无 runtime）上全绿**。旧报告担忧的"绿灯不能证明真实链路"今天仍然成立。

#### B-5 [P1] 两个 GUI 测试二进制从未被 CTest 执行

`ctest -N` 只注册 **3 个**：`inline_editor`、`gui_input_persistence`、`pf_tests`。而 `paperforge-gui-smoke`（**39 个断言**）与 `paperforge-ui-verify`（截图工具）虽被构建，**从未注册 `add_test`**。README 宣称的"全流程冒烟测试"在 CTest 中不存在。

#### B-6 [P1] 无 sanitizer、无 CI、无 label 分类

无 ASan/UBSan/TSan job，无 CI 工作流；测试无 `LABELS`，因此**无法用 `ctest -L` 区分"纯逻辑"与"真实 TeX"**，"跳过"与"通过"在回报中不可区分。

#### B-7 [P2] 测试临时目录全部是固定共享路径（本轮实测导致污染）

全仓约 **40 处** `temp_directory_path() / "固定名"`（`/tmp/pf-font-test`、`/tmp/pf-ieee-marks`、`/tmp/pf-e2e-*`、`/tmp/pf-build-test`、`/tmp/pf-save-test` …），并非 RAII 随机目录。加上 `RuntimeManager` 把健康检查工作区放在 **runtime 安装目录内**（`<runtime>/texlive/texmf-var/health`，`:122`）且**只在成功路径清理**（`:148`），导致：

- **并发测试互相破坏**（本轮亲历：并行运行时 `RuntimeIsHealthy` 假失败，报 `I can't find file 'main.aux'`；串行即 140/140 全绿）；
- 失败时在 runtime 安装目录**残留垃圾**；
- **只读/系统级 runtime 安装上健康检查必然失败**，被误报为 `RuntimeStatus::Corrupted`。

另有 sleep 同步的 flaky 风险（`TestBuild.cpp` 的 `sleep_for(20ms/300ms)` 配 `debounce{150ms}`、GUI 测试 `QThread::msleep(5)` 轮询）。

### 7.3 保存与 I/O 稳定性（P0）

#### B-8 [P0] 保存不是单一事务，错误被吞

- `Save()` 调用 `WriteFileAtomically()` **忽略返回值**（`ProjectSession.cpp:572`）。
- `ImportBibliography()` 同样忽略（`:723`），却照旧 `BumpRevision()`、`MarkDirty()`、`RequestBuild()` 并返回 **Ok**。
- `EnsureDirectories()` 四次 `create_directories(path, ec)` **全部丢弃 `ec`**（`:290-299`）；`NewProject()` **无论目录是否创建成功都 `return true`**（`:301-316`）。
- `ProjectPersistence.cpp:660` 用 `std::rename` 覆盖目标，Windows 上行为不满足跨平台原子替换需求；且与 `ProjectSession.cpp:33` 使用的 `std::filesystem::rename` 不一致。
- `SaveCoordinator::Flush()` 无超时（无限等 condition_variable）。

**风险**：`project.paper`（worker 异步写）与 `references.bib`（应用线程同步写）是两条独立路径、两个失败维度，可处于不同 revision；UI 报"已保存/导入成功"但数据未完整落盘。

#### B-9 [P0] 路径信任边界缺失

`bibliographyPath` 与 asset `path` 从 JSON **原样接收**（`ProjectPersistence.cpp:466-468`、`:576-577`），随后直接用于 `project_dir / path`（`ProjectSession.cpp:351`）、`"assets/" + relative_path`（`SnapshotFactory.cpp:24-28`）、`request.workspace / file.path` 与 `copy_file`（`Compiler.cpp:45,55-60`）。全仓**不存在**任何 `ResolveProjectRelativePath`/canonical 校验。`AssetManager` 对新导入资产有净化，但**反序列化路径完全绕开它**。恶意/损坏的 `project.paper` 可用绝对路径或 `../` 读取、复制或覆盖项目外文件。

### 7.4 稳定性热点

#### B-10 [P2] 启动路径同步编译、TEMP-DEBUG 常开、Windows 分支退化

- `ProjectSession` 构造函数（`:84-85`）**每次创建都同步跑一次真实 pdfLaTeX 健康检查编译**，而 GUI 在 `ProjectController.cpp:58` 构造期调用 → **启动即阻塞**。
- TEMP-DEBUG 的项目内 dump 在生产路径**无条件开启**（`ProjectSession.cpp:91-96` + `BuildCoordinator.cpp:228-262,318-328`），每次构建都把完整 LaTeX 源、全部 asset、PDF 复制进用户项目目录。
- Windows 编译分支丢弃超时/取消/流式输出（§4.2 S-5）。
- 公式缓存在 256 条时**整体清空**（`MathPreviewRenderer.cpp:1800`），无有界 LRU。

---

## 8. 本轮新增的发布阻断项（含独立复现）

### 8.1 反序列化可致进程崩溃（数值溢出）— P0

**这是旧报告 P0-05 的真实病因，但旧报告给出的机制是错的。**

关键代码：

```cpp
// src/core/Json.cpp:318-326
if (is_float) {
    return std::make_unique<JsonValue>(std::stod(num));   // ← 未加保护！
}
try {
    return std::make_unique<JsonValue>(static_cast<std::int64_t>(std::stoll(num)));
} catch (...) {
    return std::make_unique<JsonValue>(std::stod(num));   // ← 这里也能再抛
}
```

`Parser::Parse()` **只 catch `ParseAbort`**（`:122-124`），`JsonParse`（`:336-339`）、`ProjectSerializer::Deserialize`（`ProjectPersistence.cpp:447`）、`ProjectPersistence::Load`（`:670-693`）**均无 try/catch**；`src/app/main.cpp` **也没有顶层 try/catch**。

**独立复现**（链接仓库自身静态库）：

```
project.paper 内容: {"schemaVersion":"2","projectId":"p1","revision":1e999,...}
→ Load LEAKED exception: stod
对照组（合法文件）: Load status=0 detail=''   ← 正常
```

**影响**：打开一个含超大数值字面量的项目文件，`std::out_of_range` 会穿透 `Load` → `ProjectSession::OpenProject` → Qt 槽函数。异常逃出 Qt 槽在事件循环中即 `std::terminate`。**这是一个可被外部文件触发的确定崩溃。**

### 8.2 反序列化可致栈溢出崩溃（深嵌套）— P0

`ParseValue` ↔ `ParseObject`/`ParseArray` 相互递归，**全程无深度计数**（`Json.cpp:164-219`）；对比 `DumpTo` 有 `depth` 参数而 parser 没有。`Load` 也**无文件大小上限**（`ss << in.rdbuf()`）。

**独立复现**（同一复现程序，最小输入为约 100 KB 的 `[[[[…]]]]`）：

| 输入深度 | 结果 |
|---|---|
| 2,000 / 5,000 / 8,000 | 正常解析 |
| 10,000 | 解析成功，**析构时 SIGSEGV**（递归析构爆栈） |
| 50,000 / 200,000 | **解析阶段 SIGSEGV** |

**修复建议**：`ParseNumber` 内捕获数值异常并返回结构化错误；`Deserialize`/`Load`/`OpenProject` 增加异常边界；parser 加最大深度/节点数上限；`Load` 前置文件尺寸上限；补 malformed corpus/fuzz 测试，确保"任意输入只产生成功对象或结构化错误"。

### 8.3 已修复/未修复状态一览

| 旧报告条目 | 本轮状态 |
|---|---|
| 生成物入库（22 个） | **仍成立**：22 个 / 859,631 B；`.gitignore` 仍无 `CMakeFiles/`、`CTestTestfile.cmake`、`cmake_install.cmake`、`*.ninja*`（`git check-ignore` 全部 NOT IGNORED） |
| tectonic-cache 673 文件 | **仍成立**，且**新发现** `tools/tectonic-cache/.cache/Tectonic/` 是约 **43 MB 的近似整份重复副本**（324 组同 hash） |
| `GMVG3/` 实验项目在仓库中 | **仍成立**（14 文件），且 `.gitignore` 写的是 `/GMVG/`，与实际目录名不匹配 |
| 无 `.clang-format`/CI/warning policy | **仍成立** |
| CMake 源清单重复 | **仍成立且量化**（4–6 次） |
| 测试静默通过 | **仍成立且范围更大** |
| `TEMP-DEBUG`、机构上标 9、Config 注释称 Tectonic、`TemplateRegistry` 字典序 | **全部仍成立** |
| runtime 缺失 | **已过期**（见 §3.1） |

---

## 9. 统一代码规范（建议作为仓库强制规则）

### 9.1 文件与模块

1. 一个文件一个主要职责；超过约 500 行须有明确单一职责依据。
2. 领域层禁止 Qt 类型；Qt 适配只位于 `src/app`。
3. **按 target 收窄 include 目录**，`pf_core` 不再 PUBLIC 暴露整个 `src/`，使分层由 CMake 而非约定强制。
4. `Toolchain.h` 移入 `src/core/`（或新建 `pf_contract`），消除 `template → build` 反向边。
5. 生成文件、缓存、用户项目、构建产物禁止进入源码目录。

### 9.2 命名

1. 类型/enum/public method `PascalCase`；局部变量与私有成员 `snake_case` + 尾 `_`；常量 `kPascalCase`。
2. public 聚合字段不加尾 `_`（把该约定成文）。
3. enum class 统一显式 `: std::uint8_t`（补齐现存 6 处）。
4. **产品名冻结为唯一一个**，同步窗口标题、`setApplicationName`、CMake `project()`、错误文案、文档、用户目录名。

### 9.3 格式

1. 落 `.clang-format`（建议 LLVM + 4 空格 + 行宽 100），一次独立 commit 全仓格式化。
2. 统一 `Type* value` / `Type& value`。
3. `if/for/while` 一律加大括号（现存 406 处待改）。
4. include 顺序：本文件头 → C++ 标准库 → Qt/第三方 → 项目头（现存 37 处违规）。

### 9.4 所有权与生命周期

1. 非 QObject 独占资源用 `unique_ptr`；共享仅在确有语义时用 `shared_ptr`。
2. QObject 依赖 Qt parent；**跨事件循环引用一律 `QPointer`**（当前仅 3 处）。
3. 禁止回调捕获生命周期不受 context object 约束的裸 QObject。
4. worker 不得读活状态：所有 worker 输入必须是值对象或不可变快照（修 `debug_dump_dir`）。
5. 线程 RAII stop/join，`Flush()` 必须带超时。

### 9.5 错误处理

1. 可恢复错误统一 `Result<T, Error>`；**禁止 `bool` 丢失上下文**（`WriteFileAtomically` 加 `[[nodiscard]]`）。
2. 文件系统调用必须检查 `error_code`；**不允许忽略保存、建目录、copy、rename、process start 的结果**。
3. 错误对象至少含 code、用户消息、技术 detail、相关路径/节点、可否重试。
4. 错误码集中登记，禁止散落字符串字面量。
5. GUI 不拼接核心错误字符串，只渲染结构化错误。
6. 进程入口与所有反序列化边界必须有异常屏障。

### 9.6 并发

1. 可变 `ProjectState` 只属于 owner 线程；哨兵必须覆盖所有跨线程入口。
2. 所有异步结果携带 project/revision/request identity。
3. 取消是显式状态，不靠析构碰运气。
4. worker 回调不得操作 QWidget/QPixmap。
5. 每个异步模块必须测试：旧结果、取消、关闭、切换项目、超时、异常。

### 9.7 数据与路径

1. 外部 JSON、BibTeX、Excel、图片、LaTeX 一律视为不可信输入。
2. **实现唯一 `ResolveProjectRelativePath(root, relative)`**：拒绝绝对路径、根路径、盘符、`..`、空组件与 canonical 后逃逸 root 的路径；反序列化与 staging 各校验一次。
3. relative path 与 template id 使用强类型，不再传裸 `std::string`。
4. 反序列化必须校验类型、范围、唯一性、引用完整性与资源上限（深度/节点数/表格行列/字符串长度）。
5. 未知的**更高** schemaVersion 必须硬失败，不得加载后静默降级覆盖。

### 9.8 测试

1. 禁止"环境缺失则 return 成功"；环境型测试用 CTest `LABELS` + `SKIP_RETURN_CODE` 显式表达。
2. 测试临时目录必须随机/RAII，**禁止固定共享名**；健康检查不得写入产品安装目录。
3. 分类：unit / integration / GUI / runtime / sanitizer / fuzz / performance。
4. 每个已修复崩溃保留最小回归测试。
5. CI 必须从 clean checkout 开始；runtime 健康检查作为**必须通过的独立 job**。

---

## 10. 修复优先级与实施顺序

### 第一阶段：稳定性封口（P0）

| 顺序 | 任务 | 关键动作 |
|---|---|---|
| 1 | **未保存变更保护** | 新增 `closeEvent(QCloseEvent*)` 与统一 `MaybeSaveBeforeDestructiveNavigation()`（关窗/新建/打开/最近项目共用）；先 `CommitFocused()` 再读真实 `PersistenceState`；Dirty/Saving/SaveFailed 一律"保存/放弃/取消"；**补"仅 autosave 存在"的恢复分支** |
| 2 | **反序列化崩溃闭环** | 修 `std::stod` 异常穿透、加解析深度/尺寸上限、加异常屏障；补 malformed corpus/fuzz |
| 3 | **保存事务与错误传播** | `EnsureDirectories` 返回 `Result<void, IoError>`；文献写入失败不得 bump revision/报成功；`WriteFileAtomically` 加 `[[nodiscard]]`；统一跨平台 replace |
| 4 | **路径安全边界** | 实现唯一 `ResolveProjectRelativePath`，反序列化与 staging 双重校验 |
| 5 | **行内公式异步化** | 独立 `MathRenderService`（worker 线程 + `editor_id/formula_id/generation` 校验 + `QPointer`）；建 ASan/UBSan 门禁后关闭 `double free` |

### 第二阶段：功能与状态纠偏（P1）

1. **一次 `VisitBlocks` 重构同时消掉四项缺陷**：Subsection/Subsubsection 的 Figure/Table 渲染（U-1）、插入/移动失败（U-2）、引用候选不全（U-3）、字数少计（U-4）。顺手统一 `ContainerKindFor` 与 `⋯` 菜单到 `InsertOptionsFor`。这是投入产出比最高的一项。
2. **保存指示器与状态单一真相**：接线死信号 `stateChanged`，删除 38 处手工 `mark_unsaved`；`saveFinished` 携带 `saved_revision`。
3. **Figure/Table `\label`**：可引用即无条件输出 `\label`，与 alt 解耦。
4. **表格入口**：短期改名 `Insert Empty Table` 并补行列设置；或按 F-04 做真实 CSV 导入。
5. **autosave 在 SaveFailed 下继续工作** + 提供重试入口。
6. **Ctrl+B 冲突**、Problems 双击无响应、`@` 菜单残留文档等交互缺陷。
7. 接线 `DocumentChangedEvent` 载荷，实现增量 GUI 更新（A-2 是前置条件）。

### 第三阶段：工程化

1. 清理 22 个生成物与 `GMVG3/`，补 `.gitignore` 规则，删除重复缓存副本。
2. 落 `.clang-format`/`.clang-tidy`/`.editorconfig`；加 warning policy；加 CI 矩阵（GCC/Clang、format、offscreen GUI、ASan+UBSan、独立 runtime job）。
3. 抽 `pf_gui_components` 库，拆分 `BlockEditor`/`MathPreviewRenderer`/`MainWindow`；统一 `ToQ`。
4. 统一产品名与文档；补 LICENSE 与第三方许可；`runtime/` 改为带版本与校验和的 release artifact。
5. 加 `install()`/CPack，消除 `__FILE__` 反推 install_root。

### 第四阶段：产品扩展

按业务价值排序：文档投影层、表格导入、增量 GUI、模板 manifest、跨平台发布、版本历史与恢复（revision journal）、国际化与可访问性、性能基线。

---

## 11. 最终评价

**这个项目的真实水平高于旧报告给人的印象，也低于它自己 documentation 宣称的水平。**

高于旧印象的部分：领域层确实做到了 Qt-free；`PreviewGate`、不可变快照、类型化事件、保存 latest-wins、落盘前回读校验这批设计是**有工程品味**的，不是练手项目常见的水准；140 + 51 + 86 个断言在真实 runtime 上全绿；CLI 复用同一 `ProjectSession`，证明分层不是纸面的。

低于自我宣称的部分：README 描述的"遍历逻辑只写一份""三级标题全链路可用""Import Table""真实构建测试"**都与代码不符**；保存指示器会说谎；Subsection 里的图片在编辑器里根本不存在；损坏的项目文件能让程序崩溃；而这一切在无 CI、无警告开关、无 sanitizer、测试可假绿的环境里长期无人发现。

**核心矛盾不是"功能太少"，而是"新功能的增长速度超过了 GUI 投影层、I/O 边界与工程门禁的收口速度"。** `DocumentTraversal` 早已提供正确的遍历抽象，GUI 却仍在五个地方手写嵌套循环；`DocumentChangedEvent` 已经把变更载荷算出来了，却没人接收；`EnsureDirectories` 已经拿到了 `error_code`，却没人检查。**这个项目的下一阶段价值不在于再加一种 block 类型，而在于把已经写出来的正确抽象真正接到生产路径上。**

完成前三阶段后，这将是一个有说服力的工程作品；在此之前，它是一个架构方向正确、但尚不可交付的原型。

---

## 附录 A：问题总表

| 编号 | 级别 | 维度 | 问题 | 证据 | 旧报告 |
|---|---|---|---|---|---|
| P0-1 | P0 | 功能 | 未保存变更零保护 + 新项目恢复通道不可达 | `MainWindow.cpp:776-788,598-626,636-657`；`ProjectSession.cpp:371-388,390-406` | 成立，本轮补强 |
| P0-2 | P0 | 功能 | 行内公式同步阻塞 GUI（最坏 ~28 s），无异步模型 | `MathPreviewRenderer.cpp:47-48,1553-1573`；`InlineEditor.cpp:383` | 成立 |
| P0-3 | P0 | 稳定性 | 项目内相对路径可逃逸 | `ProjectPersistence.cpp:466-468,576-577`；`SnapshotFactory.cpp:24-28`；`Compiler.cpp:45,55-60` | 成立 |
| P0-4 | P0 | 稳定性 | 保存非事务、I/O 错误被吞 | `ProjectSession.cpp:290-299,301-316,572,723`；`ProjectPersistence.cpp:660` | 成立 |
| P0-5 | P0 | 稳定性 | 反序列化 `std::stod` 异常穿透 + 递归无深度上限 → 崩溃 | `Json.cpp:320,325,164-219`；**本轮独立复现** | 病因错、结论对 |
| P0-6 | P0 | 功能 | Subsection/Subsubsection 内 Figure/Table 不渲染 | `BlockEditor.cpp:2137-2149,2155-2168` vs `:2013,2080` | **漏报** |
| A-1 | P0 | 架构 | 保存指示器谎报"已保存"（丢 `saved_revision`） | `ProjectController.cpp:97-99`；`MainWindow.cpp:675-681`；`ProjectSession.cpp:617-623` | 漏报 |
| A-2 | P1 | 架构 | `DocumentChangedEvent` 零消费者 → 全量重建 | `ProjectSession.h:250-253`；`EditingSystem.cpp:482-496`；`ProjectController.cpp:241-245` | 漏报 |
| A-3 | P1 | 架构 | worker 侧读活状态，突破单所有者规则，哨兵有盲区 | `ProjectSession.cpp:91-96`；`BuildCoordinator.cpp:233,237,239,251,320,321` | 漏报 |
| A-4 | P1 | 架构 | Undo 事务合并空操作；每次编辑两次整份深拷贝 | `UndoHistory.cpp:5-17`；`EditingSystem.cpp:136,155,161` | 漏报 |
| A-5 | P1 | 架构 | `DocumentIndex` 全量重建但生产零消费者 | `EditingSystem.cpp:161,427,462` | 漏报 |
| A-6 | P1 | 架构 | 模板变更双实现、双通知通道 | `ProjectSession.cpp:640-661` vs `EditingSystem.cpp:124-131` | 漏报 |
| A-7 | P1 | 架构 | 失效保存报成 `IoError`；停止后静默丢任务返回 `Queued` | `SaveCoordinator.cpp:41-47,106-117`；`ProjectSession.cpp:556-563` | 漏报 |
| A-8 | P2 | 架构 | Reset 不清 id/settings、裸指针、DocumentVersion 回退、缓存整体清空、全局单例等 | 见 §5.3 | 部分 |
| U-2 | P1 | 功能 | Subsubsection 内插入/移动确定性失败，菜单仍提供 | `ProjectController.cpp:144-176,875-947` | 成立 |
| U-3 | P1 | 功能 | 引用候选只两级 | `MainWindow.cpp:541-564` | 成立 |
| U-4 | P2 | 功能 | 字数少计三级内容 | `MainWindow.cpp:568-594` | 成立 |
| U-6 | P1 | 功能 | 保存失败后 autosave 停摆 | `ProjectSession.cpp:234-241,624-625` | 漏报 |
| U-7 | P1 | 功能 | 保存指示器与真实状态不一致；`stateChanged` 死信号 | `MainWindow.cpp:294-297,766-772,720-764`；`ProjectController.h:139` | 漏报 |
| U-8 | P1 | 功能 | Ctrl+B 被 Build 抢走 | `MainWindow.cpp:242,290` vs `BlockEditor.cpp:730`；`InlineEditor.cpp:596-597` | 漏报 |
| U-9 | P1 | 功能 | Problems 双击对 11 类诊断无响应 | `MainWindow.cpp:947-969`；`Diagnostic.h:61-68,85-95` | 漏报 |
| U-10 | P2 | 功能 | 窗口标题/无导出/Undo 禁用/预览陈旧/i18n/a11y 等 | 见 §6.3 | 部分 |
| S-1 | P1 | 规范 | 工程门禁文件全缺、无 CI、无 LICENSE | 逐项存在性检查 | 成立 |
| S-2 | P1 | 规范 | CMake 无 warning policy（实测 94 条警告） | 各 CMakeLists；补编译实测 | 成立 |
| S-3 | P1 | 规范 | 品牌四套命名 | `MainWindow.cpp:87`；`main.cpp:7-8`；`Compiler.cpp:509` | 成立，补用户可见证据 |
| S-4 | P1 | 规范 | 错误处理双轨、I/O 失败被吞、`catch(...)` | `Result.h`；`ProjectSession.cpp:572,723` | 成立 |
| S-5 | P1 | 规范 | Windows 分支丢弃取消/超时/流式日志 | `Compiler.cpp:210-217` | 漏报 |
| S-6 | P2 | 规范 | 格式大面积不统一（406 处无大括号等） | 量化表 §4.2 S-6 | 建议级 |
| S-7 | P2 | 规范 | TEMP-DEBUG 常开、死 TectonicCompiler、死抽象 | `ProjectSession.cpp:66-72,91-96`；`BuildCoordinator.cpp:228,318` | 部分 |
| S-8 | P2 | 规范 | `ToQ` 五份、测试框架两套、CMake 清单重复 | 见 §4.2 S-8 | 部分 |
| S-9 | P2 | 规范 | 超大文件与 420 行函数 | `BlockEditor.cpp:1781` 等 | 成立 |
| B-1 | P1 | 构建 | GUI 源清单重复 4–6 次 | `src/app/CMakeLists.txt` | 成立 |
| B-2 | P2 | 构建 | 无 install/CPack；`__FILE__` 反推 install_root | `ProjectSession.cpp:78-83` | 漏报 |
| B-3 | P2 | 构建 | `find_program` 无 FOUND 校验 | `CMakeLists.txt:22-26` | 漏报 |
| B-4 | P1 | 测试 | 假绿通道（6+4 处 return、E2E 无断言） | `TestInlinePdf.cpp:73,85,96,112,125,132`；`TestRuntime.cpp:85,141,223,270`；`TestEndToEnd.cpp:274-290` | 成立，范围更大 |
| B-5 | P1 | 测试 | 2 个 GUI 二进制未注册 CTest（39 断言从不执行） | `src/app/CMakeLists.txt:33,48`；`ctest -N` = 3 | 漏报 |
| B-6 | P1 | 测试 | 无 sanitizer/CI/label | 全仓 | 成立 |
| B-7 | P2 | 测试 | 固定共享临时路径 → 并发污染、只读安装误报 | 约 40 处；`RuntimeManager.cpp:122,148` | 漏报 |
| B-10 | P2 | 稳定性 | 启动同步编译、TEMP-DEBUG 常开、Windows 退化 | `ProjectSession.cpp:84-85,91-96` | 部分 |

## 附录 B：复核方法与可复现命令

```bash
# 1) 干净构建
cmake -S . -B build-audit -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-audit -j4                  # → exit 0, 155/155

# 2) 测试（必须串行：套件使用固定共享路径）
export QT_QPA_PLATFORM=offscreen
./build-audit/tests/pf_tests                   # → 140 tests, 0 failures
./build-audit/src/app/paperforge-inline-editor-test   # → 51 tests, 0 failures
./build-audit/src/app/paperforge-gui-persistence-test # → 86 checks
ctest --test-dir build-audit                   # → 3/3 passed

# 3) 注册测试缺口
ctest --test-dir build-audit -N                # → 仅 3 个；gui-smoke/ui-verify 缺失

# 4) JSON 崩溃复现（最小输入）
#   1e999 → 异常穿透 Load；[[[[…]]]] 深度 50k → SIGSEGV
#   复现程序源码见本报告 §8.1、§8.2 说明

# 5) 警告量化
#   用 compile_commands.json 追加 -Wall -Wextra -Wpedantic -Wshadow -fsyntax-only
#   → 94 warnings / 0 errors

# 6) 仓库卫生
git ls-files | grep -E "CMakeFiles/|CTestTestfile|cmake_install|\.ninja" | wc -l   # → 22
git ls-files tools/tectonic-cache | wc -l                                          # → 673
git check-ignore -v CMakeFiles/rules.ninja                                         # → 未忽略
```

**局限**：本环境无 clang 与 ASan 运行配置，旧报告记录的 `double free or corruption (out)` 内存安全问题**未能复现或排除**，仍应视为未关闭的发布阻断项；此外 §2.3 的并发污染已证明本测试套件不可并行运行，任何并行执行得到的测试结果都不可采信。
