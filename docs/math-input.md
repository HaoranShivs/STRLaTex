# Math input: InlineMath and EquationBlock

This document records how the math-input redesign from `equation.txt` is
implemented, and where each design section lives in the tree.

## 1. One content type: `MathExpression`

`src/math/MathExpression.h` defines the single math value every feature is
built on:

```cpp
struct MathExpression { std::string latex; };
enum class MathFlavor { Inline, Display };
enum class MathState  { Valid, Pending, Invalid };
```

`latex` is the **bare math body** the user typed (`\frac{a}{b}`). Delimiters
and outer environments are never stored (design §2, §10).

The document model (`src/document/Document.h`) uses it in two places:

| Type | Where | Design |
| --- | --- | --- |
| `InlineMath{ MathExpression expression; }` | a variant alternative in `InlineContent`, i.e. inside a Text block | §3 |
| `EquationBlock{ NodeId id; MathExpression expression; bool numbered; std::string label; }` | a `Block` variant alternative | §4 |

## 2. The LaTeX input boundary: `MathValidator`

`src/math/MathValidator.cpp` scans the source once and rejects:

* document-level commands (`\documentclass`, `\usepackage`, `\section`,
  `\input`, `\def`, `\newcommand`, …),
* outer formula/document environments (`\begin{equation}`, `\begin{align}`,
  `\begin{figure}`, `\begin{document}`, …),
* user-supplied delimiters (`$…$`, `\(…\)`, `\[…\]`),
* unbalanced braces, `\left`/`\right` and `\begin`/`\end`.

Math-internal structures stay legal: `\frac`, `\sqrt`, `\sum`, `\int`,
`\left…\right`, `\begin{aligned}`, `\begin{cases}`, `\begin{pmatrix}`, font
commands and accents. Empty source is `Pending`, not `Invalid`; invalid source
is reported and **kept verbatim** (design §8). `Validator` turns those results
into `E-MATH-*` diagnostics so they appear in the Problems panel.

## 3. The generator owns the environment

`src/math/MathGenerator.cpp` is the only place that writes delimiters or an
outer environment:

```text
inline       \(\frac{a}{b}\)
numbered     \begin{equation}\label{eq:energy} … \end{equation}
unnumbered   \[ … \]
```

`LatexRenderer` calls it for inline math and for equation blocks (design §6).
Equation labels resolve through a label map built at the start of `Render`:
`\ref{node}` becomes `\ref{eq:energy}` when the user set a label, and falls
back to the node id otherwise. Persistence writes only `latex` + `numbered` +
`label`; the generated environment text is never saved (design §10).

## 4. Preview: `MathPreviewRenderer`

`src/app/MathPreviewRenderer.{h,cpp}` compiles a validated math body with the
bundled TeX Live `pdflatex`, emits a one-box tightly cropped PDF, reads the TeX
box ascent/descent from the compiler log, and rasterises the result at the
requested device-pixel ratio. Results are cached by source, font/style,
template and backend. If the TeX runtime or rasteriser is unavailable, the
bounded `QPainter` renderer remains as a fast fallback with
`exact == false`; the source is never rewritten.

`InlineMathObjectRenderer` implements `QTextObjectInterface`. It uses the TeX
baseline rather than the image bottom, scales ascent and descent into the
current `QFontMetricsF` line box, and paints the descent below the text
baseline. Inline math therefore does not increase the Text row height.

## 5. Inline math in a Text block

`InlineEditor` stores an inline math object as a custom **inline preview
object** carrying the LaTeX body and render metrics in its character format.
Consequences:

* the paragraph shows the rendered formula, not the source;
* clicking selects the object whole, Backspace/Delete removes it whole;
* the caret crosses it as one character, Undo/Redo and the document protocol
  are untouched;
* copy/paste inside the editor carries the private
  `application/x-strlatex-inline` flavour so marks, tokens and math survive.

The toolbar's **Inline Math** action opens `MathEditorDialog` (source field +
live preview + state) and inserts whatever body the user accepts. Double-
clicking an existing object reopens the same dialog with its source. The old
hard-coded `x^{2}` insertion is gone.

## 6. Equation rows

`BlockEditor::MakeEquationCard` builds the row described in design §4:

```text
Equation
├── LaTeX Source   (body only)
├── Preview        (MathPreviewRenderer)
├── Numbered       (checkbox)
└── Label          (eq:…)
```

Editing the source schedules a debounced re-render; validation failures show in
the row but never rewrite the source. Numbered/Label commit immediately
through `EditEquationPayload`.

## 7. Persistence

Schema V3 stores:

```json
{ "type": "inline_math", "latex": "\\frac{a}{b}" }
{ "type": "equation", "id": "n7", "latex": "E = mc^2",
  "numbered": true, "label": "eq:energy" }
```

