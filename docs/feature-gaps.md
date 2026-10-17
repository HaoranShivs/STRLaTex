# 功能缺口与交互改进清单：图形界面 → LaTeX → PDF

> 适用代码：本仓库当前工作树（2026-10 审查清理之后）。
> 目标场景：**科研论文 / 技术文档**——用户只通过图形界面完成写作、引用、图表、
> 公式与交叉引用，系统生成 LaTeX 并编译为 PDF。
> 配套文档：[运行时调用链路](runtime-call-chain.md)、
> [编辑器设计审查](editor-design-review.md)、[审查总报告](code-review-2026-10.md)、
> [接口级实现方案](feature-implementation-design.md)。
>
> 本文只做**产品与交互层面的缺口分析**，不改代码。每一项都给出：
> 现状与代码位置 → 为什么不足 → 建议方案 → 验收方式 → 优先级。
> 文中的“缺失”与“不足”有严格区分，见 §1。具体接口设计见
> [`feature-implementation-design.md`](feature-implementation-design.md)。

---

## 0. 结论摘要（先看这 12 条）

| # | 缺口 | 类型 | 现状一句话 | 优先级 |
| --- | --- | --- | --- | --- |
| 1 | **Citation 选择器信息不足**：没有标题、没有文中引用编号、没有“已引用”标记、不能多选、不能叙述式、点击已插入引用不能重选 | ⚠ 不足 | 条目只有 `FirstAuthor et al. · year`，右列固定写 `Reference` | **P0** |
| 2 | **数学没有图形化入口**：花体/黑板体/希腊字母/结构模板只能手打 LaTeX，无符号面板、无命令补全 | ◐ UI 缺失 | `MathEditorDialog` 只是一个带预览的纯文本框 | **P0** |
| 3 | **图表题注是纯文本**：题注里的公式、引用、粗斜体在编辑时会被压平；表格单元格完全不可编辑 | ◐ 模型支持、UI 缺失 | 题注用 `BlockEdit`（`QPlainTextEdit`）承载，提交时 `InlineFromText` | **P0** |
| 4 | **交叉引用选择器不完整**：只列 Section 和顶层 Figure，缺 Equation/Table/Subsection/Subsubsection；GUI 胶囊显示标题而 PDF 显示编号 | ⚠ 不足 | `MainWindow::RefreshReferenceItems` 手写两层循环 | **P0** |
| 5 | **中文文档没有可用模板**：内置模板都是 pdfLaTeX 且未加载 ctex/xeCJK | ❌ 缺失 | 中文正文进入 pdflatex 会缺字/报错 | **P0（中文用户）** |
| 6 | **矢量图不能插入**：只接受 PNG/JPG/GIF，明确拒绝 PDF/SVG/EPS | ❌ 缺失 | `AssetManager::Stage` 直接返回 `UnsupportedType` | **P1** |
| 7 | **列表、脚注、定理、代码块、子图、附录完全缺失** | ❌ 缺失 | `Block` 只有 Paragraph/Figure/Table/Equation | **P1** |
| 8 | **citation 样式只有数字型**：不支持 author-year、biblatex/Biber；模板契约被硬编码 | ❌ 缺失 | `LatexPlanner` 只接受 `natbib-numeric-first-use-v1` + BibTeX | **P1** |
| 9 | **无法查看/导出生成的 LaTeX**：没有 `.tex` 导出、没有 arXiv 包、没有源码视图 | ❌ 缺失 | 生成结果只在内部 `.paperforge/build` 里（调试转储） | **P1** |
| 10 | **PDF 预览是位图**：不能选文字、搜索、复制、点链接，也不能从 PDF 跳回源码位置 | ⚠ 不足 | `PdfPreview` 用 `pdftoppm` 出 PNG | **P2** |
| 11 | **缺少查找/替换、拼写检查、评论/版本历史、协作** | ❌ 缺失 | 菜单只有 File/Edit(Undo/Redo)/Build | **P2** |
| 12 | **部分文档与代码不一致**：README 的 `@` 菜单已不存在；文档称 citation 可点击重选，但信号无人接收 | ⚠ 不足 | 见 §4.8 | **P1（文档/行为一致性）** |

> 如果只做三件事：**§4.1 Citation Picker v2、§4.2 Math Palette、§4.3/4.4 交叉引用与图表编辑**。
> 这三项决定了“图形界面”是否真的能替代手写 LaTeX。

---

## 1. 判定口径

| 标记 | 含义 | 例子 |
| --- | --- | --- |
| ❌ **缺失** | 文档模型、渲染器、GUI 都没有；用户无法通过任何路径表达该语义 | 列表、脚注、定理、代码块、子图 |
| ◐ **模型已支持、UI 缺失** | LaTeX 管线能生成正确结果，但 GUI 没有入口或入口会丢信息 | 题注中的公式/引用、表格单元格、图片宽度、多 key 引用 |
| ⚠ **已有但不足** | 功能存在，但信息、效率或一致性达不到“好用” | citation 选择器、交叉引用胶囊、PDF 预览 |

这个区分很重要：

- 对 ❌ 项，需要**数据模型 + schema 迁移 + 渲染 + GUI + 测试**的完整工作；
- 对 ◐ 项，多数只差 **GUI 控件 + 提交路径**，性价比最高；
- 对 ⚠ 项，通常只需要**信息补全与交互重排**，最适合先做。

---

## 2. 当前已经打通的闭环（避免重复建设）

先明确哪些能力是真实可用的，后续缺口都以此为基线：

| 能力 | 实现位置 |
| --- | --- |
| 结构化文档：标题/作者/机构/摘要/关键词 + 三级标题 + 段落/公式/图/表 | `document/Document.h` |
| 富文本：粗体、斜体、行内公式、citation、cross-reference | `app/InlineEditor.cpp`、`TextMark` |
| 行内/行间公式：LaTeX 源码 + 异步预览 + 校验 | `math/*`、`app/MathEditorDialog.cpp`、`app/MathPreviewRenderer.cpp` |
| 构建：去抖/latest-wins/取消 + 自带 TeX Live + 诊断映射 | `build/BuildCoordinator.cpp`、`build/Compiler.cpp` |
| 预览：多页、缩放、惰性位图 | `app/PdfPreview.cpp` |
| 参考文献：`.bib` 导入、逐条新增、引用编号、搜索服务 | `bibliography/*`、`numbering/CitationNumberResolver.cpp` |
| 模板：2 个内置 + zip 外部模板导入 + 固定绑定 | `template/*`、`app/TemplateImportDialog.cpp` |
| 持久化：JSON schema v3、原子保存、autosave 恢复 | `persistence/*` |
| 撤销/重做、大纲、Problems、CLI | `editing/*`、`app/OutlinePanel.cpp`、`app/ProblemsPanel.cpp`、`cli/*` |

**重要判断**：很多“看起来缺失”的 LaTeX 能力其实管线是通的，例如
`\mathcal`、`\mathbb`、`\operatorname`、`aligned` 环境等。真正缺的是
**GUI 入口、预览降级质量与信息呈现**。因此不要在没有确认的情况下改动渲染器。

---

## 3. 缺失功能清单（按写作场景）

### 3.1 数学与公式：最需要“图形化”的部分

#### 3.1.1 花体变量（用户明确提到的例子）

LaTeX 本身支持这些字体，`MathValidator` 只拦截危险命令（`MathValidator.cpp` 的
`ForbiddenCommands`/`ForbiddenEnvironments`），并不拦截字体命令。因此：

- **PDF 侧**：`\mathcal`、`\mathbb`、`\mathfrak`、`\boldsymbol` 由 LaTeX 内核 +
  `amsmath`/`amssymb` 提供（内置模板已加载），手打后可以正常编译。
  **但并非所有“字体命令”都如此**：`\mathscr` 需要 `mathrsfs`，`\mathbbm`
  需要 `bbm`，`\mathds` 需要 `dsfont`，`\mathbfit`/`\mathup` 不是标准 LaTeX
  命令；内置模板没有加载这些包，写了会编译失败。
