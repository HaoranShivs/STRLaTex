# Stage B：富文本行内编辑（InlineEditor）

本文档记录富文本编辑落地了什么、为什么这样切，以及每个约定在代码里的位置。
上一阶段的结构语义与遍历层见 [structure-semantics.md](structure-semantics.md)，
线程模型见 [architecture-async.md](architecture-async.md)。

## 1. 领域层：TextMark 组合与富文本序列化

### Renderer 组合式渲染（计划 §4.3）

之前 `Strong` 用 `else if` 分支判断，一个 run 同时带 Strong + Emphasis 时
Emphasis 会被丢掉。现在按组合嵌套：

```text
Strong            → \textbf{...}
Emphasis          → \emph{...}
Strong + Emphasis → \textbf{\emph{...}}
```

### 富文本编辑表示（计划 §B "Rich Text serialization"）

`document/InlineText.{h,cpp}` 新增一对互逆函数：

```cpp
std::string  InlineToRichText(const InlineContent&);
InlineContent InlineFromRichText(const std::string&);
bool         InlineIsRich(const InlineContent&);
```

* 语义 token 保持可读（`[cite:key]` / `[ref:node-id]` / `$math$`），所以字符串
  仍然可 diff，纯文本路径（搜索、outline、旧编辑器）继续可用。
* 字符标记用 `**bold**` / `*italic*` / `***bold italic***` 表达。
* **往返无损**：`InlineFromRichText(InlineToRichText(x)) == x`（测试验证）。
* 解析器不会因为漏识别而丢字：无法解析的标记原样保留为文本。星号只有在
  **紧贴文字** 时才算标记（`a * b`、`2***3` 是普通文本），避免把乘号/脚注吃掉。
* `InlineIsRich` 用于 GUI 判断"这行内容用纯文本框表达会丢东西"。

`TextRun` / `InlineEquation` / `Citation` / `CrossReference` 增加
`operator==`，使结构比较（以及"提交的内容没变就跳过"的判断）成为可能。

## 2. InlineEditor（计划 §4.1、§5、§6）

`src/app/InlineEditor.{h,cpp}`：一个 `QTextEdit`，其**字符格式就是标记状态**，
语义节点渲染成行内 token。

### Token 模型

一个 token 占**一个 QChar**（私有区 U+E000），payload 存在该字符格式的
property 上：

| Kind | payload | 显示 |
| --- | --- | --- |
| Citation | `key,key2` | 高亮胶囊 |
| CrossReference | node id | 高亮胶囊 |
| Math | LaTeX 主体 | **公式预览图片**（双击编辑源） |

数学对象的具体设计（`MathExpression`、输入边界、生成器、预览器、保存格式）
见 [math-input.md](math-input.md)。

满足计划 §5 的全部约束：

* **内部标识符不可编辑**：token 是单字符 + 属性，用户改不了里面的 key。
* **Backspace/Delete 整体删除**：按键被拦截，删除的是那个字符（即整个 token）。
* **点击即选中**：mousePressEvent 用 hit-test 命中 token 后选中整块。
* **可重新选择**：释放鼠标时发 `CitationTokenActivated` /
  `CrossReferenceTokenActivated`，宿主据此打开选择器替换 token。
* **保存/Undo/Redo 后结构不变**：提交的是 `InlineContent`，经
  `EditParagraphPayload` 走既有协议，快照式 Undo/Redo 原样恢复。

**Sanitize**：从外部粘贴进来的 token 字符若丢失了 payload，会被还原成普通
文本，而不是变成一个会被 `Content()` 静默丢弃的坏 token。

### 提交路径

```
InlineEditor.Content()  ──►  BlockEditor::ParagraphContentEdited(node, InlineContent)
                        ──►  ProjectController::EditParagraphRich(node, content)
                        ──►  EditParagraphPayload{ content }  (不走字符串往返)
```

`ProjectSession::mutable_document()` 同时暴露出来，供遍历/校验这类需要非
const 容器指针的场景使用。

## 3. 格式工具栏（计划 §4.4）

`BlockEditor::BuildFormatToolbar`：Text 行获得焦点时可见：

```text
[B] [I] [Inline Math] [Citation] [Reference]
```

