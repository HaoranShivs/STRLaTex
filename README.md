# PaperForge V1 (Core + CLI + GUI)

基于 C++20 的结构化学术论文编辑器，按《PaperForge V1 架构基线》完成。

用户编辑论文的**语义结构**，系统内部维护结构化 Document Model，通过 LaTeX Renderer
和 Tectonic 自动生成 PDF，提供实时编译（debounce / latest-wins / cancellation）、
引用管理、错误定位（SourceMap → Node）、保存恢复和 Undo/Redo。

```
User → Qt6 GUI / CLI
         ↓
    EditCommand → EditingSystem → Document Model
         ↓                ↓
  ProjectRevision   ProjectSnapshot / BuildSnapshot (immutable)
         ↓                ↓
  ProjectSession  →  BuildCoordinator / SaveCoordinator（worker 线程）
         ↓                ↓
   ApplicationEvent（typed）→ PreviewGate → PreviewUpdate → GUI
```

线程模型（single-owner application state + immutable async pipeline）见
[docs/architecture-async.md](docs/architecture-async.md)；仓库布局约定见
[docs/REPOSITORY.md](docs/REPOSITORY.md)。

## 模块结构

```
src/
├── core/          StrongId / ProjectRevision / Result / Diagnostic / Json / IdGenerator
├── document/      Document / DocumentEditor / DocumentIndex / InlineText
│                  / DocumentTraversal（NodeAddress + LocateNode + Visit*）
├── editing/       EditCommand / EditingSystem / UndoHistory / AnchorResolver
├── project/       ProjectState / SnapshotFactory / ProjectSession
│                  / ApplicationEvent / PreviewGate / PreviewUpdate
├── template/      TemplateRegistry（Generic Article / IEEE Conference）
│                  / TemplateCapabilities（max_heading_depth）
├── asset/         AssetManager（staging → 注册分离，内容哈希，图片尺寸嗅探）
├── bibliography/  BibTeX 解析 / BibliographyService / 搜索
├── validation/    Validator（语义 / 模板层 Diagnostic）
├── render/        LatexRenderer / SourceMap / BuildPackage
├── build/         ICompiler / TexLiveCompiler（latexmk + 自带 TeX Live）
│                  / TectonicCompiler / CompilerFactory / RuntimeManager
│                  / BuildCoordinator（debounce + latest-wins + 两层取消 + 取消/超时）
├── template/      TemplateRegistry（… + TemplateToolchainRequirement：
│                  模板声明编译引擎，两模板均 PdfLatex + BibTex）
├── persistence/   ProjectSerializer（project.paper JSON）/ 原子保存
│                  / SaveCoordinator（worker 线程 + 完成事件）
│                  / ProjectMigrator（schema V1→V2）
└── cli/           paperforge 命令行工具
src/app/           Qt6 GUI（paperforge-gui）：结构化编辑器 + Outline + Problems
│                  + Build 状态 + PDF 预览（pdftoppm 渲染）
│                  + InlineEditor（富文本 Text 行：Bold/Italic/行内公式/
│                    Citation/Reference token + 格式工具栏）
tests/             106 个单元 + 端到端 + 场景回归测试（含真实 Tectonic 构建、
│                  自动保存恢复、M1 线程/协议场景、遍历与三级标题、
│                  Schema migration、GUI 冒烟）
docs/              architecture-async.md（线程模型）/ structure-semantics.md
│                  （遍历 + 三级标题 + migration）/ inline-editor.md
│                  （富文本）/ toolchain.md（编译工具链 + IEEE 字体根因）
│                  / REPOSITORY.md（仓库布局）
runtime-tests/     font-test.tex / ieee-test.tex（runtime 健康检查与手工复现）
tools/build-runtime/  build_runtime.sh + packages.txt + texlive.profile
│                  （可复现构建自带 Portable TeX Live，user-mode 免 root）
tools/
├── bin/tectonic         静态链接的 tectonic 0.15.0
└── tectonic-cache/      LaTeX bundle 缓存（离线构建可用）
```

## 关键架构约束的落实