- **GUI 侧**：没有任何按钮、面板、补全或帮助；用户必须记住命令。
- **预览侧**：优先使用真实 TeX（`MathPreviewRenderer::RenderWithTex`），
  因此 runtime 健康时预览是正确的；只有 TeX 不可用/源码无效时才回退到
  `QPainter` 近似渲染。近似渲染的支持范围有限（见下表）。**特别注意**：
  近似回退会“画出”`\mathscr`，但这不代表 PDF 能编译——这是预览与构建
  一致性上的一个坑。

| 字体命令 | 近似预览支持 | 内置模板 PDF | 备注 |
| --- | --- | --- | --- |
| `\mathbf`、`\boldsymbol` | ✅ | ✅ | 粗体/粗斜体 |
| `\mathbfit`、`\mathup` | ✅ | ❌（非标准命令） | 只在回退预览中有意义 |
| `\mathcal`、`\mathfrak` | ✅ 但两者共用同一 script 字体 | ✅ | 近似模式无法区分 |
| `\mathscr` | ✅（与 `\mathcal` 同一字体） | ❌（需 `mathrsfs`） | 预览可能“看起来正常” |
| `\mathrm`、`\mathit`、`\mathsf`、`\mathtt` | ✅ | ✅ | 直立/斜体/无衬线/等宽 |
| `\text`、`\operatorname`、`\textrm`、`\textnormal`、`\mbox` | ✅ | ✅ | 文本参数 |
| `\mathbb`、`\mathds`、`\mathbbm` | ❌ | `\mathbb` ✅；`\mathds`/`\mathbbm` ❌ | 近似模式会显示字面命令 |
| `\mathnormal`、`\mathbfcal`、`\mathbfrak`、`\pmb` 等 | ❌ | 取决于包 | 需要明确依赖 |

**结论：花体变量不是“LaTeX 不支持”，而是“GUI 没有给入口、部分命令需要额外
宏包、且离线回退时 `\mathbb` 等不可用”。** 如果只做按钮而不做“命令 → 所需
宏包”的映射，用户会在预览正常、PDF 失败之间来回踩坑。

#### 3.1.2 其他数学缺口