* `B` / `I`：对选区合并格式；光标未选中时改变**接下来输入**的格式。
* 快捷键 `Ctrl+B` / `Ctrl+I` 直接在 InlineEditor 的 keyPressEvent 处理。
* `Inline Math` 打开 `MathEditorDialog`（源码 + 实时预览 + 状态），用户在
  对话框里输入数学主体后插入一个公式预览对象；**不再**硬编码插入 `x^{2}`。
  双击已有公式对象会带着它的源码重新打开同一个对话框。源只保存数学主体，
  `\(...\)` 由生成器添加（见 [math-input.md](math-input.md)）。
* `Citation` / `Reference` 打开 `PopupList` 选择器，payload 来自
  `SetReferenceItems`（`cite:` → 引用 key，`xref:` → 节点 id，与 `/` `@`
  菜单共用同一份数据）。

**不支持**（按计划）：字体家族、任意字号、颜色、背景色、任意行距——正文的
视觉排版由 Template 决定。粘贴时这些属性也被丢弃（见下）。

## 4. 粘贴规则（计划 §14）

| 操作 | 行为 |
| --- | --- |
| `Ctrl+V`（含富文本源） | 保留 Bold/Italic；**丢弃** font-family/size/color/背景/间距 |
| `Ctrl+Shift+V` | 只保留文本 |
| PDF 硬换行 | 继续走 `ReflowHardWrappedText`（单换行→空格，空行→分段） |

富文本粘贴的实现：HTML → QTextDocument，逐 fragment 只拷贝 weight/italic，
其余格式天然被丢掉——这正是"允许 Bold/Italic，丢弃其余"的直接表达。

## 5. 测试（tests/TestInline.cpp，13 个）

| 测试 | 覆盖 |
| --- | --- |
| `RichTextSerializationRoundTripsMarks` | 往返无损，`**`/`*` 可读 |
| `RichTextSerializationHandlesBoldItalicCombination` | `***` 组合且 plain 尾巴不继承标记 |
| `RichTextSerializationKeepsSemanticTokens` | `[cite:]`/`[ref:]`/`$...$` 保持为节点 |
| `RichTextSerializationOfPlainTextStaysPlainText` | 无标记内容序列化后原样 |
| `RichTextParseNeverDropsUnknownMarkup` | `a * b ** c` 不会被当成标记吃掉 |
| `RendererNestsStrongAndEmphasis` | `\textbf{\emph{...}}` |
| `RendererKeepsMarksThroughDocumentRender` | 整篇渲染后标记仍在 |
| `ParagraphContentRoundTripsThroughEditingProtocol` | 富内容经协议原样入库，Undo/Redo 恢复 |
| `InlineCitationAndReferenceSurviveProtocolRoundTrip` | 语义节点顺序与结构不变 |
| `InlineEquationRendersAsMathInParagraph` | `The loss $\mathcal{L}$ decreases.` |
| `InlineEquationTokenIsNotUserEditableText` | token 是节点不是字符串 |
| `PasteRulesAreExpressedInTheEditorRepresentation` | 硬换行重排 + 标记保留 |
| `SessionPersistsRichParagraph` | 保存/重开后富内容原样 |

## 6. GUI 测试的适配

Text 行从 `QPlainTextEdit` 换成了 `InlineEditor`，而 Qt 里这两个类**不共享
`QTextEdit` 基类**（都继承 `QAbstractScrollArea`）。GUI 回归测试原来直接拿
`QPlainTextEdit*`，现在通过一个 `RowEditor` 小封装按 `row_focus_key` 定位行，
并把 text/cursor/focus 转发到两种 widget 之一。行查找只认**可见**的 widget，
因为一次 rebuild 的旧行要等 deferred delete 才消失。

## 7. 富文本落库后修掉的四个实 bug

第一版只在 GUI 层搭出了结构，四个症状（加粗不稳定、斜体完全无效、点工具栏
按钮文本框膨胀、token 丢失）追到底后是三个根因，全部用回归测试钉死：

### 根因 1：`charFormat()` 在光标位置报告的是**前一个字符**的格式

`InlineEditor::Content()` 逐字符调 `charFormat()` 读标记，于是每个 run 的边界
整体错位一位：

```text
输入  plain bold italic
得到  run=[plain b|0] [old |bold] [i|0] [talic|italic]
LaTeX plain b\textbf{old }i\emph{talic}
```