| 基线要求 | 实现 |
| --- | --- |
| Text 行内富文本 | `InlineEditor` 直接编辑 `InlineContent`：Bold/Italic 组合、行内公式、Citation/Reference token（不可编辑内部标识、整体删除、点击选中）；提交走 `EditParagraphRich`，不做字符串往返。**标记以 fragment 为准**（`charFormat()` 在光标处报的是前一个字符，逐字符读会让 run 边界错位、斜体丢首字符），token 从 fragment 属性读取，提交/焦点/量高同时识别富行 |
| 渲染标记组合 | `Strong + Emphasis` → `\textbf{\emph{...}}`，不再互相覆盖 |
| 编译引擎由模板决定 | `TemplateToolchainRequirement` → `CompilerFactory` → `TexLiveCompiler`；IEEEtran 强制 ptm(Times)，XeTeX 引擎下字体选择失效，故两模板钉住 pdfLaTeX；PDF 中 Regular/Bold/Italic 三种字型可验证 |
| 自带 TeX 环境 | `runtime/texlive`（`tools/build-runtime/` 重建，user-mode 免 root）；编译进程只看 runtime 的 PATH/HOME/TEXMF*，用户 TeX 与 PATH 不影响结果；runtime 不健康时报 `E-RUNTIME` 而非文档错误 |
| 真实 PDF 回归 | `IeeeMarksReachPdfLatex`（IEEEtran 四种标记组合经 pdfLaTeX 真实构建）、`FontTestBuildsWithoutSubstitution`、`BoldAndItalicReachTheBuiltPdf`（端到端 + `pdffonts` 字型断言） |
| 粘贴规则 | `Ctrl+V` 保留 Bold/Italic、丢弃字体/字号/颜色/背景；`Ctrl+Shift+V` 只留文本；PDF 硬换行继续重排 |
| Document Tree 永远 structurally valid | 闭式 schema：`Section/Subsection/Subsubsection` + `Paragraph/Figure/Table/DisplayEquation` 为固定 `struct` + `std::variant`；Table 由 `MakeTable` 工厂保证矩形 |
| 遍历逻辑只写一份 | `DocumentTraversal` 提供 `NodeAddress` + `LocateNode/VisitNodes/VisitBlocks/VisitHeadings/VisitInlineContent`；Renderer、Validator、DocumentIndex、Outline、删除/移动定位全部复用，不再各自维护嵌套循环 |
| 三级标题层级 | `Subsubsection` 挂在 `Subsection` 下，Insert/Rename/Delete/Move/Undo/Redo 全链路可用；Renderer 产出 `\subsubsection{...}\label{node-id}` |
| 模板能力约束 | `TemplateCapabilities::max_heading_depth`：GUI 依此过滤插入菜单与 `/` 菜单，Validator 对超深标题报 `W-HEADING-DEPTH` |
| Schema migration | `ProjectMigrator`：加载时探测版本并**在内存里**迁移（V1→V2），磁盘文件不动，保存时写入当前版本 |
| NodeId 唯一且稳定 | `StrongId<Tag>` 强类型 ID；序列化/反序列化保留原 ID |
| ProjectRevision 单调递增 | `ProjectState::BumpRevision()` 是唯一入口；Undo/Redo 也产生新 Revision（测试验证） |
| Template Change 不改 Document | `DocumentVersion` 不变、`ProjectRevision +1`、可进入 Undo（测试验证） |
| mutable state 只属于应用线程 | `ProjectState`/`Document`/`EditingSystem` 仅由 UI 线程访问；`owner_thread_violations()` 哨兵 + 场景测试断言为 0 |
| 后台不读 Live Document | `SnapshotFactory` 深拷贝出 `shared_ptr<const Document>` / `SerializedProject`；worker 只收值对象 |
| 异步结果只走 typed event | `ApplicationEvent` = `variant<BuildPhaseChanged, BuildResultReady, SaveCompleted, AutosaveTick>`；`ProcessApplicationEvents()` 在应用线程分发 |
| 最多 1 Active + 1 Pending | `BuildCoordinator` 用 `optional<Active/Pending>` 而非队列；测试验证 Rev1 被取代后永不完成 |
| Stale Result 不进 UI | `PreviewGate` 纯函数按 **project id → revision → snapshot id → build id** 判定；stale 在应用线程丢弃 |
| 预览携带 artifact 身份 | `PreviewUpdate{ project_id, build_id, revision, pdf }` 取代 `buildFinished(bool, pdf_path)` |
| Compiler 不知道 Document | Compiler 只见 `BuildPackage`；错误经 `DiagnosticMapper + SourceMap` 映射回 NodeId |
| LaTeX 隔离 | Document Core 无任何 `\section`/`\label` 字符串知识；全部在 Renderer |
| 保存串行 + 旧不覆盖新 | `SaveCoordinator` 单 worker FIFO + revision 比较；原子写 = 临时文件 → 校验 → rename |
| 保存期间继续编辑仍是 Dirty | save 完成事件带回被写 revision，只有等于当前 revision 才置 `Clean`（测试验证） |
| Asset 导入与注册分离 | `AssetManager::Stage` 只暂存；真正插入文档时才 `Register` |
| 异步插入用 Stable Anchor | `AnchorResolver` 按当前 Revision 重新解析插入点 |
| 全操作可撤销 | Undo 历史存储操作前后的**全文快照**（before/after），删除 Section 等结构性操作也能完整恢复，NodeId 跨 Undo 保持稳定 |
| 自动保存不干扰状态机 | 定时器线程只发 `AutosaveTickEvent`；快照在应用线程抓取，仅在 Dirty 时写 `.paperforge/autosave/autosave.paper`，不改变 Clean/Dirty；重开项目时若 autosave 比 project.paper 新则提示恢复 |