| ID | 缺口 | 现状/证据 | 影响 | 建议 |
| --- | --- | --- | --- | --- |
| M-01 | 无符号面板 | `MathEditorDialog` 只有 `QPlainTextEdit` + `QLabel` 预览 | 用户必须背 LaTeX 命令 | 按类别做符号面板（希腊/运算/关系/箭头/集合/字体/结构） |
| M-02 | 无命令补全 | 没有 `QCompleter`，没有 `\` 触发提示 | 输入慢、易拼错 | 输入 `\` 弹出命令列表，带说明与示例 |
| M-03 | 无结构编辑 | 分式/根号/上下标/矩阵/分段函数只能手写 | “图形化”名不副实 | 结构按钮：点击插入模板；选中内容时包裹 |
| M-04 | 无宏/自定义命令 | `MathValidator` 明确禁止 `\newcommand`、`\DeclareMathOperator` | 技术文档无法定义 `\R`、`\E` 等简写 | 项目级“数学宏”表，由渲染器注入导言区 |
| M-05 | 无 `\tag` / 自定义编号 | `\label`、`\tag`、`\nonumber` 被禁止；编号只有 `EquationBlock::numbered` | 无法写 `(3a)`、`(A.1)`、按章节编号 | 后续：编号策略（全局/按节）+ 自定义 tag |
| M-06 | 公式编号在编辑器中不可见 | `MakeEquationCard` 只有 Numbered 复选框与 Label 输入框 | 用户看不到“这是第几个公式” | 在卡片上显示 `(n)`，并与交叉引用共用编号模型 |
| M-07 | Label 无校验/无重复检测 | 用户输入经 `Label()` 静默清洗；重复 label 不报错 | `\ref` 可能指向错误公式 | 保存/构建前校验格式与唯一性，给出可定位诊断 |
| M-08 | 无单位/量纲支持 | 模板未加载 `siunitx`；无 `\SI`/`\qty` 入口 | 技术文档中的数值单位排版不可控 | 可选包 + 数值/单位结构化输入（低优先级可先允许 `\SI`） |
| M-09 | 无化学/算法/代码公式 | 无 `mhchem`、`algorithm2e`、`listings` 支持 | 化学、算法论文写不了 | 见 §3.4 |
| M-10 | 预览回退静默降级 | 行内公式忽略 `MathRenderResult.note`；公式卡片也不显示 note（`InlineEditor::ApplyMathRender`、`MakeEquationCard`） | 用户看到字面命令，不知道为什么 | 在公式对象 tooltip/状态行显示“近似预览”与原因 |
| M-11 | 近似预览覆盖不足 | `MathPreviewRenderer` 缺少 `\binom`、`\boxed`、`\overbrace`/`\underbrace`、`\overset`/`\underset`、`\substack`、`\overrightarrow`、`\begin{array}`、`\begin{split}`、`\pmod` 等 | 离线/降级时显示字面命令 | 补齐常用命令；无法支持时明确显示错误 |
| M-12 | 无数学源码高亮 | `MathEditorDialog` 与公式卡片都是纯 `QPlainTextEdit`；`QSyntaxHighlighter` 在 `BlockEditor.cpp` 里只被 include | 长公式难读 | 复用/新增 LaTeX math highlighter |
| M-13 | 中文数学文本不可用 | 内置模板 pdfLaTeX，未加载 ctex/xeCJK；`\text{中文}` 在 pdflatex 下会失败 | 中文论文中的公式说明写不了 | 与 §3.7 中文模板一起解决 |
| M-14 | 无公式“所见即所得”拖拽/框选 | 只能编辑源码 | 复杂公式门槛高 | 中期：结构树/框选编辑（工作量大，不建议先做） |

#### 3.1.3 建议的 Math Palette v1（最小可用）

```
┌ Inline Math ─────────────────────────────────────────────────────┐
│ [结构]  a/b   √x   x²   xₙ   ∑   ∫   matrix   cases   aligned     │
│ [字体]  𝒜\mathcal  ℝ\mathbb  𝔤\mathfrak  ℱ\mathscr  𝐱\mathbf …  │
│ [符号]  α β γ δ …  ≤ ≥ ≠ ≈ …  → ⇒ ↔ …  ∈ ⊂ ∪ ∩ …  ∞ ∂ ∇ …        │
│                                                                  │
│  \mathbb{R}^{n}                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │                    （实时预览，已有异步服务）               │  │
│  └────────────────────────────────────────────────────────────┘  │
│  Valid                                     [Cancel] [Insert]     │
└──────────────────────────────────────────────────────────────────┘
```

交互规则：

1. **有选区**：点击结构/字体按钮时包裹选区（如选中 `x+1` 点 `a/b` → `\frac{x+1}{}`）；
2. **无选区**：在光标处插入模板，并把光标放到第一个参数内；
3. 按钮悬停显示命令名 + 简短说明（例如 `\mathbb`：双线体，用于实数集/期望）；
4. 命令补全列表同时支持键盘上下与 Esc；
5. 所有插入都走现有 `MathEditorDialog::latex()` 返回值，**不改文档协议**。

---

### 3.2 引用与参考文献

（最详细的交互设计见 §4.1，这里先列功能面。）

| ID | 缺口 | 现状/证据 | 建议 |
| --- | --- | --- | --- |
| C-01 | 条目无标题 | `MainWindow::RefreshReferenceItems` 只拼 `first author + year`，`detail` 固定 `Reference` | 行内显示标题（截断）、venue、年份、key |
| C-02 | 无文中引用编号 | `CitationNumberResolver` 已存在且 `BlockEditor` 持有 `citation_numbers_`，但选择器不使用 | 已引用的 key 显示 `[n]` |
| C-03 | 无“已引用”状态 | 同上 | 分组“本文已引用 / 未引用”，或行尾 ✓ |
| C-04 | 搜索不含标题 | picker 只用 `item.search = item.label`；而 `BibliographyService::Search` 本来就搜 title/venue/authors/year/key | 构建 `Item.search` 时拼接全部字段 |
| C-05 | 不能多选 | `PopupList` 只发 `chosen(QString)`，一次一项；`InsertCitationObject` 本身支持多 key | 多选 + 一次插入 `\citep{a,b}` |
| C-06 | 无叙述式引用 | 模型有 `CitationMode::Narrative`，渲染器用 `\citet`；GUI 从不产生，token 不保存 mode，且胶囊始终按数字格式显示 | 选择器加 Narrative 开关；token 保存 mode；胶囊按 mode 显示（数字样式：`Author [n]`） |
| C-07 | 点击已插入引用不能重选 | `InlineEditor` 发 `CitationTokenActivated`，但全仓无 `connect`（见 §4.8） | 连接信号，打开 picker 并替换 token |
| C-08 | 无 locator/prefix/suffix | 数据模型 `Citation` 只有 keys + mode，没有 `\citep[see][p.3]{}` 信息 | 中期：加入可选 locator/prefix/suffix（schema 变更） |
| C-09 | 无文献库编辑/删除 | `AddReferenceDialog` 只追加；导入 `.bib` 会整体替换 `bibliography_db_` | 编辑/删除单条 + 导入时合并/替换可选 |
| C-10 | 无 DOI/URL/摘要显示 | `BibEntry.fields` 有，但 picker 不展示 | 详情展开区（hover/右侧预览） |
| C-11 | 无重复检测 | 只检测重复 key；同名/同 DOI 不检测 | 导入时按 DOI/title 提示疑似重复 |
| C-12 | 无作者-年份样式 | `LatexPlanner` 硬性要求 `natbib-numeric-first-use-v1` + BibTeX | 模板契约扩展 + natbib author-year / biblatex |
| C-13 | 无 Zotero/DOI/arXiv 导入 | 只能粘贴 BibTeX 或导入 .bib | 中期集成 |
| C-14 | 无“引用出现位置”反查 | 只有编号，没有“第 3 节引用过” | 文献详情显示引用位置列表 |

---

### 3.3 交叉引用与编号

| ID | 缺口 | 现状/证据 | 建议 |
| --- | --- | --- | --- |
| X-01 | 引用选择器类型不全 | `MainWindow::RefreshReferenceItems` 只列 `Section` 与**顶层 section.blocks 里的 Figure**；没有 Subsection/Subsubsection/Equation/Table，也漏掉 subsection 里的 Figure | 用 `DocumentTraversal` 遍历所有节点，按类型分组 |
| X-02 | 不显示编号 | 条目 label 是标题/题注文字 | 显示 `Section 2`、`Figure 3`、`Eq. (4)`、`Table 1` |
| X-03 | 胶囊与 PDF 不一致 | `CrossReferenceDisplayText` 用 `xref_labels_`（标题/题注），渲染器用 `\ref`（编号） | 胶囊显示编号，与 PDF 一致；或 PDF 用 `\autoref`/`\cref` |
| X-04 | 只能 `\ref` | `LatexPlanner` 固定 `Cmd("ref", ...)` | 引入 `cleveref` 或按目标类型生成 `\autoref` |
| X-05 | 图表没有用户 label | Figure/Table 的 label 固定为 node id；Equation 有 label 字段 | Figure/Table 增加 label 字段并校验唯一性 |
| X-06 | 空题注表格没有 label | `LatexPlanner` 只有 caption 非空才写 `\label`，但交叉引用仍可能指向它 | 表格总是写 label（或禁止引用无题注表） |
| X-07 | 点击交叉引用不能重选 | `CrossReferenceTokenActivated` 无人接收 | 与 C-07 同一机制 |
| X-08 | 无页码引用/多目标引用 | 只支持单节点 | 中期 |

---

### 3.4 文档结构：论文/技术文档必需的块类型

当前 `Block` 只有四种：

```cpp
using Block = std::variant<Paragraph, Figure, Table, EquationBlock>;
```

这意味着以下结构**根本不存在**，用户无法通过 GUI 或模型表达：

| ID | 缺失结构 | 说明 | LaTeX 生态 |
| --- | --- | --- | --- |
| S-01 | 无序/有序/描述列表 | 无法生成 `itemize`/`enumerate`/`description`；`MathValidator` 也禁止这些环境（因为它们不属于公式） | 标准 LaTeX |
| S-02 | 脚注 | 没有行内 `Footnote` 节点；正文无法插入脚注 | `\footnote` |
| S-03 | 定理/引理/定义/证明 | 没有块类型；无法编号、无法交叉引用 | `amsthm` |
| S-04 | 代码/verbatim | 没有代码块；`verbatim`、`lstlisting`、`minted` 都被 validator 禁止 | `listings`/`minted` |
| S-05 | 算法/伪代码 | 没有算法块 | [algorithm2e](https://ctan.org/pkg/algorithm2e) |
| S-06 | 子图/图组 | `Figure` 只持有单个 `AssetId`；没有 subfigure 模型 | [subcaption](https://ctan.org/pkg/subcaption) |
| S-07 | 附录 | `BackMatter` 只有 `bibliography_enabled` | `\appendix` |
| S-08 | 致谢/作者贡献/利益冲突/数据可用性 | 无对应字段或块 | 期刊常见要求 |
| S-09 | 目录/图目录/表目录 | 渲染器不生成 `\tableofcontents`/`\listoffigures`/`\listoftables` | 标准 LaTeX |
| S-10 | 引用块/摘要外结构 | 无 quote/epigraph/highlights/graphical abstract | 期刊模板 |
| S-11 | 术语表/符号表/索引 | 无 | `glossaries`/`makeidx` |
| S-12 | 原始 LaTeX 块（逃生口） | 没有任何“自定义 LaTeX”入口；TikZ 等无法使用 | `tikzpicture` 等 |

> 说明：S-01/S-02/S-03/S-04 属于“科研写作刚需”，建议优先于 S-07~S-11。
> S-12 是双刃剑：它能让高级用户突破模型限制，但会破坏“结构化”与可移植性，
> 应作为可选的高级功能并明确标注风险。

---

### 3.5 图与表

| ID | 缺口 | 现状/证据 | 建议 |
| --- | --- | --- | --- |
| F-01 | 只支持位图 | `AssetManager::DetectMediaType` 识别 PDF/SVG/EPS，但 `Stage` 明确拒绝，提示 “V1 only accepts bitmap images” | 支持 PDF（pdfLaTeX 原生）、SVG（转 PDF/PNG 或 `\includesvg`）、EPS（epstopdf） |
| F-02 | 不能拖拽/粘贴图片 | `BlockEditor` 的 drag/drop 只处理块重排 MIME，没有 `hasUrls`/`image/*`；插入图只走 `QFileDialog` | 拖入文件/粘贴剪贴板图片 → `AssetManager::Stage` → `InsertFigure` |
| F-03 | 不能替换/裁剪图片 | Figure 卡片只显示图片；删除再插是唯一路径 | 右键“Replace image”、裁剪/缩放 |
| F-04 | 图片宽度不可调 | 模型有 `FigureWidth`（25/50/75/100%），但 GUI 只暴露双栏开关；插入固定 100% | 下拉/滑块设置宽度 |
| F-05 | 没有图片浮动位置控制 | 渲染器固定 `htbp` | 选择 `htbp`/`H`/`t`/`b` 等 |
| F-06 | 题注不能富文本 | `MakeFigureCard` 用 `BlockEdit` 编辑题注，提交走 `InlineFromText`；题注中的公式/引用/粗体会被压平 | 题注改用 `InlineEditor`（模型与渲染器已支持 `InlineContent`） |
| F-07 | 题注无编号显示 | 卡片只显示题注文字 | 显示 “Figure 1” 并纳入编号模型 |
| F-08 | 表格不可编辑 | `QTableWidget` 设置 `NoEditTriggers`；只能看 | 单元格编辑复用行内编辑器；行列操作走 `DocumentEditor` 已有 API |
| F-09 | 表格尺寸固定 3×3、无对话框 | `ProjectController::InsertTableAfter` 固定 3 列 3 行 | 插入时选行列/表头；支持增删行列 |
| F-10 | 表格无对齐/合并/多行多列 | 模型只有 `ColumnAlignment`；无 `\multicolumn`/`\multirow` | 先做列对齐 UI，再做合并单元格（schema 升级） |
| F-11 | 表格单元格不能富文本 | `QTableWidgetItem` 用 `InlineToPlainText`；模型 `TableCell::content` 是 `InlineContent` | 编辑时用 InlineEditor，渲染器已支持 |
| F-12 | 无图/表 label | 使用 node id；用户不可读 | 增加 label 字段 |
| F-13 | 无图/表目录 | 见 S-09 | — |
| F-14 | alt text 不可编辑/不输出 | `Figure::alt_text` 持久化但无 UI；渲染器不输出任何 alt（PDF 标签） | 编辑器加 alt 字段；PDF/UA 需要 tagged PDF，属长期项 |

---

### 3.6 富文本与行内元素

`TextMark` 只有两位：

```cpp
enum class TextMark { None = 0, Strong = 1 << 0, Emphasis = 1 << 1 };
```

| ID | 缺失 | 影响 | 建议 |
| --- | --- | --- | --- |
| R-01 | 下划线 | 技术文档常用 | 新增 `Underline`（schema/渲染/编辑器） |
| R-02 | 上标/下标 | 化学式、单位、脚注标记 | 新增 `Superscript`/`Subscript`（注意与作者机构上标的文本 hack 解耦） |
| R-03 | 小型大写/删除线 | 期刊格式 | 可选 |
| R-04 | 行内代码/等宽 | API 名、文件路径 | `\texttt{}` + 新 mark |
| R-05 | 超链接 | DOI/URL/项目主页 | `\href{}`；需要 link 节点 |
| R-06 | 脚注引用 | 见 S-02 | `\footnote{}` |
| R-07 | 题注/标题富文本 | 见 F-06；标题行也是纯文本 `BlockEdit` | 统一改用 `InlineEditor` |
| R-08 | 作者机构上标上限 5 | `BlockEditor.cpp` 里 `const char* supers[] = {"¹"…"⁵"}`，超过 5 个重复 `⁵` | 支持任意位数（¹²³⁴⁵⁶⁷⁸⁹⁰ 组合），与解析器的 10 个机构能力对齐 |

---

### 3.7 模板、排版与导出

| ID | 缺口 | 现状/证据 | 建议 |
| --- | --- | --- | --- |
| T-01 | 无中文模板 | 内置 `generic-article`/`ieee-conference` 都是 pdfLaTeX，未加载 ctex/xeCJK；GUI 字体支持 CJK，但 PDF 不支持 | 新增 `chinese-article`/`chinese-thesis` 模板，使用 [ctex](https://ctan.org/pkg/ctex) + XeLaTeX/LuaLaTeX（或 ctex 的 pdfLaTeX 方案）并配置中文字体 |
| T-02 | 模板数量少、无画廊 | 只有 2 个内置；外部 zip 导入是唯一扩展 | 内置 ACM/Elsevier/Springer/ CVPR 等常用模板；模板预览与一键新建 |
| T-03 | 不能选包 | 包列表由模板固定 | 模板能力声明 + 可选包（谨慎：影响可复现性） |
| T-04 | 不能选 citation 样式 | 见 C-12 | author-year/数字样式选择；或模板声明 |
| T-05 | 不能选页面/字体/行距 | 模板固定 `geometry margin=1in`、字号等 | “文档设置”面板，参数写入项目 |
| T-06 | 不能导出 `.tex` | 生成的 `main.tex` 只存在于 `.paperforge/build`（调试转储） | “导出 LaTeX 源码/arXiv 包”（含 assets、references.bib、main.tex） |
| T-07 | 不能导出/另存 PDF | `project.pdf` 自动生成，但无 Save As/打印 | 导出 PDF；打印 |
| T-08 | 不能导入 `.tex`/Word/Markdown | 只能新建结构化项目或导入模板 | 中期：LaTeX 导入（解析器）；Word 属长期 |
| T-09 | 生成的 LaTeX 不可见 | 用户无法检查/微调 | “查看生成的 LaTeX”只读视图（可复制） |
| T-10 | 无编译引擎选择 | 模板决定引擎；内置都是 pdfLaTeX | 高级设置中允许覆盖（带警告） |
| T-11 | PDF 元数据未设置 | 模板只加载 `hyperref` 的 `hidelinks`；没有 `pdftitle`/`pdfauthor`/`pdfkeywords` | 从 FrontMatter 写入 PDF 元数据（标题/作者/关键词/主题） |

---

### 3.8 写作工作流

| ID | 缺失 | 说明 |
| --- | --- | --- |
| W-01 | 查找/替换 | 菜单只有 Undo/Redo；无跨块查找 |
| W-02 | 拼写/语法检查 | 无 |
| W-03 | 评论/建议/审阅 | 无 |
| W-04 | 版本历史/命名版本 | 只有会话内 undo + autosave；无历史快照 |
| W-05 | 协作/云同步 | 无 |
| W-06 | 只读/锁定 | 无 |
| W-07 | 字数统计对中文失真 | `MainWindow::CountWords` 用 `\s+` 切分；中文无空格 → 近似 1 |
| W-08 | PDF 文本层/搜索/复制/链接 | `PdfPreview` 用 `pdftoppm` 生成 PNG，无文本层 |
| W-09 | PDF ↔ 源码双向定位 | SourceMap 存在，但只用于编译器错误；预览点击不能跳回块 |
| W-10 | 设置/偏好 | 无自动保存间隔、字体、主题、快捷键设置 |
| W-11 | 深色主题 | 无 |
| W-12 | 可访问性 | 无 `accessibleName`；自定义卡片/浮动菜单键盘可达性弱 |
| W-13 | 国际化 | 0 处 `tr()`；UI 字符串英文硬编码 |

---

## 4. 现有功能不足（逐项拆解）

### 4.1 Citation 选择器：当前实现与目标设计（重点）

#### 4.1.1 当前是怎么工作的

```text
MainWindow::RefreshReferenceItems()                  （每次 documentChanged）
  for entry in bibliography:
      label  = "FirstAuthor et al. · 2020"           ← 没有 title/venue/key
      detail = "Reference"
      search = label.toLower()                       ← 标题不可搜
      payload= "cite:" + key
  editor_->SetReferenceItems(items)

BlockEditor::ShowCitationPicker(editor)              （工具栏 Citation / 选区菜单）
  for item in reference_items_ where payload starts with "cite:":
      label  = item.label
      detail = item.detail
      group  = "Citations"
      payload= key
      search = label.toLower()                       ← 仍然只有作者/年份
  popup->popup(...)

用户点一行 → chosen(key) → InlineEditor::InsertCitationObject({key})
  → 生成 TokenKind::Citation，payload = key
  → CommitInlineRow → EditParagraphRich
```

相关位置：

- 条目构建：`app/MainWindow.cpp::RefreshReferenceItems`
- 选择器：`app/BlockEditor.cpp::ShowCitationPicker`
- 通用弹层：`app/PopupList.{h,cpp}`（固定 340px、两列、行高一致、只支持单选）
- 插入：`app/InlineEditor.cpp::InsertCitationObject`
- 编号：`numbering/CitationNumberResolver.cpp`（`Find` / `numbers()`）
- 数据：`bibliography/BibliographyService.h` 的 `BibEntry{key, entry_type, title, authors, year, venue, fields}`

**当前弹层实际长这样：**

```text
┌ Type to filter… ─────────────────────────────┐
│ CITATIONS                                     │
│ Vaswani et al. · 2017            Reference    │
│ Smith et al. · 2020              Reference    │
│ ...                                           │
└───────────────────────────────────────────────┘
```

问题很直接：**同一作者同一年有多篇时无法区分；不知道哪些已经引用过；
不知道自己会得到 `[3]` 还是 `[7]`；不能一次选多篇；不能选择叙述式。**

#### 4.1.2 问题清单（按影响排序）

| # | 问题 | 后果 | 代码证据 |
| --- | --- | --- | --- |
| P-1 | 行内没有论文标题 | 无法区分 `Smith 2020` 的多篇论文；只能靠记忆或去 Outline 查 | `RefreshReferenceItems` 只拼 author/year |
| P-2 | 没有 venue/年份独立列 | 会议/期刊信息缺失 | 同上；`BibEntry.venue` 未使用 |
| P-3 | 没有文中引用编号 | 用户不知道插入后会显示 `[n]`，也无法判断是否已引用 | `CitationNumberResolver` 存在但 picker 不读 |
| P-4 | 没有“已引用”标记/分组 | 重复引用与漏引都难发现 | 同上 |
| P-5 | 搜索不含标题/venue/key | 记得标题但记不住作者时搜不到 | `item.search = item.label` |
| P-6 | 只能单选 | 无法生成 `\citep{a,b}`；分两次插入在 PDF 中会变成 `[1][2]` 而不是 `[1,2]` | `PopupList::chosen(QString)` 每次一项 |
| P-7 | 不能插入叙述式，且胶囊不区分模式 | 模型/渲染器支持 `\citet`，但 GUI 永远 `\citep`；即使文档里有 Narrative，胶囊也仍显示 `[n]` | `InsertCitationObject` 不接收 mode；`CitationDisplayText` 忽略 mode；`src/app` 里没有 `Narrative` |
| P-8 | 点击已插入的 citation 不能重选 | 选错 key 只能删掉重插 | `CitationTokenActivated` 全仓无 connect |
| P-9 | 编辑含叙述式引用的段落会**丢失 mode** | 从 `project.paper` 读入的 `Narrative` 在 GUI 提交后变成 `Parenthetical` | `InlineEditor::ContentInRange` 构造 `Citation` 时没有设置 mode |
| P-10 | 无 locator/prefix/suffix | 无法写 “see @smith2020, p. 3” | `Citation` 只有 keys + mode |
| P-11 | 无文献条目编辑/删除 | 写错只能改 `.bib` 或重新导入 | `AddReferenceDialog` 只有 Add |
| P-12 | 无格式预览 | 不知道最终是 `[1]` 还是 `(Smith, 2020)` | 无样式选择 |
| P-13 | 无重复/同名检测 | 导入时只报重复 key | `BibliographyService` 仅检查 key |
| P-14 | 无键盘快捷入口 | 浮动工具栏只在有选区时出现；右键菜单里虽可点到 Citation，但没有 Ctrl+Shift+C 之类的直达快捷键 | `BlockEditor::ConfigureFormatActions` 未设 shortcut；`InlineEditor::UpdateFormattingToolbar` 要求 `hasSelection()` |

#### 4.1.3 目标设计（Citation Picker v2）

**布局（建议宽度 560–640px，两行式行高）：**

```text
┌ Add citation ──────────────────────────────────────────────────────────────┐
│ Search: attention                          [ ] Narrative (Author [n]) │
│                                                                            │
│ CITED IN THIS PAPER                                                        │
│ ☑ [3]  Vaswani et al. · 2017 · Attention Is All You Need · NeurIPS        │
│ ☐ [7]  Smith & Lee · 2020 · Efficient Attention Mechanisms · ICML         │
│ NOT CITED                                                                  │
│ ☐ [ ]  Brown et al. · 2020 · Language Models are Few-Shot Learners · NeurIPS │
│ ☐ [ ]  Doe et al. · 2021 · Sparse Attention Revisited · ACL               │
│                                                                            │
│ Selected: 1 → will render as [3]                       [Cancel] [Insert]   │
└────────────────────────────────────────────────────────────────────────────┘
```

**行为规格：**

1. **搜索**：匹配 title / 所有作者 / year / venue / key；`Smith 2020`、`attention`、
   `vaswani` 都能命中；大小写不敏感；搜索结果保持分组。
2. **行信息**：`[n]`（已引用）或 `[ ]`（未引用）+ 作者（第一位 + et al.）+
   年份 + 标题（单行省略）+ venue + 类型徽标（Article/Conference）。
3. **已引用分组**：带编号的条目放“CITED IN THIS PAPER”，其余放“NOT CITED”；
   这直接来自 `CitationNumberResolver::numbers()`。
4. **多选**：复选框/Ctrl+点击；底部实时显示“将生成 `[1, 3]`”的预览；
   插入时调用 `InsertCitationObject(keys)`（后端已支持 key 列表）。
5. **Narrative 开关**：勾选后插入 `CitationMode::Narrative`。在当前数字引用样式
   （`numbers` + `unsrtnat`/`IEEEtranN`）下，natbib 的 `\citet` 输出
   `Author [n]`，因此胶囊/预览也应显示 `Author [n]`；等 author-year 样式落地
   （C-12）后才是 `Author (Year)`。这要求显示层知道第一作者名，见 §4.1.4。
6. **重编辑**：点击正文中已有的 citation 胶囊 → 打开同一选择器，预选当前 keys
   与 mode；确认后替换 token，而不是插入新 token。
7. **键盘**：↑/↓ 移动、Enter 选中/插入、Esc 关闭、Ctrl+Enter 立即插入；
   输入即过滤。
8. **空状态**：没有文献时提示“导入 .bib / Add Reference…”，并给出动作按钮
   （不是只显示一行灰字）。

#### 4.1.4 实现拆解（按改动面）

| 层 | 改动 | 工作量 |
| --- | --- | --- |
| `PopupList::Item` | 增加 `title`、`meta`、`badge`、`value`、`checked`、`search_extra`；或改为 `QStyledItemDelegate` 画两行 | S |
| `PopupList` | 多选模式（`Qt::ItemIsUserCheckable` / Ctrl 点击）、`chosenMany(QStringList)`、保持打开直到 Insert、行高不再 `uniform` | M |
| `MainWindow::RefreshReferenceItems` | 用 `BibEntry` 的 title/venue/year/entry_type 构建 `label`/`meta`/`search`；不要再把 detail 固定成 `Reference` | S |
| `BlockEditor::ShowCitationPicker` | 读取 `citation_numbers_`，为每个 key 加上 `[n]`/未引用；分组；Narrative 选项；多选；把 key 列表传给 `InsertCitationObject(keys, mode)` | M |
| `InlineEditor` | `InsertCitationObject(keys, mode)`；token 的 payload 或 char format 保存 mode；`ContentInRange` 恢复 mode；`InlineToRichText`/`InlineFromRichText` 编码 mode；新增 `ReplaceCitationAt(position, keys, mode)` | M |
| `CitationNumberResolver`（或新增 `CitationDisplayFormatter`） | 目前只保存 `key -> number`，无法显示叙述式的作者名。需要携带每篇文献的第一作者（构建时来自 `BibEntry.authors`），并让 `FormatPill(keys, mode)` 在 Narrative 下输出 `Author [n]`；author-year 样式落地后再输出 `Author (Year)` | S–M |
| `OutlinePanel` References 列表 | 行文本加标题（tooltip 已有 title，但搜索不含 title）；双击仍插入单条 | S |
| 测试 | picker 显示标题/编号/已引分组；多选生成一个 Citation；Narrative 经过 commit/save/reopen/undo 不变；点击胶囊重选 | M |

**关键设计决定**：mode 必须进入 token 的身份。现在 token 的 payload 只有
`key,key2`，而 `ContentInRange` 按默认 Parenthetical 重建；这是 P-9 数据丢失的
根因。建议：

```text
payload 形式 A： "parenthetical|key1,key2" / "narrative|key1,key2"
payload 形式 B： 保持 payload=keys，另用 char format property 存 mode
```

形式 A 更简单，且与现有 `InlineToRichText` 的文本编码兼容；但需要同时更新
`InsertPillObject`、`ContentInRange`、`InlineToRichText`/`InlineFromRichText`、
以及 `SanitizeTokens` 的识别逻辑。推荐 A。

#### 4.1.5 验收（可执行）

- 新建项目、导入含 3 篇文献的 `.bib`；先插入第 1 篇；
  打开 picker：第 1 篇在 “CITED” 组并显示 `[1]`，其余在 “NOT CITED” 且无编号；
- 搜索 `attention` 能命中标题含该词的条目；
- 勾选第 1、3 篇 → 插入 → 文档中只有一个 citation 胶囊 `[1, 3]`；
  PDF 的 `\citep{key1,key3}` 输出 `[1, 3]`；
- 勾选 Narrative → 插入 → 胶囊与 PDF 都显示 `Author [n]`（数字样式）；
  保存、重开、Undo/Redo 后 mode 不变；
- 点击已插入 citation → picker 预选当前条目；改选后替换成功；
- 搜索标题中的词能命中；搜索 key 能命中。

---

### 4.2 数学编辑器与“花体变量”：现状、缺口与面板设计

#### 4.2.1 现状

- 入口：Text 行工具栏 **Inline Math**（`InlineEditor::BeginInlineMath`）与
  公式块卡片（`BlockEditor::MakeEquationCard`）。
- 编辑器：`MathEditorDialog` = `QPlainTextEdit`（源码）+ `QLabel`（异步预览）+
  状态行；公式卡片 = `BlockEdit`（源码）+ `QLabel`（预览）+ Numbered + Label。
- 校验：`MathValidator` 按黑名单拒绝文档级命令/外层环境/定界符，
  数学结构合法。
- 预览：`MathRenderService` 在 worker 线程用真实 TeX；失败/离线时回退到
  `QPainter` 近似渲染。
- 结果：**用户必须会写 LaTeX**；没有面板、没有补全、没有结构模板。

#### 4.2.2 为什么这是“图形化”的核心缺口

数学是论文中唯一仍然要求用户直接写 LaTeX 的地方。对大多数作者：

- 不知道 `\mathcal` 与 `\mathbb` 的区别；
- 不知道矩阵/分段函数的环境名；
- 不记得希腊字母/箭头的命令；
- 写错一个花括号就要看编译错误。

因此，“GUI → LaTeX → PDF”的体验在数学处断掉了：**用户又回到了手写 LaTeX**。

#### 4.2.3 Math Palette v1（最小实现）

见 §3.1.3 的线框图。实现上建议复用现有对话框，不新建窗口：

1. 在 `MathEditorDialog` 的源码框上方加两行 `QToolBar`/`QGridLayout`：
   - 结构按钮：`\frac{}{}`, `\sqrt{}`, `^{}`, `_{}`, `\sum_{}^{}`, `\int_{}^{}`,
     `\begin{pmatrix}...\end{pmatrix}`, `\begin{cases}...\end{cases}`,
     `\begin{aligned}...\end{aligned}`；
   - 字体按钮：`\mathcal{}`, `\mathbb{}`, `\mathfrak{}`, `\mathscr{}`,
     `\mathbf{}`, `\mathrm{}`, `\mathit{}`, `\mathsf{}`, `\mathtt{}`,
     `\boldsymbol{}`。
2. 每个按钮的 `clicked` 读取 `QPlainTextEdit::textCursor()`：
   - `hasSelection()` → 用命令包裹选中文本；
   - 否则插入 `\cmd{}` 并把光标放到 `{}` 内。
3. 加一个符号面板（`QTabWidget` 或 `QToolBar` + `QMenu`）：
   希腊字母、二元运算、关系、箭头、集合/逻辑、大型运算符、杂项。
4. 加 `QCompleter`：源码框输入 `\` 时弹出命令列表，带命令说明；
   `QCompleter` 的 model 可以从一张静态表生成（命令 → 说明/示例）。
5. 预览：沿用 `MathRenderService`；对近似回退补上 `\mathbb`、`\binom`、
   `\boxed`、`\overbrace`/`\underbrace`、`\overset`/`\underset`、
   `\substack`、`\overrightarrow`、`\begin{array}`、`\begin{split}`。
6. 公式卡片也复用同一套工具栏（抽成 `MathPaletteWidget`），
   避免两套实现。

#### 4.2.4 命令/符号清单建议

| 组 | 内容 |
| --- | --- |
| 结构 | `\frac`、`\dfrac`、`\tfrac`、`\sqrt`、`^`、`_`、`\sum`、`\prod`、`\int`、`\oint`、`\lim`、`\begin{matrix}`/`pmatrix`/`bmatrix`/`cases`/`aligned`/`array` |
| 字体 | `\mathcal`、`\mathbb`、`\mathfrak`、`\mathscr`、`\mathbf`、`\boldsymbol`、`\mathrm`、`\mathit`、`\mathsf`、`\mathtt`、`\operatorname` |
| 希腊 | α–ω、Γ–Ω、variants（`\varepsilon`、`\varphi`、`\vartheta`） |
| 运算 | `\pm`、`\mp`、`\times`、`\div`、`\cdot`、`\ast`、`\star`、`\circ`、`\oplus`、`\otimes` |
| 关系 | `\leq`、`\geq`、`\neq`、`\approx`、`\equiv`、`\propto`、`\sim`、`\simeq`、`\cong` |
| 箭头 | `\to`、`\rightarrow`、`\leftarrow`、`\Rightarrow`、`\Leftarrow`、`\leftrightarrow`、`\mapsto` |
| 集合/逻辑 | `\in`、`\notin`、`\subset`、`\supset`、`\subseteq`、`\cup`、`\cap`、`\forall`、`\exists`、`\emptyset` |
| 大型/杂项 | `\sum`、`\prod`、`\int`、`\oint`、`\infty`、`\partial`、`\nabla`、`\ell`、`\hbar`、`\Re`、`\Im` |

#### 4.2.5 花体变量专项验收

- 在 Text 行插入行内公式，点击 `\mathbb` 按钮 → 生成 `\mathbb{}`，
  光标在花括号内；输入 `R` → 预览为双线 R；
- 选中 `x+1` 点击 `\mathcal` → 生成 `\mathcal{x+1}`；
- `\mathbb{R}`、`\mathfrak{g}`、`\mathcal{L}` 在真实 TeX 预览/PDF 中显示正确；
  `\mathscr` 需要先给模板加 `mathrsfs`（见 §3.1.1）；
- 断开 TeX（或强制 `ApproximateOnly`）时，`\mathbb` 不再显示字面命令，
  而是给出可读的近似/明确错误；
- 保存/重开/Undo 后公式源码不变。

---

### 4.3 交叉引用选择器：从“标题列表”升级为“带编号的语义引用”

**当前**（`MainWindow::RefreshReferenceItems`）：

```cpp
for (section : doc.body().sections) {
    add Section item (label = section title, payload = xref:<id>);
    for (block : section.blocks)
        if (Figure) add Figure item (label = caption);
}
```

因此：

- 只有 **Section** 与 **顶层 section 下的 Figure**；
- 没有 Subsection/Subsubsection/Equation/Table；
- 没有 subsection/subsubsection 内的 Figure；
- 没有编号，胶囊显示标题/题注，PDF 显示 `\ref` 编号；
- 表格只有题注非空时才有 `\label`，但选择器根本不列表格。

**目标**：

```text
┌ Insert reference ─────────────────────────────────────────────┐
│ Sections      Math      Figures      Tables                    │
│ Section 1  Introduction                                        │
│ Section 2  Method                                             │
│ §2.1  Problem Formulation                                     │
│ Eq. (4)  E = mc²                                              │
│ Figure 3  System overview                                     │
│ Table 1  Dataset statistics                                   │
└────────────────────────────────────────────────────────────────┘
```

- 按 `NodeKind` 分组，使用 `DocumentTraversal::VisitNodes/VisitBlocks`；
- 显示编号（需要共享的编号模型，见 §5）；
- 胶囊显示编号（与 PDF 一致），而不是标题；
- 通过 `cleveref` 或按类型生成 `\autoref`/`\ref`，避免用户手写 “Figure ”；
- 图表增加用户可读 label（`fig:arch`、`tab:stats`），并做唯一性校验；
- 点击胶囊重选（同 C-07）。

---

### 4.4 图/表卡片：题注与编辑能力

**题注**：

- `MakeFigureCard`/`MakeTableCard` 用 `NewEditor(...)`（`BlockEdit`，
  `QPlainTextEdit`）编辑 `Figure::caption` / `Table::caption`，
  提交时 `InlineFromText`。
- 但模型/渲染器都支持 `InlineContent`：`Inline(fig->caption)`、
  `Inline(table->caption)` 会处理公式、citation、cross-reference、粗斜体。
- 结果：**GUI 显示的是 `InlineToPlainText`，编辑一次就把富内容压平**。
  对已有富题注的项目（例如导入的 JSON）是数据丢失。

**表格**：

- `QTableWidget` 设置 `NoEditTriggers`；单元格只显示纯文本；
- `DocumentEditor` 已有 `SetTableCell`/`InsertTableRow`/`DeleteTableRow`/
  `InsertTableColumn`/`DeleteTableColumn`，但 GUI 没有任何入口；
- 插入固定 3×3，不能选行列，不能改对齐，不能合并。

**建议**：

1. 题注改用 `InlineEditor`（与正文同一套富文本/引用/公式；提交走
   `EditCaption` 的 `InlineContent` 版本——需要新增 `EditCaptionRich` payload，
   或复用 `EditCaptionPayload` 并直接传 `InlineContent`）。
2. 表格单元格双击进入 `InlineEditor`；提交用 `SetTableCell`。
3. 表头/列对齐/行列增删做成卡片工具栏。
4. 图片宽度用 `FigureWidth`；增加 `Replace image`。

---

### 4.5 作者与机构

- Authors/Institutions 用纯文本行 + 上标字符解析（`SetAuthorsText`、
  `SuperscriptOrdinal`）；绑定面板提供图形化勾选。
- 限制：GUI 显示只支持 5 个机构（`supers[]` 长度 5，超出重复 `⁵`）；
  解析器 `SuperscriptOrdinal` 支持 1–10；LaTeX 渲染使用 `\textsuperscript{数字}`，
  没有 5 个的限制。因此这是**纯 GUI 显示 bug**，不是模型限制。
- 缺：作者邮箱编辑（`Author::email` 有模型无 UI）、作者顺序拖拽、
  机构顺序调整、作者贡献标注、通讯作者标记。
- 建议：作者/机构改成结构化表格（姓名、邮箱、机构多选、通讯作者），
  把上标作为渲染结果而不是输入语法。

---

### 4.6 PDF 预览

- `PdfPreview` 用 `pdfinfo` 取页数、`pdftoppm` 转 PNG；
  没有文本层，因此不能选中/搜索/复制，链接不可点。
- `SourceMap` 已能把 LaTeX 行映射回 Node，但只用于编译器错误；
  没有“点击 PDF 跳回对应块/反向高亮”。
- 预览渲染在 GUI 线程同步执行（另有设计文档 P1-04），大文档会卡。
- 建议：用 poppler 的文本层做选择/搜索；用 SourceMap + 页内坐标做双向定位；
  渲染移到 worker。

---

### 4.7 字数统计与中文

`MainWindow::CountWords`：

```cpp
return text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts).size();
```

- 中文没有空格 → 一整段被算成 1 个词；
- 只统计 Section 标题、Section/Subsection 的 Paragraph，漏掉
  Subsection/Subsubsection 标题、Subsubsection 段落、Figure/Table 题注；
- 建议：按 Unicode 词边界/字符统计，区分“字符数/词数”，并覆盖所有
  `VisitInlineContent`。

---

### 4.8 文档与实现不一致（会增加“看起来有、其实没有”的错觉）

| 文档声明 | 实际代码 | 影响 |
| --- | --- | --- |
| `README.md`：“`@` 引用菜单：输入 `@` 弹出 References / Sections / Figures 分组菜单” | 全仓没有 `@` 键处理；`BlockEditor` 注释明确说旧的 `@` 菜单已移除 | 用户按 README 操作无反应 |
| `docs/inline-editor.md`：“可重新选择：释放鼠标时发 `CitationTokenActivated` / `CrossReferenceTokenActivated`，宿主据此打开选择器替换 token” | 两个信号只有 emit，没有任何 `connect` | 点击已插入引用无反应（C-07/X-07） |
| `docs/inline-editor.md` 的 token 表把 payload 写成 `key,key2`（暗示多 key 可用） | token/payload 支持 `key,key2`，但 picker 只能单条插入 | `\citep{a,b}` 无法通过 UI 生成 |
| （需求侧）插入 citation 时应能看见“当前有哪些引用、各自编号” | picker 只有作者/年份，没有标题/编号 | 用户明确指出的不足（见 §4.1） |

**建议**：要么实现这些行为，要么把文档改成“当前限制”，不要留下“文档承诺
但功能不存在”的落差。本文写完后，建议同步修正 `README.md` 与
`docs/inline-editor.md`。

---

## 5. 需要的数据模型 / Schema 变更（按优先级）

当前 JSON schema 为 v3，模型集中在 `document/Document.h`。以下变更会影响
`DocumentEditor`、`DocumentTraversal`、`ProjectPersistence`/`ProjectMigrator`、
`LatexPlanner`、`Validator`、`BlockEditor` 与测试。

| 变更 | 目的 | 影响 | 建议版本 |
| --- | --- | --- | --- |
| `Citation.mode` 进入编辑器 token | 修复 Narrative 丢失（P-9） | `InlineEditor` token 编码/`InlineText`/测试 | 不需要 schema 变更（已存 mode） |
| `Citation` 增加 locator/prefix/suffix | 支持 `\citep[see][p.3]{}` | schema v4；渲染器 | v4 |
| `TextMark` 增加 Underline/Superscript/Subscript/Code | 富文本（R-01~R-04） | schema v4；渲染；编辑器；测试 | v4 |
| `InlineNode` 增加 `Footnote` | 脚注（S-02） | schema v4；遍历；渲染；UI | v4 |
| `InlineNode` 增加 `Link` | 超链接（R-05） | schema v4；安全（URL 校验） | v4 |
| `Block` 增加 `ListBlock`（ordered/unordered + items） | 列表（S-01） | schema v4；编辑器；渲染；遍历 | v4 |
| `Block` 增加 `TheoremBlock`/`ProofBlock` | 定理（S-03） | schema v4；模板需 `amsthm` | v4 |
| `Block` 增加 `CodeBlock` | 代码（S-04） | schema v4；安全/字体；渲染 | v4 |
| `Figure` 增加 `label` + `subfigures` | 图表交叉引用/子图（S-06/F-12） | schema v4；渲染 `subcaption` | v4 |
| `Table` 增加 `label`、合并单元格 | 表格交叉引用/复杂表 | schema v4；渲染 | v4 |
| `BackMatter` 增加 appendix/acknowledgments | 论文结构（S-07/S-08） | schema v4；渲染 | v4 |
| 文档设置（citation style、语言、页面） | T-04/T-05 | `ProjectSettings` 扩展 | v4 |
| 数学宏表 | M-04 | 项目设置 + 渲染器注入 | v4 |

**迁移策略**：

- 新增字段使用 optional/默认值，旧项目加载时补默认；
- `ProjectMigrator` 增加 V3→V4 步骤（只改版本戳与默认值）；
- 每个新块类型都必须有 `NodeKind`、`NodeAddress`、`Visit*`、Persistence、
  Renderer、Validator、Editor、测试的完整链路，否则不要合并。

---

## 6. 优先级与路线图

### 6.1 建议顺序

| 阶段 | 内容 | 理由 | 估计 |
| --- | --- | --- | --- |
| **第 1 阶段（1–2 周）** | Citation Picker v2（标题/编号/已引/搜索/多选/叙述式）；连接 token 重选；修复 Narrative 丢失 | 用户明确提出，改动集中在 GUI + token 编码，收益立竿见影 | M |
| **第 2 阶段（2–3 周）** | Math Palette v1（结构/字体/符号 + 补全 + 预览回退补齐）；公式卡片显示编号；label 校验 | 让“图形化”覆盖数学；复用现有异步预览 | M–L |
| **第 3 阶段（2–4 周）** | 题注富文本；表格单元格/行列编辑；图宽；交叉引用选择器补全 + 编号 | 模型/渲染器大多已支持，主要是 GUI 与编号模型 | L |
| **第 4 阶段（3–4 周）** | 矢量图（PDF/SVG）；中文模板（ctex + XeLaTeX）；列表/脚注（模型 + 迁移） | 科研写作刚需，但涉及 schema/渲染/模板 | L |
| **第 5 阶段** | author-year/biblatex；定理/代码/子图；`.tex`/arXiv 导出；查找替换 | 期刊适配与工作流 | L |
| **持续** | PDF 文本层/双向定位、评论/版本、可访问性/i18n | 体验与长期维护 | M–L |

### 6.2 每项优先级（汇总）

| 优先级 | 项目 |
| --- | --- |
| **P0** | C-01~C-07（citation picker + 重选 + mode）、M-01~M-03（面板/补全/结构）、F-06/F-08/F-09（题注/表格）、X-01~X-03（交叉引用）、T-01（中文模板，中文用户） |
| **P1** | F-01（矢量图）、S-01/S-02（列表/脚注）、C-12（author-year）、S-03/S-04（定理/代码）、T-06/T-09（导出/源码）、X-05/X-06（label）、M-04~M-11 |
| **P2** | S-06（子图）、F-10/F-11（表格高级）、T-02/T-03（模板/包）、W-01（查找替换）、W-08/W-09（PDF 文本/双向）、S-07~S-09（附录/目录）、W-07（中文计数） |
| **P3** | W-02~W-06（拼写/评论/版本/协作）、R-05（链接）、S-11（术语表）、W-10~W-13（设置/主题/可访问性/i18n） |

---

## 7. 可执行验收清单（建议直接写成测试）

### Citation

1. picker 行包含：编号（已引时）、作者、年份、标题（截断）、venue；
2. 搜索 `attention` 命中标题；搜索 `vaswani` 命中作者；搜索 `2017` 命中；
3. 已引用条目出现在 “CITED” 组并显示 `[n]`，未引用在 “NOT CITED”；
4. 多选 2 条 → 文档中一个 `Citation` 节点，keys 顺序稳定 → PDF `\citep{a,b}`；
5. Narrative 插入 → 保存/重开/Undo/Redo 后仍为 Narrative；
6. 点击胶囊 → picker 预选 → 修改 → token 被替换（不是新增）；
7. 空文献库 → 显示导入入口；
8. 重复插入同一 key：编号不变，UI 可提示“已引用”。

### Math

9. 点击 `\mathbb` → 生成 `\mathbb{}`，光标在花括号内；
10. 选中 `x+1` 点击 `\frac` → `\frac{x+1}{}`；
11. `\mathbb{R}`、`\mathfrak{g}`、`\mathcal{L}` 的真实 TeX 预览与 PDF 正确；
    `\mathscr` 在加入 `mathrsfs` 后才作为验收项；
12. 断网/无 runtime 时，上述命令不再静默显示字面命令，而是明确降级提示；
13. 公式卡片显示 `(n)`，与交叉引用显示一致；
14. 重复 label 在保存前报诊断。

### 图/表

15. 插入 PDF 矢量图并构建成功；
16. 拖拽 PNG 到编辑区 → 自动 `Stage` + 插入；
17. 题注输入 `$\mathcal{L}$ and \citep{key}` → 保存/重开后仍是富内容；
18. 表格单元格双击编辑 → 保存后 JSON 为 `InlineContent`；
19. 增删行列/对齐 → PDF 表格结构正确；
20. 图片宽度 50% → PDF `width=0.5\linewidth`。

### 交叉引用

21. 引用 Equation/Table/Subsection/Subsubsection 均出现在 picker；
22. 胶囊显示编号（`Eq. (2)`/`Table 1`/`§2.1`），与 PDF 一致；
23. 空题注表格不允许被引用，或总是写入 label；
24. 点击胶囊可重选。

### 中文 / 模板 / 导出

25. 新建中文文章 → 输入中文正文/公式说明 → PDF 正常显示；
26. 导出 `.tex` + assets + `.bib`，可在 Overleaf 编译；
27. author-year 模板能构建 `(Smith, 2020)` 样式；
28. 列表/脚注/定理/代码在 PDF 中正确渲染（模型落地后）。

---

## 8. 参考实现与规范

以下仅作为“成熟做法”的参照，不代表本项目必须照搬：

- **中文排版**：[CTAN ctex](https://ctan.org/pkg/ctex)（支持 pdfLaTeX/XeLaTeX/LuaLaTeX，
  提供中文标点、字号、标题格式与多引擎字体配置）。
- **交叉引用**：[CTAN cleveref](https://ctan.org/pkg/cleveref)（按目标类型自动生成
  “Figure 3”“Eq. (2)”等，并支持引用列表压缩）。
- **子图**：[CTAN subcaption](https://ctan.org/pkg/subcaption)（与 caption 包配套，
  提供 subfigure/subtable）。
- **文献样式**：[CTAN biblatex](https://ctan.org/pkg/biblatex)（Biber 后端、Unicode、
  可定制样式、多文献表；对应本项目的 author-year/样式扩展）。
- **算法伪代码**：[CTAN algorithm2e](https://ctan.org/pkg/algorithm2e)。
- **单位/量纲**：[CTAN siunitx](https://ctan.org/pkg/siunitx)。
- **引用选择器交互参考**：[Zotero Word Plugin 文档](https://www.zotero.org/support/word_processor_plugin_usage)
  ——其 citation dialog 支持按标题/作者/年份搜索、顶部 “Cited” 分组、
  多条目引用、叙述式/Omit Author、locator（页码）、prefix/suffix。
  这正是本文 §4.1 目标设计的现实参照。

> 说明：上述包是否可用取决于模板与 runtime；引入新包时必须同步更新
> `TemplateDefinition.packages`、`latexmk` 依赖检查与回归测试。

---

## 9. 代码位置索引（实现时按图索骥）

| 主题 | 文件/函数 |
| --- | --- |
| 文档模型/Block/Inline | `document/Document.h`、`document/DocumentEditor.{h,cpp}`、`document/DocumentTraversal.{h,cpp}` |
| 富文本标记 | `document/Document.h`（`TextMark`）、`document/InlineText.{h,cpp}` |
| Citation 数据/渲染 | `document/Document.h`（`Citation`/`CitationMode`）、`render/LatexPlanner.cpp`（`\citep`/`\citet`） |
| Citation 编号 | `numbering/CitationNumberResolver.{h,cpp}`、`app/ProjectController.cpp::CitationNumbers` |
| Citation 选择器 | `app/BlockEditor.cpp::ShowCitationPicker`、`app/PopupList.{h,cpp}` |
| Citation 条目构建 | `app/MainWindow.cpp::RefreshReferenceItems`、`app/OutlinePanel.cpp` |
| Citation 插入/重建 | `app/InlineEditor.cpp::InsertCitationObject/ContentInRange` |
| 参考文献 | `bibliography/BibTeX.cpp`、`bibliography/BibliographyService.{h,cpp}`、`app/AddReferenceDialog.cpp` |
| 数学校验/生成 | `math/MathValidator.cpp`、`math/MathGenerator.cpp` |
| 数学编辑/预览 | `app/MathEditorDialog.{h,cpp}`、`app/MathPreviewRenderer.cpp`、`app/math/MathRenderService.cpp` |
| 公式块 | `app/BlockEditor.cpp::MakeEquationCard` |
| 交叉引用 | `app/BlockEditor.cpp::ShowReferencePicker`、`app/InlineEditor.cpp::CrossReferenceDisplayText`、`render/LatexPlanner.cpp`（`\ref`/`LabelFor`） |
| 图/表卡片 | `app/BlockEditor.cpp::MakeFigureCard/MakeTableCard` |
| 资源格式 | `asset/AssetManager.cpp::DetectMediaType/Stage` |
| 模板 | `template/TemplateRegistry.cpp`、`template/TemplatePackage.{h,cpp}`、`render/LatexPlanner.cpp`（citation contract） |
| PDF 预览 | `app/PdfPreview.cpp`（`pdftoppm`/`pdfinfo`） |
| 导出现状 | `build/BuildCoordinator.cpp`（debug_dump_dir）、`persistence/ProjectPdf.cpp` |
| 字数统计 | `app/MainWindow.cpp::CountWords` |
| 作者/机构上标 | `app/BlockEditor.cpp`（`supers[]`）、`app/ProjectController.cpp`（`SuperscriptOrdinal`） |
| 菜单/入口 | `app/MainWindow.cpp::BuildMenus`、`app/BlockEditor.cpp::ConfigureFormatActions` |

---

## 10. 一句话总结

当前系统已经能把**结构化文本 + 公式 + 引用 + 图表**正确地变成 LaTeX→PDF，
但“图形化”在四个地方断开了：**引用选择器的信息、数学的输入方式、
图表题注/单元格的编辑、交叉引用的完整性**。把这四处补齐，再处理
**中文模板、矢量图、列表/脚注**这三类结构缺口，才谈得上“用图形界面写论文”。