加粗"稳定生效"是假象——run 足够长，读偏一位后大部分字符仍在粗体里；斜体
通常只有一两个词，首字符被丢进普通文本，所以 **PDF 里斜体永远缺一个词的
开头**，看起来"完全无效"。

**修复**：不再逐字符问格式，改为遍历 `QTextDocument` 的
`QTextFragment`——一个 fragment 天然就是"同一格式的最大连续文本"，run 边界
由 Qt 的文档模型给出，不靠猜。

### 根因 2：token 从 fragment 读取时踩了同一个坑

`TokenAt()` 用 `charFormat()`（同样报前一个字符），导致连续两个 token 时
第二个被误读成第一个的类型：文档里明明是 `[cite][xref]`，读出来却是
`[cite][cite]`，交叉引用凭空消失。

**修复**：token 的 kind/payload 直接从 **fragment 自身的 charFormat 属性**
读（fragment 与 token 一一对应），不再按位置反查。

### 根因 3：`BlockEditor` 的提交/焦点逻辑只认 `BlockEdit`

`CommitBlock` / `HasUncommittedFocus` / `CommitFocused` / `FocusedNodeId` 都用
`qobject_cast<BlockEdit*>`，而 Text 行是 `InlineEditor`，四个函数全部短路。
后果是 `HasUncommittedFocus()` 对正在编辑的 Text 行**恒为 false**，任何一次
rebuild 都会在用户敲键时把这行拆掉重建——这就是"点斜体按钮文本框一下子变大，
delete 后复原"：rebuild 在失焦状态下用塌缩的 viewport 宽度量高，一个词一行。

**修复**：四个入口都同时识别两种编辑器；`ResizeToContent()` 改用
`max(width(), contentsRect().width())` 而非瞬态的 `viewport()->width()`。

### 回归测试

`paperforge-inline-editor-test`（`tests/TestInlineEditor.cpp` +
`tests/TestInlinePdf.cpp`，ctest 名 `inline_editor`，offscreen 可跑）：

| 测试 | 钉住什么 |
| --- | --- |
| `InlineEditorReturnsExactRuns` | run 边界不错位（`plain /bold/ /italic` 四段） |
| `InlineEditorItalicReachesTheLatexAndStaysWhole` | `\emph{italic}` 整词，且**没有** `\emph{talic}` |
| `InlineEditorBoldAndItalicTogetherReachTheLatex` | `\textbf{\emph{both}}` |
| `InlineEditorRoundTripsThroughTheModel` / `...SurvivesAProgrammaticReload` | 重载后再提交不变，标记不漂移 |
| `InlineEditorKeepsTokensAcrossAReload` | cite 与 xref 同时存在且 payload 正确 |
| `InlineEditorToolbarToggleMarksTheSelection` | 选区 B/I 落到 `InlineContent` |
| `InlineEditorDirtyFlagGuardsTheRebuild` | 仅格式变化也算 dirty，保护行不被 rebuild |
| `InlineEditorHeightDoesNotBlowUpWhenUnlaid` | 重新量高不随 viewport 塌缩膨胀 |
| `BoldAndItalicReachTheBuiltPdf` | **真实 tectonic**：存储的 run 带 bold/italic、LaTeX 含 `\textbf{...}`/`\emph{...}`、PDF 产出成功 |

## 8. 第二轮报告的两个问题（工具栏 / 模板）

用户复测后又报了两条，仍然用真实控件（`MainWindow` + 真工具栏 + 真
`InlineEditor`）复现并定位。两条其实是**同一个焦点缺陷**的两种表现，另加一个
缺钩子的问题。

### 8.1 点格式按钮后文本框多出一大片空白，按 Delete 又恢复

**根因 A：工具栏按钮会抢走焦点。**
5 个 `QToolButton` 没有设 `setFocusPolicy(Qt::NoFocus)`（默认 `Qt::TabFocus`，
鼠标点击即可获得焦点）。用户点"I"的瞬间发生：

```text
点 I → 按钮获得焦点 → InlineEditor::focusOutEvent
     → emit Committed（提交的是"还没加格式"的内容）
     → documentChanged → RefreshDocumentView → RebuildFromDocument
     → 正在编辑的行被拆掉重建（失焦状态下量高 → 高度算错）
     → 点击处理器这才执行 ToggleItalic()，作用在已被销毁的旧 editor 上
```

