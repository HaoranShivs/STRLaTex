# 编译工具链：Portable TeX Live + pdfLaTeX

本文档记录编译工具链重构（第一/二阶段方案 §31）落地了什么，以及为什么
IEEE 的粗体/斜体必须换引擎才能修好。线程/异步模型见
[architecture-async.md](architecture-async.md)，富文本见
[inline-editor.md](inline-editor.md)。

## 1. 根因：Tectonic 是 XeTeX，IEEEtran 按 pdfLaTeX 设计

多番测试定位到：**Tectonic 官方说明其核心是 XeTeX，不是 pdfLaTeX**；而
`IEEEtran.cls` 会主动把正文 Roman 字体设为 ptm（传统 LaTeX 的 Times 字体族）。
已有直接案例：IEEEtran 本身不按 XeLaTeX 路径设计，用 XeLaTeX 时字体选择可能
失效而 pdfLaTeX 正常。

这正是"默认模板（article 类）里粗体/斜体正常、IEEE conference 里不行"的原因：
article 类不强制 ptm，IEEEtran 强制。编辑器与协议层（前两轮修的）没有问题，
问题在编译引擎。

验证（修复后，真实 pdflatex + IEEEtran）：

```text
$ pdffonts main.pdf
NimbusRomNo9L-Regu      <- regular
NimbusRomNo9L-Medi      <- bold
NimbusRomNo9L-ReguItal  <- italic
```

三种字型齐全，无字体替换。

## 2. 生产编译链

```text
Document → Snapshot → LatexRenderer → BuildPackage
        → TemplateDefinition.toolchain（模板决定引擎）
        → CompilerFactory → TexLiveCompiler
        → latexmk → pdfLaTeX / XeLaTeX / LuaLaTeX → BibTeX
        → PDF → PreviewGate → PdfPreview
```

* **模板决定编译要求**：`TemplateDefinition.toolchain`
  （engine / bibliography_engine / required_packages）。两个模板均
  `PdfLatex + BibTex`；`XeLatex`/`LuaLatex` 已进协议，留给需要
  fontspec/Unicode 的模板（方案 §32）。
* **Renderer 只生成 LaTeX**，不选编译器、不感知本机环境。
* **Compiler 执行编译**：`TexLiveCompiler` 以 `latexmk` 驱动
  （`-pdf|-xelatex|-lualatex -interaction=nonstopmode -file-line-error
  -halt-on-error`），工作目录为 `CompileRequest.workspace`。
* **Runtime 提供固定 TeX 环境**（见下）。
* **BuildCoordinator 只做调度**，按 snapshot 携带的 toolchain 选择编译器。

## 3. Portable TeX Live runtime

```text
runtime/texlive/bin/x86_64-linux/{latexmk,pdflatex,xelatex,lualatex,bibtex}
runtime/texlive/texmf-dist/
```

* 由 `tools/build-runtime/build_runtime.sh` 以 **user-mode** 安装（无需 root）：
  `install-tl --profile texlive.profile`（scheme-basic）+
  `tlmgr install $(tools/build-runtime/packages.txt)`。
* `packages.txt` 里必须是**真实 tlmgr 包名**：`amssymb` 由 `amsfonts` 提供，
  `latex-base`/`latex-recommended` 是集合名，`fontconfig` 是系统库而非 TeX 包，
  这三类都不能直接写。当前清单 24 项已逐项用 `tlmgr info` 校验通过。
* runtime 约 265MB，**不进 git**（`.gitignore` 忽略 `/runtime/`）；靠脚本可复现。
* **禁止依赖**用户 shell PATH / 系统 TeX Live / MiKTeX / `~/.texlive*`。
  `TexLiveCompiler` 显式设置 `PATH`（只含 runtime bin）、`HOME`、
  `TEXMFHOME/TEXMFVAR`，并清空 workspace 后再 stage（避免 latexmk
  "gave an error in previous invocation" 用旧状态拒绝新构建）。
* **禁止 `tlmgr update --all`**；TeX Live 升级 = Runtime 显式版本升级。

## 4. Runtime 健康检查（RuntimeManager）

* 定位 `runtime/texlive`，校验 `bin/<platform>` 下
  latexmk/pdflatex/bibtex/kpsewhich。
* 执行**真实最小编译测试**（font-test.tex：regular/bold/italic/bold-italic）。
* `RuntimeStatus = Healthy | Missing | Corrupted | UnsupportedVersion`；
  不健康时**禁止进入正式 Preview Build**，错误归类为
  `CompileFailureKind::RuntimeMissing`（`E-RUNTIME`），与文档 LaTeX 错误
  严格区分（方案 §21、§37、§38），不会混进 Problems Panel 的普通文档错误。

## 5. 诊断与失败分类

`ParseMessages` 识别 `!` 错误块（带 `l.<n>` 行号回填）、`file:line:` 引用、
`Warning`、`Overfull/Underfull \hbox`，并分类：
`RuntimeMissing / RuntimeCorrupted / PackageMissing / FontMissing /
LatexError / BibliographyError / Timeout / Cancelled`。
`CompileResult.auxiliary_logs` 保留 `latexmk.log` 与 `main.log`。

取消与超时（§36）：`latexmk` 起独立进程组，取消/超时先 `SIGTERM` 整组、
宽限后 `SIGKILL`，确保 pdflatex/bibtex 子进程一并结束；单次 build 超时 120s。

## 6. 回归测试

| 测试 | 钉住什么 |
| --- | --- |
| `RuntimeIsHealthy` | runtime 存在、可执行文件齐全、健康编译通过 |
| `FontTestBuildsWithoutSubstitution` | font-test.tex 经 pdfLaTeX 编译，无字体替换（§19） |
| `IeeeMarksReachPdfLatex` | **本 bug 的永久回归**：IEEEtran 下四种标记组合经真实 pdflatex 构建，LaTeX 含 `\textbf{}`/`\emph{}`/`\textbf{\emph{}}`，无字体替换（§39） |
| `CompileRequestCarriesToolchain` | engine 随请求传递，不自行推断 |
| `BoldAndItalicReachTheBuiltPdf`（inline-editor 测试） | **IEEE 模板 + 自带 runtime 端到端**：InlineEditor 输入 → 协议 → pdfLaTeX → PDF；并断言 `pdffonts` 可见粗体/斜体字型 |
| `ToolbarMarksWorkOnEveryTemplate` | 每个模板下工具栏加粗/斜体都落库并进入 LaTeX |

`runtime-tests/` 下另有可手工复现的 `font-test.tex` 与 `ieee-test.tex`。

## 7. Tectonic 的处置（§26）

`TectonicCompiler` 保留源代码与测试注入路径，但**不再进入默认 production 编译
路径**；`tools/bin/tectonic` 与 `tools/tectonic-cache` 暂留（离线可复现），待
Portable TeX Live 稳定后移除。

## 8. 第二阶段（未实施）

XeLaTeX / LuaLaTeX / Biber 真实启用、fontspec、Unicode、中文模板、
`runtime-manifest.json`、Windows 发布目录（§32、§34、§35）。
