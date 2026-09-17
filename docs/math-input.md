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
with `math`), so existing projects load unchanged; `ProjectMigrator` stamps
V2 files to V3 with a lossless step.