所以标记丢失、行被重建、高度算错。修复：5 个按钮全部
`setFocusPolicy(Qt::NoFocus)`。

**根因 B：`InlineEditor` 缺少 `resizeEvent`/`showEvent` 钩子。**
`BlockEdit`（旧的行编辑器）有 `resizeEvent → Resize()`，`InlineEditor` 没有。
于是宽度变化（窗口缩放、rebuild 后重新布局、行首次显示）之后**不会重算高度**，
`setFixedHeight` 把高度钉在"用旧/塌缩宽度算出来的"值上——看起来就是一大片空白。
按 Delete 之所以"恢复"，是因为删除触发 `textChanged → ResizeToContent()`，那时
宽度已经正确了。

修复：补 `resizeEvent` / `showEvent`；`resizeEvent` 用事件携带的新宽度
（此时 `width()` 还是旧值）调用 `ResizeToWidth(event->size().width())`。

### 8.2 同样操作在 IEEE conference 模板下不行，默认模板却可以

**结论：不是模板问题。** 逐层验证：

1. **渲染层**：两个模板对同一段带标记的文本产出的 LaTeX **逐字节相同**
   （`plain \textbf{bold} mid \emph{ital}`）。
2. **真实 tectonic**：两个模板各自编译成功，无模板相关错误。
3. **GUI 全链路**：修复 8.1 之后按模板逐个跑，结果完全一致：

```text
generic-article: doc(bold=1 italic=1) tex(bold=1 italic=1)
ieee-conference: doc(bold=1 italic=1) tex(bold=1 italic=1)
```

模板之间唯一的差异是 `max_heading_depth` 和必填字段提示，与字符标记无关。

**那为什么用户看到"默认模板可以、IEEE 不行"？** 因为这条链路同时还依赖
**提交时机**：标记只有在行失焦（`Committed`）时才写进文档。默认模板下用户
先点进正文再点按钮、又点了别处，提交赶上；IEEE 模板下必填字段更多，行的
布局/hint 刷新更频繁，抢焦点引发的 rebuild 更容易插在"格式应用"与"提交"
之间，于是标记被覆盖掉。**同一个焦点缺陷，在 IEEE 下更容易踩中。**
8.1 的 NoFocus 修复同时消掉了这个模板差异。

### 8.3 新增回归测试（`paperforge-inline-editor-test`）

| 测试 | 钉住什么 |
| --- | --- |
| `ToolbarClickDoesNotGrowTheTextRow` | 点按钮后行高不膨胀，且按钮 `focusPolicy == NoFocus` |
| `ToolbarClickKeepsTheCaretInTheRow` | 点按钮后焦点仍在行内（不触发提交/rebuild） |
| `ToolbarMarksWorkOnEveryTemplate` | **逐个模板**用真工具栏加粗+斜体，断言文档与 LaTeX 都拿到标记 |
| `WideningARowReflowsWithoutUserInput` | 宽度变大后自动重排（窄 88px → 宽 46px），不需要按 Delete |
| `FreshlyBuiltRowIsNotPinnedToAGiantHeight` | 尚未布局的行不会被钉在巨大高度 |

## 9. 常见疑问

**"斜体不生效，是不是因为我没在论文里注册公式？"**
不是。InlineEquation / Citation / CrossReference 是文档里的语义节点，不需要
任何"注册"步骤。斜体失效与公式无关——是第一轮的格式错位（§7 根因 1）和
第二轮的焦点缺陷（§8.1）叠加造成的。

## 10. 尚未实施

* **Stage D**：xlsx/csv/tsv 导入、Import Table 对话框、TableSource 元数据。
* **Stage E**：TableImportRequest 的后台导入 worker（Build/Save 的管道已在
  M1 完成，导入只需走同一条 Snapshot→worker→typed event→应用线程 accept）。
* Citation/Reference 的**点击后重选**目前提供"删除 + 重新插入"，token 级的
  in-place 重选（§5 第 4 条）留给下一次迭代——现在双击 token 会选中它，
  删除后用工具栏按钮重新插入即可。