## 构建

```bash
# 一次性构建自带 TeX runtime（user-mode，免 root；约 265MB，不进 git）
tools/build-runtime/build_runtime.sh

cmake -B build -G Ninja
cmake --build build
```

要求：CMake ≥ 3.16，C++20 编译器（GCC 12+ 已验证），Qt6（可选——未安装时自动
跳过 GUI，核心库与 CLI 不受影响），poppler-utils（GUI 的 PDF 预览用 `pdftoppm`，
可选），perl + 网络（仅构建 runtime 时需要）。**编译 PDF 不依赖用户机器上的
LaTeX**：PaperForge 用自带的 Portable TeX Live。

## 使用

### 桌面应用（Qt6 GUI）

```bash
./build/src/app/paperforge-gui
```

结构化论文编辑工作台——用户只编辑论文语义，不编辑排版代码：

- **三栏布局**（可拖动）：Outline | Editor | Preview + Problems（18/52/30）
- **作者-机构绑定（图形化）**：Authors 卡片下方是 **Institution links** 面板，
  每个作者一行，点右侧胶囊按钮勾选所属机构（可多选），选择结果实时反映到
  Authors 行的上标编号；也可以直接在 Authors 行输入 `姓名²` 这样的上标，两条
  路径等价
- **拖动重排**：卡片左上角的 `⋮⋮` 手柄可**拖动**，块之间的空隙是放置目标
  （拖动时该缝隙变粗高亮），松手即按落点重排；`⋯` 菜单里的 Move Up / Move
  Down 仍然可用
- **Block 卡片编辑器**：Title / Authors / Institution / Abstract / Keywords /
  Section / Paragraph / Equation 都是卡片，居中限宽 820px
  - **长文本块（摘要/段落/图题表题）自适应高度与宽度**：行高按换行后的真实
    文本高度计算，正文铺满整块宽度并随窗口宽度重新换行
  - **硬换行会自动软化**：从 PDF 复制来的段落是被预先折行成固定列的，单行
    换行在文档模型里没有意义（LaTeX 视其为空格），因此粘贴时、提交时、以及
    打开项目渲染时都会把单换行合并为空格、空行保留为分段；列表、编号、`\\`
    显式换行等有结构的内容原样保留。块菜单里也有 **Reflow Text** 可手动整理
    已有内容（`ReflowHardWrappedText`，核心库纯函数，有单测）
  - 卡片**没有内部滚动条**：超长内容由编辑区整体的一根侧边滚动条承担。
    高度测量要注意两个 Qt 陷阱：
    `QPlainTextDocumentLayout` 的 `documentSize()`（以及 `QTextDocument::size()`）
    返回的是**行数**而不是像素；而按块测量只在块已被布局时才有效，粘贴进来
    的多行文本会量成一行。因此以 `QFontMetrics` + `Qt::TextWordWrap` 测量
    纯文本为准，再与各块高度之和取较大值
  - hover 显示拖拽手柄 + 类型标签 + `⋯` 菜单（Move Up / Down / Delete），
    空间预留不跳动
  - 聚焦块左侧强调线；模板必填但为空的卡片红边高亮