The reader still accepts the V2 spellings (`inlineEquation`/`displayEquation`
with `math`), so existing projects load unchanged. Schema V4 added optional
Figure/Table labels and V5 adds lists/footnotes; both keep this exact math
representation, and `ProjectMigrator` stamps V2/V3/V4 files to V5 with
lossless steps.


## 8. WP-02：图形化数学输入（核心已实现）

### 8.1 命令目录与面板

`math/MathCommandCatalog` 是面板按钮、`\` 补全与宏包诊断的唯一来源：

| 类别 | 内容 |
| --- | --- |
| Structure | 上标/下标、frac/sqrt/sum/int、matrix 族、cases、aligned、array、split、binom、boxed、overbrace/underbrace、overset/underset、substack、overrightarrow、pmod、括号/绝对值/范数、导数、重音符号 |
| Font | mathcal/mathbb/mathfrak/mathscr、mathbf/boldsymbol、mathrm/mathit/mathsf/mathtt、operatorname |
| Greek | 小写/大写/变体 |
| Functions | log/ln/exp/sin/cos/tan/…、min/max/sup/inf/liminf/limsup、det/dim/ker/gcd/arg/mod |
| Operators / Relations / Arrows / Sets & Logic / Misc | 常用符号 |

- 面板中的 `$` 是唯一占位符：有选区时第一个 `$` 被选区替换，其余留空；
  无选区时插入空模板并把光标放到第一个空参数。
- 数学面板同时嵌入 `MathEditorDialog` 和公式卡片（卡片默认折叠，点
  `∑ Palette` 展开），共享 `MathPaletteWidget::InsertMathCommand`。

### 8.2 命令补全

- 输入 `\` 触发 `MathCommandCompleter`（`QCompleter`）；弹层宽度固定为可读
  尺寸，不会因为光标矩形只有 1px 而退化成光标下方的一条长线；
- `\ma` 列出 `\mathbb`/`\mathfrak`/`\mathscr`/`\mathbf`…；
- Tab/Enter 接受，Esc 关闭；接受后插入模板并把光标放到第一个空参数；
- 缺包命令在补全项后显示 `⚠ needs <package>`，但仍可选择。

### 8.3 宏包诊断

`MathValidation.required_packages` 由目录扫描得到。Validator 把它与模板
（以及 renderer 总会注入的 `amsmath`/`amssymb`）比较，缺包时在构建前报：

```text
E-MATH-MISSING-PACKAGE  math requires package 'mathrsfs';
the current template does not load it
```

因此 `\mathbb` 在全部内置模板下可用；`\mathscr` 需要模板声明 `mathrsfs`，
面板按钮置灰，不会到 PDF 阶段才发现缺包。

### 8.4 预览一致性与 note

`MathPreviewRenderer` 的纯 Qt 回退补齐了 `\mathbb`/`\mathfrak`/`\mathscr`
（Unicode 数学字母）、`\binom`、`\boxed`、`\overbrace`/`\underbrace`、
`\overset`/`\underset`、`\substack`、`\overrightarrow`、`\begin{array}`、
`\begin{split}`、`\pmod`。仍无法精确绘制的结构会把命令名写进
`MathRenderResult.note`：

- 行内公式：`InlineEditor::MathRenderNoteAt` + tooltip；
- 显示公式：卡片状态行显示 `Approximate preview: …`；
- 对话框：状态行追加 note。

### 8.5 不在本次范围

M-04 自定义宏、M-05 `\tag`/按节编号、M-08 siunitx、M-12 数学语法高亮
仍属于后续扩展；本次先打通“有入口、有补全、缺包可诊断、预览不误导”的闭环。

### 8.6 测试

- `TestMathCatalog`：命令唯一、分类/搜索、`\mathbb`→amssymb、
  `\mathscr`→mathrsfs；
- `TestMathPalette`：`MakeMathInsertion`、选区包裹、光标定位、缺包禁用、
  `MathEditorDialog` 的 `Validation()`/palette、`\` 补全接受；
- `TestMath`：`required_packages` 与 `E-MATH-MISSING-PACKAGE`；
- `TestMathPreview`：新回退命令不再显示字面命令，note 包含命令名；
- `TestInlineMathGui`：公式卡片的折叠 palette/completer、行内 note；
- `TestEndToEnd`：真实 tectonic 构建 `\mathbb{R} + \mathfrak{g}`。


### 8.7 公式卡片的展示/编辑两态

- 默认（焦点不在公式卡片上）：只显示**公式预览 + 尾部的自动编号**；
- 点击公式预览/卡片：显示 LaTeX 源码、`∑ Palette` 按钮、Numbered 与 Label；
- 点击 Palette 分类/命令或 Numbered/Label 不会抢走源码焦点，也不会退出编辑态；
  只有实际点击公式卡片外部时才提交并收起，回到纯预览；单纯焦点变化（补全
  弹层关闭、窗口切换）不会让卡片退回展示态；
- 编号来自 `DocumentNumberer`（全文文档顺序）；Numbered 关闭时编号隐藏，
  打开后重新参与编号。