- **块间插入按钮**：每个块上下之间都有固定高度的空隙（始终占位，不跳动），
  鼠标移入时显示一条强调线和圆形 `+`，点击即弹出块类型菜单并在该位置插入；
  **最后一个块之后也有**，所以正文为空时（只有标题/作者/机构/摘要/关键词）
  仍有一个入口用来建立第一个 section，不需要先找到任何块的菜单
- **`/` 斜杠命令**：空行输入 `/` 弹出块创建菜单（Basic/Academic 分组，
  键盘 Up/Down/Enter/Esc + 文本过滤），完全非模态
- **`@` 引用菜单**：输入 `@` 弹出 References / Sections / Figures 分组菜单，
  双击 Outline 的 References Tab 条目也可插入引用
- **Subsection 插到点击处**：subsection 标题插入后，**其下方的块会归入该
  subsection**（文档模型把 section 自身的块排在 subsections 之前，不这样做
  标题只能落到最后）。因此"在这里插入 subsection"就是所见即所得
- **Outline 双 Tab**：Document（摘要 + 章节树，点击滚动定位）/ References
  （文献库，可搜索）
- **结构化 Problems**：severity 分组 + All/Errors/Warnings 过滤 + Build Log Tab
- **Build 按钮**：`Build ▶` → `Building ◌` → `✓ Built`（绿）/ `! Build failed`
  （红），状态栏显示构建阶段与 Revision
- **Welcome 页**：New / Open / 最近项目（QSettings 持久化）
- **状态栏**：✓Saved / ●Unsaved、Build 状态、字数统计
- **自动保存**：30 秒后台保存；崩溃后重开提示恢复
- **PDF 预览（多页连续 + 可缩放）**：构建成功后渲染**每一页**，按阅读顺序纵向
  排列，页与页之间留出可见的缝隙；工具条显示 `p. 当前 / 总数`
  - **惰性渲染**：只为视口附近的页保留位图（超出 ±2 页即释放），因此十几页的
    论文不会因为预览吃掉几百 MB
  - 每页渲染分辨率随缩放变化（96 DPI × 缩放，上限 300 DPI，140ms 去抖）
  - **滚轮缩放**，以光标位置为锚点（指针下的文字不会跑掉）；Shift+滚轮横向
    平移；按住左键/中键拖动平移；工具条有 `−` / 百分比 / `+` / 适宽 / `1:1`
  - 缩放范围 25%–800%；放大后按新分辨率**重新光栅化**（96 DPI × 缩放，
    上限 400 DPI，140ms 去抖），先拉伸位图保证跟手、再换成清晰页面
- **作者 / 机构**：Institution 行支持任意多个机构（换行或 `;` 分隔），
  渲染为编号列表；Authors 行写 `姓名\u00b9` 这样的上标即可绑定机构，
  上标会从姓名中剥离并写入 `Author::affiliations`。机构 id 按位置复用，
  编辑列表不会打断已有绑定；缩短列表时失效的绑定由 `EditingSystem` 清理
- **输入保护（重要）**：内容提交发生在**失焦、Enter、Ctrl+Enter** 时，
  没有"打字停顿就自动提交并刷新"的行为。行重建只在结构变化（增删/移动
  块、章节变更、打开项目、撤销重做）时发生；只要有未提交的输入，重建就
  被推迟，输入文本与光标位置都会保留。窗口关闭时会先尽力提交当前行，
  且禁止在销毁过程中重建（否则析构触发的失焦提交会在半销毁的控件树上
  重建编辑器并崩溃）
- **构建缓存固定**：编译时把 `TECTONIC_CACHE_DIR`/`HOME` 指向项目自带的
  `tools/tectonic-cache`（可用 `PAPERFORGE_TECTONIC_CACHE` 覆盖），
  因此离线构建不依赖 `~/.cache/Tectonic` 是否可写

### 命令行

```bash
./build/src/paperforge new /tmp/mypaper     # 创建示例项目
./build/src/paperforge info /tmp/mypaper    # 查看项目信息
./build/src/paperforge build /tmp/mypaper   # 编译 PDF
./build/src/paperforge demo /tmp/demopaper  # 一键端到端演示
```

生成的项目布局与基线一致：

```
MyPaper/
├── project.paper          # 真正的源数据（JSON）
├── references.bib
├── assets/
└── .paperforge/
    ├── build/
    └── autosave/
```

## 测试

```bash
./build/tests/pf_tests                      # 100 个测试
QT_QPA_PLATFORM=offscreen \
  ./build/src/app/paperforge-inline-editor-test   # +10 个富文本/widget 测试
ctest --test-dir build          # 或通过 CTest（pf_tests + gui_input_persistence）
```

覆盖：Document schema/编辑器约束/Table 矩形性、Editing 协议（stale 拒绝、
undo/redo、revision 单调性、Template 变更语义、**删除恢复型 Undo**）、
Renderer（转义、SourceMap 映射、bib 打包）、持久化（round-trip、原子写、
stale-save 拒绝、**自动保存/崩溃恢复**）、BibTeX 解析/搜索、BuildCoordinator
（latest-wins、诊断映射）、Validator（缺失引用/悬空交叉引用/空标题）、
ProjectSession 工作流、**真实 Tectonic 端到端 PDF 构建**，以及 GUI 控制器
全流程冒烟测试。

**M1 架构场景回归**（`tests/TestSessionAsync.cpp`，把纸面验算变成可执行约束）：
编译中继续输入 / Undo / 切换模板 / 切换项目时旧 build 不得进入预览、
保存期间继续编辑必须仍是 Dirty、自动保存永远写完整 snapshot、
删除被引用的 Figure 后文档仍有效且 Validator 报 dangling 诊断。

**编译工具链回归**（`TestRuntime.cpp`）：runtime 健康检查（可执行文件齐全 +
真实最小编译）、`font-test.tex` 无字体替换、
`IeeeMarksReachPdfLatex`——**IEEEtran 下四种标记组合经真实 pdfLaTeX 构建**
（本 bug 的永久回归），以及 toolchain 随请求传递。

**富文本 widget 回归**（`paperforge-inline-editor-test`，15 个）：run 边界不错位、
斜体整词到达 `\emph{}` 且**没有** `\emph{talic}`、token 重载后不丢、
点工具栏按钮不抢焦点也不膨胀行高、宽度变化自动重排，以及**真实 tectonic 构建**
验证 `\textbf{}`/`\emph{}` 进入 LaTeX 与 PDF；`ToolbarMarksWorkOnEveryTemplate`
对**每个模板**分别断言文档与 LaTeX 都拿到标记（曾出现"默认模板可以、IEEE 不行"
的报告，实测两模板行为一致，差异来自焦点缺陷而非模板）。

**富文本回归**（`tests/TestInline.cpp`）：TextMark 组合往返无损、
Renderer 嵌套 `\textbf{\emph{}}`、富内容经编辑协议与保存/重开保持结构、
行内公式/引用/交叉引用保持语义节点、粘贴重排不丢标记。

**结构语义回归**（`tests/TestStructure.cpp`）：`NodeAddress`/`LocateNode` 对每个
节点给出完整坐标、遍历顺序与文档顺序一致、`Subsubsection` 的
Insert/Rename/Delete/Move/Undo/Redo、块在三级容器间移动后文档仍有效、
Renderer 产出 `\subsubsection` + `\label`、DocumentIndex 覆盖第三层、
Schema V1→V2 迁移幂等且不改动磁盘文件。

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-gui-smoke
```

两个 GUI 回归测试（都可无头运行）：

```bash
# 输入保护、行高自适应、硬换行重排、块间插入、拖动重排、多页预览、
# 预览缩放、作者机构绑定的 86 项检查
QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-gui-persistence-test

# 截图验证：/tmp/pf-ui-workspace.png、/tmp/pf-ui-built.png、/tmp/pf-ui-zoomed.png
QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-ui-verify
```

## V1 范围外（按基线）

Subsubsection、List、Footnote、复杂表格、Subfigure、Word 导入、Zotero 同步、
AI 等均按基线排除。
