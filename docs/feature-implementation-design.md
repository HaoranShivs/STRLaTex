# 功能补全与改进实现方案（接口级设计）

> 输入基线：本仓库当前工作树 + [`docs/feature-gaps.md`](feature-gaps.md)（缺口清单）。
> 本文把该清单中的 P0/P1 项拆成可独立交付的工作包（WP），为每个工作包给出
> **可评审的接口草案、数据模型/JSON 形态、命令载荷、渲染与迁移策略、测试与依赖**。
> 本文是设计文档，不是已实现代码；签名可以在评审后调整。
>
> 相关文档：[运行时调用链路](runtime-call-chain.md)、
> [编辑器设计审查](editor-design-review.md)、[冗余清理记录](redundancy-cleanup.md)。

> **实施状态（2026-10）**：WP-00、WP-01、WP-03 已实现；WP-02 的核心
> （MathCommandCatalog、面板、命令补全、宏包诊断、预览 note）已实现并通过测试。
> M-04（自定义宏）、M-05（\tag/按节编号）、M-08（siunitx）与 M-12（数学语法高亮）
> 仍待后续迭代。
> token 编解码见 `document/InlineTokenCodec`，citation picker 见
> `BlockEditor::ShowCitationPicker`，缓存见
> `ProjectController::CitationResolver`；编号见
> `numbering/DocumentNumberer` 与 `numbering/LabelRegistry`；schema 已升到 v5
> （v4：Figure/Table 可选 `label`；v5：ListBlock 与 Footnote）；数学目录/面板/补全见
> `math/MathCommandCatalog`、`app/MathPaletteWidget`、
> `app/MathCommandCompleter`，缺包诊断见 `Validator` 的
> `E-MATH-MISSING-PACKAGE`。实现记录见
> [`docs/inline-editor.md`](inline-editor.md) §10/§11 与
> [`docs/math-input.md`](math-input.md) §8。WP-04 已接入富文本题注、可配置的表格
> 插入、单元格草稿弹窗、行列/对齐/表头命令及图片宽度选择；数据仍使用现有 v4。
> 实现与验证入口见 [`docs/tables-and-captions.md`](tables-and-captions.md) 与 §7.8。
> WP-05 已接入 PDF/可选 SVG/EPS 转换、图片替换、拖放与粘贴，以及异步 PDF
> 缩略图；资源元数据沿用 v4，无需迁移。见 [图片与矢量图](figures-and-assets.md)
> 与 §8.7。WP-06 已接入列表与脚注：ListBlock/Footnote 模型、可撤销命令、
> V4→V5 迁移、遍历/渲染/校验、列表卡片与脚注编辑弹窗；WP-07 已接入定理/
> 证明与代码块（amsthm + 代码 fallback）、模板能力、卡片与真实 TeX 构建。
> 实现记录见 §9.10、§10.8；后续工作包（WP-08 起）仍按本文接口草案推进。

---

## 0. 阅读指南与工作包矩阵

本文的接口都遵守现有架构的四条硬约束：

1. **领域层 Qt-free**：`src/app/` 之外不出现 Qt 类型；GUI 只做投影与命令构造。
2. **所有文档变更走 `EditCommand`**：不得在 GUI 直接改 `Document`；每次成功变更
   产生新 `ProjectRevision`，Undo/Redo 依赖现有快照历史。
3. **LaTeX 只在 renderer 生成**：Document 模型不含 `\section`/`\label` 等字符串。
4. **schema 只增不改、加载时迁移**：磁盘文件不在加载时被重写；保存写当前版本。

工作包与缺口 ID 的对应关系：

| WP | 主题 | 对应缺口 | 优先级 | 依赖 | 估计 |
| --- | --- | --- | --- | --- | --- |
| WP-00 | 文档/行为一致性修复 | 4.8、C-07、X-07 | P0（立即） | — | S |
| WP-01 | Citation Picker v2 + 叙述式/重编辑 | C-01~C-11、P-1~P-14 | P0 | WP-00、PopupList v2 | M |
| WP-02 | Math Palette + 命令补全 + 预览一致性 | M-01~M-12 | P0 | MathCommandCatalog | M–L |
| WP-03 | 交叉引用 + DocumentNumberer | X-01~X-07 | P0 | 编号服务 | M–L |
| WP-04 | 富文本题注 + 表格编辑 | F-06、F-08~F-11、R-07 | P0 | WP-03（题注编号） | L |
| WP-05 | 图片宽度/label/替换 + 矢量图 | F-01~F-05、F-11、F-12 | P1 | AssetManager 扩展 | M–L |
| WP-06 | 列表 + 脚注（schema v5） | S-01、S-02、R-06 | P1 | schema v5 | L |
| WP-07 | 定理/证明 + 代码块 | S-03、S-04、M-09 | P1 | schema v5、模板包 | L |
| WP-08 | 子图 + 表格高级能力 | S-06、F-10、F-13 | P2 | WP-04/05 | L |
| WP-09 | 中文模板 + FrontMatter 适配器 | T-01、T-02、M-13 | P1（中文） | 模板注册表扩展 | M |
| WP-10 | author-year / biblatex | C-12、T-04 | P1 | CitationResolver 样式化 | L |
| WP-11 | 导出 .tex/arXiv + 源码视图 | T-06、T-09 | P1 | 现有 renderer | M |
| WP-12 | 查找替换、PDF 文本层/双向定位 | W-01、W-08、W-09 | P2 | 新 PDF 服务 | L |
| WP-13 | 派生数据缓存与性能 | W-07、P0 性能 | 持续 | 无 | S–M |

> 建议的实施顺序：**WP-00 → WP-01 → WP-02 → WP-03 → WP-04 →（WP-09/05）→
> （WP-06/07/10）→ WP-11 → WP-12**。理由见 §16。

---

## 1. 兼容与演进规则

### 1.1 C++ 接口演进

- **只加必选参数是破坏性的**：公共接口的新参数一律给默认值，或用重载保留旧签名。
- **variant 只追加**：`InlineNode`/`Block`/`EditPayload` 的新类型追加到末尾；
  不用 `std::variant::index()` 做持久化或协议判断（持久化用 type 字符串）。
- **Qt 信号**：改变参数列表会破坏 connect；新需求优先新增信号，旧信号保留并
  标记 deprecated（第一版可保留到下一次大版本）。
- **测试接缝**：新增服务尽量以纯函数/值对象暴露，避免 GUI 测试才能验证。

### 1.2 schema 版本

- 当前 `kSchemaVersion = "5"`；v4 已承载 Figure/Table `label`，v5 承载
  ListBlock 与 Footnote。
- `ProjectMigrator` 增加 V3→V4（label）与 V4→V5（列表/脚注）：旧文件在内存里
  补默认值，`migrated = true` 但**不重写磁盘**；保存时写 v5。
- 读取器同时接受旧写法；写入器只写 v5。迁移测试必须覆盖“V3/V4 文件打开后
  不丢数据、保存后再读回一致”。

### 1.3 模板能力

新增内容类型（定理、代码、子图、中文）都要求模板声明能力；未声明的模板在
插入菜单中隐藏该类型，Validator 报 `E-TEMPLATE-UNSUPPORTED`，而不是生成非法
LaTeX。模板扩展字段见 §2.8 与各 WP。

---

## 2. 公共基础设施（多个 WP 共用）

### 2.1 `PopupList` v2：两行条目、多选、可选开关

**新增/修改头文件** `app/PopupList.h`：

```cpp
namespace pf::gui {

class PopupList : public QFrame {
    Q_OBJECT

  public:
    struct Item {
        QString label;      // 主行：作者 · 年份 / 标题
        QString title;      // 次行：论文标题、描述（可为空）
        QString detail;     // 右侧：编号/venue/类型（可为空）
        QString group;      // 分组标题
        QString payload;    // 返回值（key、node id、命令词）
        QString search;     // 小写过滤文本（可包含标题/作者/venue/key）
        bool checkable = false;
        bool checked = false;
        bool enabled = true;
    };

    struct Options {
        bool multi_select = false;
        bool show_toggle = false;    // 例如 Narrative 开关
        QString toggle_label;
        bool toggle_initial = false;
        int width_px = 340;
        int max_height_px = 420;
        QString empty_title;         // 空状态标题（不是一行灰字）
        QString empty_hint;
    };

    explicit PopupList(QWidget* parent = nullptr);

    // 兼容旧调用（块命令、交叉引用）。
    void popup(const QPoint& global_pos, const std::vector<Item>& items,
               const QString& filter = {});
    // 新调用：多选/开关/更宽的两行布局。
    void popup(const QPoint& global_pos, const std::vector<Item>& items,
               const Options& options);

    void SetFilter(const QString& text);
    bool eventFilter(QObject* watched, QEvent* event) override;

  signals:
    // 旧事件：单选时仍发出，保持现有调用方不变。
    void chosen(const QString& payload);
    // 新事件：多选/开关模式。单选模式下也会在 Enter 时发出，便于统一处理。
    void chosenMany(const QStringList& payloads, bool toggle_checked);
    void dismissed();

  private:
    void Refilter();
    void ConfirmCurrent();
    // 实现建议：
    // - QTreeWidget 两列 + QStyledItemDelegate 画两行（label/title），
    //   setUniformRowHeights(false)，宽度按 Options::width_px；
    // - checkable item 用 Qt::ItemIsUserCheckable；
    // - Enter：multi_select → chosenMany；否则 chosen（兼容）；
    // - Esc：dismissed；外部点击：dismissed。
};

} // namespace pf::gui
```

**兼容性**：`Item` 的前五个字段保持不变，现有 `ShowInsertMenu`、`ShowReferencePicker`
无需改动；`chosen` 保持原语义。

**测试**：`TestPopupList`（新增）覆盖过滤、分组、键盘、多选、空状态、旧 `chosen`。

---

### 2.2 `InlineContentEditorDialog`：可复用的行内内容编辑弹窗

用于脚注、表格单元格、题注等“需要临时打开一个富文本编辑器”的场景，避免每个
场景各写一套 `QDialog + QTextEdit`。

**新文件** `app/InlineContentEditorDialog.h`：

```cpp
namespace pf::gui {

class InlineContentEditorDialog : public QDialog {
    Q_OBJECT

  public:
    explicit InlineContentEditorDialog(const InlineContent& initial,
                                       QWidget* parent = nullptr);

    // 复用正文行同样的编号/交叉引用显示。
    void SetCitationResolver(std::shared_ptr<const pf::CitationNumberResolver>);
    void SetCrossReferenceLabels(std::map<QString, QString>);
    // 可选的上下文：例如表格单元格不允许 citation，可在这里限制动作。
    void SetAllowedActions(const QList<QAction*>& actions);

    void SetPlaceholder(const QString& text);
    InlineContent content() const;

    // 测试钩子
    InlineEditor* Editor() const { return editor_; }
};

} // namespace pf::gui
```

实现：内部放一个 `InlineEditor`，底部 `QDialogButtonBox`；确认时返回
`editor_->Content()`。它复用现有富文本/公式/引用能力，不新增数据路径。

---

### 2.3 `InlineTokenCodec`：citation token 的 mode 编码

**问题**：当前 token payload 是 `"key1,key2"`，`ContentInRange` 默认重建为
`Parenthetical`，导致 Narrative 丢失。

**新文件** `document/InlineTokenCodec.h`（纯 C++，可单测）：

```cpp
namespace pf {

// 编码格式（向后兼容）：
//   "key1,key2"       旧格式，等价于 Parenthetical
//   "p|key1,key2"     显式 Parenthetical
//   "n|key1,key2"     显式 Narrative
std::string EncodeCitationToken(const Citation& citation);
std::optional<Citation> DecodeCitationToken(const std::string& payload);

// 逗号是 key 分隔符；BibTeX key 不允许逗号（见 BibTeX ValidKey）。
} // namespace pf
```

`InlineEditor::InsertPillObject`/`ContentInRange`/`SanitizeTokens` 全部改用这对
函数。`InlineToRichText`/`InlineFromRichText` 增加 `[citet:key]` 形式，读取器
同时接受旧的 `[cite:key]`。

**测试**：`TestCitationTokenCodec`（编码/解码/旧格式兼容/空 key/重复 key）。

---

### 2.4 `MathCommandCatalog`：数学命令与宏包的唯一目录

**新文件** `math/MathCommandCatalog.h`（纯 C++，Qt-free）：

```cpp
namespace pf {

enum class MathCategory {
    Structure, Font, Greek, Operator, Relation, Arrow, SetLogic, Misc
};

struct MathCommand {
    std::string command;       // "mathbb"（不含反斜杠）
    std::string snippet;       // "\\mathbb{$}"；$ 为光标/选区占位
    std::string label;         // 按钮/补全显示："ℝ"
    std::string description;   // "双线体：实数集/期望"
    MathCategory category = MathCategory::Misc;
    std::vector<std::string> packages; // {"amssymb"}；空 = 基础 LaTeX
};

class MathCommandCatalog {
  public:
    static const MathCommandCatalog& Instance();
    const std::vector<MathCommand>& All() const;
    std::vector<MathCommand> InCategory(MathCategory category) const;
    std::vector<MathCommand> Search(const std::string& query) const;
    const MathCommand* Find(const std::string& command) const;
    // 扫描 LaTeX body，返回其中命令所需的宏包（用于 Validator/UI 提示）。
    std::vector<std::string> RequiredPackages(const std::string& latex) const;
};

} // namespace pf
```

目录初始内容（与 `feature-gaps.md` §3.1.4 对齐）：

| 类别 | 命令（示例） | 宏包 |
| --- | --- | --- |
| Structure | frac/dfrac/tfrac/sqrt/sum/prod/int/oint/matrix/pmatrix/bmatrix/cases/aligned/array | amsmath |
| Font | mathcal/mathrm/mathit/mathsf/mathtt（基础）；mathbb/mathfrak（amssymb）；mathscr（mathrsfs）；boldsymbol/operatorname（amsmath） | 见括号 |
| Greek | alpha…omega、Gamma…Omega、varepsilon/varphi/vartheta | 基础 |
| Operator | pm/mp/times/div/cdot/ast/star/circ/oplus/otimes | 基础 |
| Relation | leq/geq/neq/approx/equiv/propto/sim/simeq/cong | amssymb |
| Arrow | to/rightarrow/leftarrow/Rightarrow/Leftarrow/leftrightarrow/mapsto | 基础 |
| SetLogic | in/notin/subset/supset/subseteq/cup/cap/forall/exists/emptyset | 基础 |
| Misc | infty/partial/nabla/ell/hbar/Re/Im/angle/triangle | amssymb |

**测试**：命令唯一性、snippet 含 `$` 占位、`RequiredPackages("\\mathbb{R}")`
返回 `amssymb`、`\\mathscr` 返回 `mathrsfs`。

---

## 3. WP-00 文档/行为一致性修复（立即）

**对应**：feature-gaps §4.8、C-07、X-07。

| 项 | 动作 | 接口影响 |
| --- | --- | --- |
| README 的 `@` 菜单 | 二选一：实现 `@` 触发引用选择器，或删除 README 条目 | 若实现：`InlineEditor` 新增 `ReferenceMenuRequested()`；`BlockEditor` 连接后打开 `ShowReferencePicker` |
| `CitationTokenActivated` 无人接收 | 在 `BlockEditor` 连接它，打开 picker 并替换 token | 需要 WP-01 的 `CitationAt`/`ReplaceCitationAt` |
| `CrossReferenceTokenActivated` 无人接收 | 同上 | 需要位置参数（新信号带 `position`） |
| `docs/inline-editor.md` 的“可重新选择” | 实现后保留；否则改成“当前限制” | — |

**`@` 菜单的接口草案**（建议实现，符合 README）：

```cpp
// InlineEditor
signals:
    // 用户在空行/空白后输入 @：请求引用/交叉引用选择器。
    // 编辑器不消费字符本身，由宿主决定是否插入。
    void ReferenceMenuRequested(const QPoint& global_pos);

// BlockEditor
connect(editor, &InlineEditor::ReferenceMenuRequested, this,
        [this](InlineEditor* e, const QPoint& pos) { ShowReferencePicker(e, pos); });
```

**测试**：点击 citation pill 打开 picker；`@` 只在行首/空白后触发，普通邮箱文本
不误触发；README 与实现一致。

---

## 4. WP-01 Citation Picker v2（P0，用户明确需求）

### 4.1 目标与非目标

**目标**

1. 选择器行显示：**作者 · 年份 · 标题 · venue · 已引用编号**；
2. 搜索覆盖 title/author/year/venue/key；
3. 已引用条目置顶并显示 `[n]`；
4. 多选生成单个 `Citation{keys}`；
5. Narrative 开关（数字样式显示 `Author [n]`）；
6. 点击正文 citation pill 可重编辑；
7. 修复编辑段落丢失 Narrative mode 的数据丢失。

**非目标**（本 WP 不做）：author-year 样式（WP-10）、locator/prefix/suffix（WP-10
之后）、Zotero 集成（P3）。

### 4.2 领域接口：扩展 `CitationNumberResolver`

不新增“Presenter”类，直接把“编号 + 展示元数据”收敛到现有 resolver，避免两个
并行缓存；保留现有 `Build/Find/FormatPill/numbers` 签名，新增默认参数与视图。

**修改** `numbering/CitationNumberResolver.h`：

```cpp
namespace pf {

enum class CitationStyle {
    NumericFirstUse, // 当前：unsrtnat/IEEEtranN + numbers,sort&compress
    AuthorYear,      // WP-10：plainnat/biblatex author-year
};

struct CitationDisplayInfo {
    int number = 0;        // 数字样式的 1-based 编号；未引用/author-year 为 0
    bool resolved = false; // key 是否在 bibliography 中
    bool cited = false;    // 本文是否引用过
    std::string display;   // 单 key 的默认 pill 文本
    std::string author_label; // "Vaswani et al."
    std::string year;
};

struct CitationView {
    std::string key;
    std::string author_label;
    std::string title;
    std::string venue;
    std::string year;
    std::string entry_type;
    int number = 0;      // 0 = 未引用
    bool cited = false;
    bool known = false;
};

class CitationNumberResolver {
  public:
    static CitationNumberResolver Build(const Document& document,
                                        const BibliographyDatabase& bibliography,
                                        CitationStyle style = CitationStyle::NumericFirstUse);

    std::optional<CitationDisplayInfo> Find(const std::string& key) const;

    // 兼容旧调用；等价于 FormatPill(keys, Parenthetical)。
    std::string FormatPill(const std::vector<std::string>& keys) const;
    // 新增：mode-aware。数字样式下 Narrative → "Author [n]"，Parenthetical → "[n]"；
    // author-year 样式下分别为 "Author (Year)" / "(Author, Year)"。
    std::string FormatPill(const std::vector<std::string>& keys, CitationMode mode) const;

    bool IsCited(const std::string& key) const;
    CitationStyle style() const noexcept { return style_; }
    std::vector<CitationView> Views() const; // 全部已知条目，按 author/year 排序
    const std::map<std::string, int>& numbers() const noexcept { return numbers_; }

  private:
    CitationStyle style_ = CitationStyle::NumericFirstUse;
    std::map<std::string, int> numbers_;
    std::set<std::string> known_keys_;
    std::map<std::string, CitationView> views_; // key -> metadata
};

} // namespace pf
```

实现要点：

- `Build` 遍历 `VisitInlineContent`（含脚注/题注/表格单元格，WP-06 后自动覆盖）
  统计首次引用顺序；`views_` 从 `BibliographyDatabase::Entries()` 填充；
- `FormatPill(keys, mode)` 先按样式计算；数字样式复用现有压缩逻辑；
- 未解析 key 仍显示 `[?]`，不占编号；
- author-year 样式暂可用 `author_label`（第一作者 et al.）+ `year` 输出，
  完整姓名消歧留给 WP-10。

**测试**：现有 `TestCitationNumbering` 全部保持通过；新增 `Narrative` 输出、
`Views()` 的 number/cited、旧 `FormatPill(keys)` 兼容。

### 4.3 GUI 接口：`BlockEditor` 的引用目录

**修改** `app/BlockEditor.h`：

```cpp
// 新增：由 MainWindow 在 documentChanged 后注入；picker 不再依赖 BibEntry。
void SetCitationCatalog(std::vector<pf::CitationView> entries);

signals:
    // 富题注（WP-04）与 citation 重编辑复用。
    void CitationEditRequested(const QString& node_id, int position,
                               const QStringList& keys, bool narrative);
```

`BlockEditor::ShowCitationPicker` 的新签名：

```cpp
struct CitationPickerSeed {
    QStringList keys;       // 重编辑时预选；空 = 新插入
    bool narrative = false;
    int position = -1;      // 重编辑的 token 位置；-1 = 光标位置
};
void ShowCitationPicker(InlineEditor* editor, CitationPickerSeed seed = {});
```

行为：

1. 用 `citation_catalog_` 构建 `PopupList::Item`：
   - `label = author_label + " · " + year`
   - `title = title`（截断）
   - `detail = venue + "  [" + number + "]"`（已引用）或 `venue`
   - `group = cited ? "CITED IN THIS PAPER" : "NOT CITED"`
   - `search = author + title + venue + year + key`
   - `checkable = true`，`checked = seed.keys.contains(key)`
2. `Options{multi_select=true, show_toggle=true, toggle_label="Narrative",
   toggle_initial=seed.narrative, width_px=620, empty_title=...}`；
3. 收到 `chosenMany(payloads, narrative)`：
   - 若 `seed.position >= 0`：`editor->ReplaceCitationAt(seed.position, keys, mode)`；
   - 否则：恢复光标到 `caret`，`editor->InsertCitationObject(keys, mode)`；
   - `CommitInlineRow(editor)`；焦点恢复到 pill 之后。
4. 单选/旧 `chosen` 路径保留，供 `InsertCitationIntoParagraph` 使用。

### 4.4 GUI 接口：`InlineEditor` 的 mode 与重编辑

**修改** `app/InlineEditor.h`：

```cpp
void InsertCitationObject(const QStringList& keys,
                          CitationMode mode = CitationMode::Parenthetical);

struct CitationTokenInfo {
    QStringList keys;
    CitationMode mode = CitationMode::Parenthetical;
    int position = -1;
};
std::optional<CitationTokenInfo> CitationAt(int position) const;
bool ReplaceCitationAt(int position, const QStringList& keys, CitationMode mode);

// 显示时使用 mode-aware 文本。
QString CitationDisplayText(const QStringList& keys, CitationMode mode) const;

signals:
    // 新签名；旧的无位置/无 mode 信号可以删除（无测试依赖），
    // 或保留一个 deprecated 包装直到下一次大版本。
    void CitationTokenActivated(const QStringList& keys, int position, int mode);
```

实现要点：

- `InsertPillObject` 的 payload 用 `EncodeCitationToken`；`display` 用
  `CitationDisplayText(keys, mode)`；
- `ContentInRange`：`DecodeCitationToken` 恢复 keys + mode；
- `SanitizeTokens`：不认识的旧 payload 仍按 Parenthetical 处理；
- `ReplaceCitationAt`：`TokenAt(position)` 校验 kind，更新 char format 的
  payload/display/kind property，`dirty_=true`，`RefreshObjectDisplays()`；
- `Clipboard`：`InlineToRichText` 输出 `[citet:...]`，`InlineFromRichText` 解析。

### 4.5 `MainWindow`/`ProjectController` 接口

**`ProjectController`**：

```cpp
// 现有 CitationNumbers() 保留（数字样式兼容）；内部加缓存。
std::shared_ptr<const pf::CitationNumberResolver> CitationResolver() const;
std::vector<pf::CitationView> CitationViews() const;
```

缓存策略（WP-13）：

```cpp
mutable std::shared_ptr<const pf::CitationNumberResolver> citation_cache_;
mutable ProjectRevision citation_cache_revision_;
mutable std::uint64_t citation_cache_bib_revision_ = 0;
```

当 `document_version`/`bibliography_revision`/模板 citation style 不变时复用。

**`MainWindow`**：

```cpp
void RefreshCitationCatalog(); // 用 controller->CitationViews() 填 BlockEditor/OutlinePanel
```

**`OutlinePanel`**：

```cpp
// 与 picker 共用同一份 CitationView；不再各自拼 "author · year"。
void SetReferences(std::vector<pf::CitationView> references);
```

`RefreshSidePanels()` 调用 `RefreshCitationCatalog()`；`OutlinePanel` 的 References
行显示 `author · year · title`，tooltip 保留标题，双击仍插入单条。

### 4.6 数据流（插入/重编辑）

```text
用户点 Citation / 点击 pill / Outline 双击
  └─ BlockEditor::ShowCitationPicker(editor, seed)
      ├─ 从 citation_catalog_ 构建 Items（含 [n]/cited/title/search）
      ├─ PopupList(Options{multi_select, toggle=Narrative})
      └─ chosenMany(keys, narrative)
          ├─ seed.position >= 0 → InlineEditor::ReplaceCitationAt
          └─ 否则 → InlineEditor::InsertCitationObject(keys, mode)
               └─ CommitInlineRow → ParagraphContentEdited
                    → ProjectController::EditParagraphRich
                    → EditParagraphPayload → EditingSystem → Document
```

### 4.7 迁移/兼容

- `.paper` 中 `citation.mode` 已存在，无需 schema 变更；
- token payload 旧格式 `key,key2` 仍被解码为 Parenthetical；
- `InlineToRichText` 旧 `[cite:...]` 仍被解析；
- `PopupList::chosen` 与 `BlockEditor::SetReferenceItems` 保持兼容；
- `InlineEditor` 旧 `InsertCitationObject({key})` 调用（测试、Outline）继续可用。

### 4.8 测试与验收

- `TestCitationTokenCodec`：`p|`/`n|`/旧格式/空/重复；
- `TestCitationGui` 扩展：
  - 插入 Narrative，保存/重开/Undo/Redo 后 `ContentInRange` 仍为 Narrative；
  - `ReplaceCitationAt` 后 token 数量不增加；
  - pill 文本在数字样式下为 `Author [n]`；
- GUI 测试：picker 行包含 title、`[n]`、CITED/NOT CITED 分组、搜索 title 命中；
- 多选：一个 `Citation` 节点、keys 顺序、`FormatPill` 输出 `[1, 3]`；
- 兼容：旧 `[cite:key]` 富文本加载为 Parenthetical。

### 4.9 工作量与风险

- 工作量：M（约 5–8 人日；PopupList v2 另计）。
- 风险：`PopupList` 的两行 delegate 与现有块菜单共用，需回归 `/` 菜单与交叉
  引用菜单；Narrative 显示依赖 WP-10 的完整作者名，但数字样式下先输出
  `Author [n]` 已可闭环。

---

## 5. WP-02 Math Palette / 命令补全 / 预览一致性（P0）

### 5.1 目标

- 在 `MathEditorDialog` 与公式卡片上提供结构/字体/符号面板；
- 输入 `\` 时命令补全；
- `\mathbb` 等命令在真实 TeX 预览/PDF 中可用；缺包时给出明确诊断；
- 近似回退补常用命令，并把 `exact/note` 暴露给用户；
- 不改文档协议：仍然只提交 LaTeX body。

### 5.2 接口：`MathPaletteWidget`

**新文件** `app/MathPaletteWidget.h`：

```cpp
namespace pf::gui {

class MathPaletteWidget : public QWidget {
    Q_OBJECT

  public:
    explicit MathPaletteWidget(QWidget* parent = nullptr);

    // 目标编辑器；palette 直接在其光标处插入/包裹。
    void SetTarget(QPlainTextEdit* target);
    // 模板已提供的宏包；目录中缺包的按钮置灰并给出 tooltip。
    void SetAvailablePackages(std::vector<std::string> packages);
    void SetCategory(pf::MathCategory category);

  signals:
    // command 是 MathCommandCatalog 中的命令名（不含反斜杠）；
    // palette 自身已经完成插入，信号用于测试/宿主观察。
    void InsertRequested(const QString& command);
};

// 纯函数，便于单测：把 snippet 的 $ 替换为选中文本，或留下光标占位。
QString MakeMathInsertion(const std::string& snippet, const QString& selection);

// 在目标编辑器执行插入/包裹；返回插入的起始位置（测试用）。
int InsertMathCommand(QPlainTextEdit* target, const pf::MathCommand& command);

} // namespace pf::gui
```

实现要点：

- snippet 里的 `$` 是唯一占位符（例如 `"\\frac{$}{$}"`）；
- `hasSelection()`：第一个 `$` 用选区替换，其余 `$` 留空并定位光标；
- 无选区：删除所有 `$`，定位到第一个空参数；
- 按钮分三行（结构/字体/符号），符号用 `QToolButton` + `QMenu` 分组；
- 缺包按钮灰化，tooltip 写 `需要 mathrsfs`，并给“仍要插入”的说明（避免误用）。

### 5.3 接口：命令补全

**新文件** `app/MathCommandCompleter.h`：

```cpp
namespace pf::gui {

class MathCommandCompleter : public QCompleter {
    Q_OBJECT

  public:
    explicit MathCommandCompleter(QObject* parent = nullptr);

    // 监听 target 的 textChanged/光标变化；输入 \ 后弹出。
    void Attach(QPlainTextEdit* target);
    void SetAvailablePackages(std::vector<std::string> packages);

  private:
    void OnTextChanged();
    void InsertCompletion(const QString& command);
    QPlainTextEdit* target_ = nullptr;
};

} // namespace pf::gui
```

行为：

- 输入 `\ma` → 列表 `\mathcal`、`\mathbb`、`\mathfrak`、`\mathscr`、`\mathbf`…；
- 选中后插入 `\command{}` 并把光标放在花括号内；
- 缺包命令显示灰字/警告（例如 `\mathscr` → `需要 mathrsfs`）；
- Esc 关闭、Tab 接受、Enter 接受（不提交对话框）。

### 5.4 接口：`MathValidator` 的宏包诊断

**修改** `math/MathExpression.h` / `MathValidator.h`：

```cpp
struct MathValidation {
    MathState state = MathState::Valid;
    std::string code;
    std::string error;
    std::vector<std::string> required_packages; // 新增
};
```

`MathValidator` 在扫描 `\command` 时用 `MathCommandCatalog::RequiredPackages`
填充；`Validator` 在模板包列表中检查，缺失时报：

```cpp
Diagnostic{
  source = DiagnosticSource::Template,
  severity = DiagnosticSeverity::Error,
  code = "E-MATH-MISSING-PACKAGE",
  message = "\\mathbb requires amssymb; current template does not load it",
  location = DiagnosticLocation::ForNode(equation_node)
}
```

这个诊断让“预览正常但 PDF 失败”在构建前可见。

### 5.5 接口：预览回退与提示

**修改** `app/MathPreviewRenderer.cpp`（实现，无公共签名变化）：

- 补充 `\mathbb`（双线体可用 Unicode 数学字母或字体近似）、`\binom`、
  `\boxed`、`\overbrace`/`\underbrace`、`\overset`/`\underset`、`\substack`、
  `\overrightarrow`、`\begin{array}`、`\begin{split}`、`\pmod`；
- 对仍不支持的命令，`MathRenderResult.note` 必须包含命令名，且 GUI 显示。

**修改** `app/InlineEditor.h`：

```cpp
// 该公式对象最近一次渲染的 note（空 = 精确/无问题）。
QString MathRenderNoteAt(int position) const;
```

实现：在 token 的 char format 增加 `kMathNoteProperty`，`ApplyMathRender` 时写入；
`InlineMathObjectRenderer` 的 tooltip / 右键菜单显示 note。

**修改** `app/BlockEditor.cpp::MakeEquationCard`：

- `refresh` 的响应里读取 `response.result.exact/note`，显示在状态行：
  `"Approximate preview: unsupported command \mathbb"`；
- 增加“使用 TeX 重新渲染”按钮（可选）。

### 5.6 接口：对话框/卡片接入

**修改** `app/MathEditorDialog.h`：

```cpp
class MathEditorDialog : public QDialog {
  public:
    explicit MathEditorDialog(const QString& latex, QWidget* parent = nullptr);
    void SetAvailablePackages(std::vector<std::string> packages);
    void SetPaletteVisible(bool visible);
    MathPaletteWidget* Palette() const { return palette_; }
    // 新增：让调用方拿到最近的校验/宏包需求。
    const pf::MathValidation& Validation() const;
};
```

**修改** `BlockEditor::MakeEquationCard`：嵌入 `MathPaletteWidget`，`SetTarget(source)`；
`ProjectController`/`MainWindow` 不需要新接口。

### 5.7 测试与验收

- `TestMathCatalog`：命令唯一、snippet 占位、包映射；
- `TestMathPalette`（widget）：点击 `\mathbb` 生成 `\mathbb{}` 且光标在括号内；
  选中 `x+1` 点 `\frac` → `\frac{x+1}{}`；
- `TestMathValidator`：`\mathbb` → `required_packages={"amssymb"}`；`\mathscr`
  → `{"mathrsfs"}`；缺包诊断；
- `TestMathPreview`：补的命令至少不再显示字面命令（近似或明确 note）；
- `TestInlineMathGui`：Narrative 无关；公式对象 tooltip 有 note；
- 真实 TeX 回归：新增模板时 `\mathbb{R}`/`\mathfrak{g}` 构建成功；`\mathscr`
  在加 `mathrsfs` 后成功。

### 5.8 工作量与风险

- M–L（约 8–12 人日）。
- 风险：`QCompleter` 与 `QPlainTextEdit` 的光标/`Tab` 键冲突；预览回退补齐
  容易“看起来对、PDF 不对”，必须以 `required_packages` 诊断兜底。

---

## 6. WP-03 交叉引用 + `DocumentNumberer`（P0）

### 6.1 目标

- 选择器覆盖 Section/Subsection/Subsubsection/Figure/Table/Equation；
- 显示与 PDF 一致的编号；
- 胶囊显示编号；
- 支持自定义 label 与重复校验；
- 点击交叉引用可重选。

### 6.2 接口：`DocumentNumberer`

**新文件** `numbering/DocumentNumberer.h`（纯 C++）：

```cpp
namespace pf {

struct NumberedNode {
    NodeId node;
    NodeKind kind = NodeKind::Paragraph;
    int number = 0;              // 0 = 不编号（例如 unnumbered equation）
    std::string kind_label;      // "Section" / "Figure" / "Table" / "Equation"
    std::string display;         // "Section 2.1" / "Figure 3" / "Table 1" / "Eq. (4)"
    std::string title;           // 标题/题注的纯文本
    std::string latex_label;     // 用户 label 或 node id
};

struct NumberingOptions {
    bool number_sections = true;
    bool number_figures = true;
    bool number_tables = true;
    bool number_equations = true;
};

class DocumentNumberer {
  public:
    static DocumentNumberer Build(const Document& document,
                                  const NumberingOptions& options = {});

    std::optional<NumberedNode> Find(const NodeId& node) const;
    const std::map<NodeId, NumberedNode>& entries() const;
    std::vector<NumberedNode> ByKind(NodeKind kind) const;
    std::string DisplayFor(const NodeId& node) const; // "" if not numbered
};

} // namespace pf
```

编号规则（与 LaTeX 默认一致）：

- Section 1、Subsection 1.2、Subsubsection 1.2.3；
- Figure/Table/Equation 全局递增（不按 section）；unnumbered equation 不占号；
- 双栏 Figure/`figure*` 不影响编号；
- appendix（WP-08）以后增加 `A.1` 模式；接口预留 `NumberingOptions`。

### 6.3 接口：`CrossReferenceItem` 与 `BlockEditor`

```cpp
// numbering/DocumentNumberer.h 或单独 CrossReference.h
struct CrossReferenceItem {
    NodeId node;
    NodeKind kind;
    std::string display; // "Section 2.1" / "Figure 3"
    std::string title;
};

std::vector<CrossReferenceItem> CollectCrossReferences(const Document&,
                                                       const DocumentNumberer&);
```

`BlockEditor`：

```cpp
void SetCrossReferences(std::vector<pf::CrossReferenceItem> items);
void SetNumbering(std::shared_ptr<const pf::DocumentNumberer> numbering);
```

`ShowReferencePicker` 改为按 `kind` 分组（Sections/Math/Figures/Tables），
`label = display + "  " + title`，`search` 包含 title；选中仍发 `xref:<node>`。

边界规则：

- **未编号公式**没有可被 `\ref` 的编号（渲染器也不写 `\label`），因此不进入
  选择器，或置灰并提示“先勾选 Numbered”；
- **表格**即使题注为空也要写 `\label`（或拒绝引用），避免生成悬空 `\ref`；
- 列表/代码/定理在对应的 WP 落地后追加到分组（定理/代码见 WP-07）。

### 6.4 接口：胶囊显示与重选

`InlineEditor`：

```cpp
void SetCrossReferenceLabels(std::map<QString, QString> labels); // 已有
bool ReplaceCrossReferenceAt(int position, const NodeId& target);
signals:
    void CrossReferenceTokenActivated(const QString& target, int position);
```

`ProjectController`：

```cpp
std::shared_ptr<const pf::DocumentNumberer> Numbering() const; // WP-13 缓存
```

`BlockEditor` 的 picker 增加重编辑种子：

```cpp
struct ReferencePickerSeed {
    QString target;   // 空 = 新建
    int position = -1;
};
void ShowReferencePicker(InlineEditor* editor, ReferencePickerSeed seed = {});
```

`MainWindow::RefreshCrossReferences()`：

```cpp
auto numbering = controller_->Numbering();
auto items = CollectCrossReferences(doc, *numbering);
editor_->SetNumbering(numbering);
editor_->SetCrossReferences(items);
editor_->SetCrossReferenceLabels( /* node id -> numbering->DisplayFor(node) */ );
```

重编辑：`BlockEditor` 连接 `CrossReferenceTokenActivated(target, position)` →
`ShowReferencePicker(editor, {.target=target, .position=position})`；确认后
`InlineEditor::ReplaceCrossReferenceAt(position, new_target)`。

### 6.5 渲染：编号与 `\ref`/`\cref`

**修改** `render/LatexPlanner.cpp`：

- 统一 `LabelFor` 从 `DocumentNumberer`/`LabelRegistry` 获取；
- Figure/Table 增加 `label` 字段（见 §7.1），Equation 保持；
- 交叉引用生成：

```cpp
switch (def.capabilities.cross_reference) {
case CrossReferenceStyle::Ref:      Cmd("ref",  {Id(label)});
case CrossReferenceStyle::Cleveref: Cmd("cref", {Id(label)});
}
```

**修改** `template/TemplateRegistry.h`：

```cpp
enum class CrossReferenceStyle { Ref, Cleveref };

struct TemplateCapabilities {
    int max_heading_depth = 3;
    CrossReferenceStyle cross_reference = CrossReferenceStyle::Ref;
};
```

模板加载 `cleveref` 时设置 `Cleveref`；默认 `Ref`，保持向后兼容。
`cleveref` 需在 `hyperref` 之后加载（planner 已有 hyperref 排序逻辑，需扩展）。

### 6.6 重复 label 诊断

`Validator` 新增：

```cpp
Diagnostic{
  code = "E-DUPLICATE-LABEL",
  severity = Error,
  location = ForNode(second_node),
  message = "label 'fig:arch' is already used by ..."
}
```

`LabelRegistry` 在 planner 与 validator 共用；若重复，planner 抛
`std::runtime_error`（已被 BuildCoordinator 的异常边界转成 InternalError，
见审查报告 F-01），Validator 提前报错避免走到这里。

### 6.7 测试

- `TestDocumentNumberer`：三级标题、图/表/公式计数、unnumbered、跨 subsection；
- `TestStructure`/`TestCitationGui`：交叉引用到 Equation/Table/Subsection；
- `TestReferenceGui`：picker 分组、编号显示、点击重选；
- 真实 TeX：`\cref` 输出 “Figure 3”/“Eq. (2)”。

### 6.8 工作量与风险

- M–L（6–10 人日）。风险：编号规则必须与模板/LaTeX 一致；`cleveref` 与
  `hyperref` 加载顺序；模板切换后编号可能变化（重新计算即可）。

---

## 7. WP-04 富文本题注 + 表格编辑（P0）

### 7.1 数据模型（先做字段，再做 UI）

**修改** `document/Document.h`：

```cpp
struct Figure {
    NodeId id;
    AssetId asset_id;
    InlineContent caption;
    std::string alt_text;
    FigureWidth width = FigureWidth::Percent100;
    FigureSpan span = FigureSpan::SingleColumn;
    std::string label; // 新增：用户可读 label；空 -> node id
};

struct Table {
    NodeId id;
    InlineContent caption;
    bool has_header_row = false;
    std::vector<TableColumn> columns;
    std::vector<std::vector<TableCell>> cells;
    std::string label; // 新增
    // invariants unchanged
};
```

`Figure::label`/`Table::label` 参与 `LabelFor` 与 `DocumentNumberer`。空 label
回退 node id（与 Equation 一致）。

### 7.2 命令载荷

**修改** `editing/EditCommand.h`：

```cpp
struct EditFigureLabelPayload { NodeId figure; std::string label; };
struct EditTableLabelPayload  { NodeId table;  std::string label; };
struct EditFigureWidthPayload { NodeId figure; FigureWidth width = FigureWidth::Percent100; };
struct SetFigureAssetPayload  { NodeId figure; AssetId asset_id; };
struct SetTableCellPayload    { NodeId table; size_t row = 0; size_t column = 0;
                                InlineContent content; };
struct InsertTableRowPayload  { NodeId table; size_t index = 0; };
struct DeleteTableRowPayload  { NodeId table; size_t index = 0; };
struct InsertTableColumnPayload { NodeId table; size_t index = 0;
                                  ColumnAlignment alignment = ColumnAlignment::Left; };
struct DeleteTableColumnPayload { NodeId table; size_t index = 0; };
struct SetTableColumnAlignmentPayload { NodeId table; size_t column = 0;
                                        ColumnAlignment alignment = ColumnAlignment::Left; };
```

追加到 `EditPayload`/`FullEditPayload` 末尾；`EditingSystem::ApplyDocumentPayload`
增加分派；`DocumentEditor` 增加对应方法（部分已存在：`SetTableCell`、
`InsertTableRow` 等，只需补 label/width/asset）。

### 7.3 `BlockEditor` 接口

```cpp
signals:
    // 题注富内容提交（替代旧的 CaptionEdited(QString)；
    // 旧信号保留到迁移完成）。
    void CaptionContentEdited(const QString& node_id, const InlineContent& content);
    // 表格单元格/行列/对齐。
    void TableCellEdited(const QString& table_id, int row, int column,
                         const InlineContent& content);
    void TableRowInsertRequested(const QString& table_id, int index);
    void TableRowDeleteRequested(const QString& table_id, int index);
    void TableColumnInsertRequested(const QString& table_id, int index);
    void TableColumnDeleteRequested(const QString& table_id, int index);
    void TableColumnAlignmentChanged(const QString& table_id, int column,
                                     pf::ColumnAlignment alignment);
    void FigureWidthChanged(const QString& figure_id, pf::FigureWidth width);
    void FigureLabelEdited(const QString& figure_id, const QString& label);
    void TableLabelEdited(const QString& table_id, const QString& label);
    void FigureAssetReplaceRequested(const QString& figure_id);
```

实现要点：

- `MakeFigureCard`/`MakeTableCard` 的题注从 `NewEditor`（`BlockEdit`）改为
  `InlineEditor`，使用 `theme::BlockVisualRole::Caption` 字体；
- `Block` 结构增加 `InlineEditor* rich_editor` 与 `rich_role`，或在
  `CommitBlock` 中根据 `commit_role` 分支：`paragraph` → `ParagraphContentEdited`，
  `caption` → `CaptionContentEdited`；
- 表格 `QTableWidget` 改为可交互：双击单元格 → `InlineContentEditorDialog`；
  行列/对齐用卡片工具栏；
- 图片宽度用 `QComboBox`（25/50/75/100%）；
- label 用 `QLineEdit`，失焦提交。

### 7.4 `ProjectController`/`MainWindow` 接口

```cpp
EditResult EditCaptionRich(const NodeId& block, const InlineContent& caption);
EditResult SetTableCell(const NodeId& table, size_t row, size_t column,
                        const InlineContent& content);
EditResult InsertTableRow(...); DeleteTableRow(...);
EditResult InsertTableColumn(...); DeleteTableColumn(...);
EditResult SetTableColumnAlignment(...);
EditResult EditFigureWidth(...); EditFigureLabel(...); EditTableLabel(...);
EditResult ReplaceFigureAsset(const NodeId& figure, const std::filesystem::path& source);
```

`MainWindow::WireEditor` 连接新信号；失败时状态栏显示 `result.detail`。

**题注的持久化**：`project.paper` 的 `caption` 本来就是 `InlineContent`，无需
schema 变更；`EditCaptionPayload` 已经是 `InlineContent`，只是 GUI 以前用
`InlineFromText` 构造。因此 WP-04 的题注部分**不需要 schema 升级**。

### 7.5 表格单元格编辑器的选择

两种实现二选一：

- **A（推荐，改动小）**：双击单元格打开 `InlineContentEditorDialog`，确认后
  `SetTableCellPayload`；表格网格保持只读预览。
- **B（体验更好，工作量大）**：每个单元格内嵌 `InlineEditor`（虚拟化困难，
  100 行×8 列会创建大量控件）。

本 WP 先做 A；B 放到 WP-08。

### 7.6 测试

- `TestCaptionRich`：题注含 `\mathcal{L}` + citation，保存/重开/Undo 后仍保留；
- `TestTableEditing`：单元格编辑、增删行列、对齐、label 往返；
- GUI：双击单元格弹窗、题注 InlineEditor 不触发 focusOut 重建；
- 真实 TeX：题注中的数学/引用进入 PDF；表格 label 的 `\ref` 正常。

### 7.7 工作量与风险

- L（10–15 人日）。风险：题注编辑器与正文行共享 `InlineEditor` 的提交/焦点
  逻辑；表格弹窗的焦点往返需 `BeginProtectedInsert`；Undo 快照会变大（已有）。

### 7.8 实施记录（2026-10）

- 题注改用 `InlineEditor`；`NewInlineEditor` 统一初始化排版、数学宏包、引用
  目录和格式动作，`CommitBlock` 按 `commit_role` 区分正文与题注。保留旧
  `CaptionEdited`/`EditCaption` 接口，GUI 新路径使用 `CaptionContentEdited` /
  `EditCaptionRich`，不再经过纯文本转换。
- 单元格采用方案 A：`InlineContentEditorDialog` 维护独立草稿；确认后发送
  `SetTableCellPayload`，取消不产生 revision。数学编辑器与引用选择器复用正文
  入口，选择器挂到单元格弹窗下；文档通知不会拆除仍在编辑的卡片。
- 表格命令已追加到 `EditPayload` / `FullEditPayload`。另增
  `SetTableHeaderRowPayload`，支持插入后切换表头；行列操作保留当前选择，执行前
  提交尚未失焦的题注。不能删除最后一列。
- `InsertTableDialog` 提供尺寸/表头选择，原 `InsertTableAfter(anchor)` 保留 3×3
  默认值。模型、编辑器、GUI 和持久化共用 100 行 / 50 列上限，避免创建无法保存
  或重新加载的表格。图片宽度选择使用现有 `FigureWidth` 字段和新的可撤销命令。
- 保存校验接受章节/子节/三级标题作为交叉引用目标，并通过 `VisitInlineContent`
  检查各层题注、单元格和正文。Validator 同时检查单元格的数学宏包及引用 key。
- 回归覆盖见 `TestTableEditing.cpp`、`TestCaptionTableGui.cpp`，含保存重开、
  Undo/Redo、取消、弹窗期间文档变化，以及真实 TeX 构建。
- 合并单元格、逐单元格内嵌编辑、子图仍属于 WP-08；图片替换与矢量格式仍按
  WP-05 推进。

---

## 8. WP-05 图片控制 + 矢量图（P1）

### 8.1 目标

- 图片宽度/ label/替换/拖放/粘贴；
- 支持 PDF 矢量图；SVG/EPS 通过转换器；
- 不破坏现有 `AssetId`/持久化。

### 8.2 `AssetManager` 接口扩展

**修改** `asset/AssetManager.h`：

```cpp
enum class AssetKind : std::uint8_t {
    Raster,  // png/jpeg/gif
    Pdf,     // application/pdf（pdfLaTeX 原生支持）
    Svg,     // image/svg+xml（需转换）
    Eps,     // application/postscript（需转换）
};

struct AssetMetadata {
    // ... 现有字段 ...
    AssetKind kind = AssetKind::Raster;
    bool IsVector() const noexcept { return kind != AssetKind::Raster; }
};

// 可选转换器：SVG/EPS -> PDF（或 PNG）。默认空 = 拒绝 SVG/EPS。
class IAssetConverter {
  public:
    virtual ~IAssetConverter() = default;
    // 返回转换后的文件路径；失败时写 error。
    virtual std::optional<std::filesystem::path>
    Convert(const std::filesystem::path& source,
            const std::filesystem::path& work_dir,
            std::string* error) = 0;
};

class AssetManager {
  public:
    explicit AssetManager(std::filesystem::path assets_dir,
                          std::shared_ptr<IAssetConverter> converter = nullptr);
    // 现有 Stage；新增 options 默认值保持兼容。
    AssetImportResult Stage(const AssetImportRequest& request,
                            bool allow_vector = true);
};
```

实现：

- `DetectMediaType` 已识别 PDF/SVG/EPS；`Stage` 不再一律拒绝；
- PDF：直接复制，`width/height` 先填 0（或通过 `pdfinfo` 探测；探测失败不阻塞）；
- SVG/EPS：若 `converter` 非空，转换后再存；否则 `UnsupportedType`，detail
  说明“需要 rsvg-convert/epstopdf”；
- `metadata.relative_path` 指向最终 LaTeX 可用的文件（PDF/PNG）；
- 原始文件是否保留：可选 `original/` 子目录，默认不保留。

**app 侧转换器** `app/AssetConverters.{h,cpp}`：用 `QProcess` 调
`rsvg-convert -f pdf` / `epstopdf`，超时 15s；这是 GUI/平台能力，不放进 domain。

### 8.3 图片插入/拖放接口

`BlockEditor`：

```cpp
signals:
    void AssetFileDropped(const QString& path, const QString& after_node);
    void AssetPasted(const QString& temp_path, const QString& after_node);
```

实现 `BlockEditor::eventFilter`/`dragEnterEvent`（当前只在 `GapWidget` 处理
块重排 MIME）：

- `QMimeData::hasUrls()` 且扩展名属于图片 → 发 `AssetFileDropped`；
- `hasImage()` → 保存临时 PNG → `AssetPasted`；
- 目标锚点由鼠标位置决定（`Gap` 的 anchor 或卡片 node id）。

`MainWindow`：

```cpp
connect(editor_, &BlockEditor::AssetFileDropped, this,
        [this](QString path, QString after) {
            controller_->InsertFigureAfter(NodeId(after.toStdString()), path);
        });
```

### 8.4 图片控件接口

`MakeFigureCard` 增加：

- `QComboBox` 宽度（25/50/75/100%）→ `FigureWidthChanged`；
- `QLineEdit` label → `FigureLabelEdited`；
- `QPushButton("Replace image…")` → `FigureAssetReplaceRequested`；
- `QCheckBox` span 已有；
- PDF 图片预览：`FigurePreviewResolver`：

```cpp
using FigurePreviewResolver =
    std::function<QPixmap(const AssetMetadata&, const QSize& target)>;
void SetFigurePreviewResolver(FigurePreviewResolver);
```

PDF 预览用 `pdftoppm` 第一页（可复用 `PdfPreview` 的 helper，或 `QPdfDocument`
若 Qt6::Pdf 可用）；失败显示占位符。

### 8.5 测试

- `TestAssetVector`：PDF 被接受、metadata.kind=Pdf；无 converter 时 SVG/EPS
  被拒绝且 detail 明确；
- GUI：拖入 PNG/PDF 生成 Figure；宽度/ label 往返；
- 真实 TeX：PDF 图片经 graphicx 构建成功，PDF 内含该图。

### 8.6 工作量与风险

- M–L（6–10 人日）。风险：SVG 转换依赖外部工具，需在 UI 中明确安装提示；
  PDF 尺寸探测可选，缺失时预览用固定比例。

### 8.7 实施记录（2026-10）

- `AssetKind`、`IAssetConverter` 和 `Stage(request, allow_vector=true)` 已实现。
  PDF 直接复制；SVG/EPS 通过 `ProjectSession::Config::asset_converter` 注入转换器，
  `ProcessAssetConverter` 在 app 层调用 `rsvg-convert` / `epstopdf`，15 秒超时。
  转换工作目录在成功和失败时均清理，资源路径指向最终 PDF/位图。
- `kind` 表示最终存储格式，转换后的 SVG/EPS 通常为 `Pdf`；原始文件名另行保留。
  `kind` 可从既有 `mediaType` 推导，加载旧 v4 文件时重建，JSON 无新增必填字段。
- `ReplaceFigureFromSource` / `ReplaceFigureAsset` 通过新增的
  `SetFigureAssetPayload` 更新图片引用，保留节点及全部题注/布局属性。资源仅在
  命令成功后注册、构建快照捕获前提交；失败移除暂存文件。旧资源保留供撤销。
- `BlockEditor` 的图片输入过滤器覆盖卡片、间隙和子编辑器视口；本地文件和剪贴板
  图像在事件结束后插入，避免销毁正在接收事件的控件。多文件保持顺序，插入前
  提交当前输入，临时 PNG 在同步导入结束后删除。模态草稿弹窗保留自身粘贴行为。
- Figure 卡片已有宽度/标签/跨栏控件，并增加 `Replace image…`。预览 resolver
  使用 `AssetId` 与目标尺寸，由 MainWindow 校验项目路径后交给
  `FigurePreviewRenderer`；保持资源查找集中在应用层。PDF 首页通过异步
  `pdftoppm` 渲染，限制分辨率/缓存/并发/时间，仅刷新对应图片，缺工具显示占位符。
- 验证见 `TestAssetVector.cpp` / `TestFigureGui.cpp`：导入和转换失败清理、拖放/
  粘贴、替换对话框、Undo/Redo、保存重开、异步缩略图和真实 graphicx 构建。
  裁剪、浮动位置控制与子图不在本工作包范围。

---

## 9. WP-06 列表 + 脚注（schema v5，P1）

### 9.1 目标

- 真正的 `itemize`/`enumerate`（v1 不支持嵌套）；
- 真正的 `\footnote`，脚注内可含公式/引用/交叉引用；
- schema v5 + V4→V5 迁移；
- 完整的遍历/渲染/校验/编辑/撤销链路。

### 9.2 模型

**修改** `document/Document.h`：

```cpp
enum class ListStyle : std::uint8_t { Bullet, Numbered };

struct ListItem {
    NodeId id;
    InlineContent content;
};

struct ListBlock {
    NodeId id;
    ListStyle style = ListStyle::Bullet;
    std::vector<ListItem> items;
};

// 脚注采用“行内引用 + 共享 body”的模型，避免递归 variant 的完整类型问题。
struct FootnoteBody; // 定义在 InlineNode 之后
struct Footnote {
    std::shared_ptr<const FootnoteBody> body;
    bool operator==(const Footnote& other) const; // 定义在 Document.cpp
};

using InlineNode = std::variant<TextRun, InlineMath, Citation,
                                CrossReference, Footnote>;
using InlineContent = std::vector<InlineNode>;

struct FootnoteBody {
    InlineContent content;
};

enum class NodeKind : std::uint8_t {
    Section, Subsection, Subsubsection,
    Paragraph, Figure, Table, Equation, List,
};

using Block = std::variant<Paragraph, Figure, Table, EquationBlock, ListBlock>;
```

要点：

- `Footnote` 用 `shared_ptr<const FootnoteBody>`，`InlineNode` 可以完整实例化；
- 脚注内容可以继续包含脚注（不做嵌套限制的话会递归）；Validator 限制
  `depth <= 1`，或 UI 不提供嵌套入口；
- `VisitInlineContent` 必须递归访问脚注 body（顺序=引用点），这样
  `CitationNumberResolver` 会统计脚注里的引用；
- 复制/粘贴：脚注 body 是 shared，粘贴后共享同一内容；编辑器 token 用新 id
  重新映射（见 9.4）。

### 9.3 命令载荷与 DocumentEditor

```cpp
struct InsertListPayload {
    NodeId parent;
    std::optional<size_t> index;
    ListStyle style = ListStyle::Bullet;
    InlineContent first_item;
};
struct InsertListItemPayload { NodeId list; size_t index = 0; InlineContent content; };
struct EditListItemPayload   { NodeId list; size_t index = 0; InlineContent content; };
struct DeleteListItemPayload { NodeId list; size_t index = 0; };
struct MoveListItemPayload   { NodeId list; size_t from = 0; size_t to = 0; };
struct SetListStylePayload   { NodeId list; ListStyle style = ListStyle::Bullet; };

struct InsertFootnotePayload {
    NodeId paragraph;
    std::optional<size_t> at_index; // 行内位置
    InlineContent body;
};
struct EditFootnotePayload {
    NodeId paragraph;   // 脚注所属段落（按引用位置定位）
    InlineContent body;
};
struct DeleteFootnotePayload { NodeId paragraph; size_t at_index; };
```

`DocumentEditor`：

```cpp
Result<NodeId, EditError> InsertList(const NodeId& parent, std::optional<size_t> index,
                                     ListStyle style, InlineContent first_item,
                                     NodeId id = {});
Result<NodeId, EditError> InsertListItem(const NodeId& list, size_t index,
                                         InlineContent content, NodeId id = {});
Result<void, EditError> SetListItemContent(const NodeId& list, size_t index,
                                           const InlineContent& content);
Result<void, EditError> DeleteListItem(const NodeId& list, size_t index);
Result<void, EditError> MoveListItem(const NodeId& list, size_t from, size_t to);
Result<void, EditError> SetListStyle(const NodeId& list, ListStyle style);

Result<void, EditError> InsertFootnote(const NodeId& paragraph,
                                       std::optional<size_t> at_index,
                                       InlineContent body);
Result<void, EditError> SetFootnote(const NodeId& paragraph, size_t at_index,
                                    const InlineContent& body);
Result<void, EditError> DeleteFootnote(const NodeId& paragraph, size_t at_index);
```

脚注的“定位”以段落内的引用顺序为准（第 n 个 Footnote 节点），因为 paragraph
内没有稳定 item id；这也与 `InlineEditor` 的 token 顺序一致。

### 9.4 `InlineEditor` 脚注接口

```cpp
// 插入一个脚注引用；body 由调用方提供（通常来自 InlineContentEditorDialog）。
void InsertFootnoteObject(const InlineContent& body);
struct FootnoteTokenInfo {
    InlineContent body;
    int position = -1;
};
std::optional<FootnoteTokenInfo> FootnoteAt(int position) const;
bool ReplaceFootnoteAt(int position, const InlineContent& body);

signals:
    void FootnoteTokenActivated(int position);
```

实现：

- `InlineEditor` 维护 `std::map<QString, InlineContent> footnote_bodies_`，
  token payload 是内部 id（例如 `"fn:3"`），避免把嵌套富文本塞进 char format；
- `SetContent` 遍历 Footnote 节点，为每个 body 分配 id 并登记；
- `Content` 按 token 的 id 取回 body；
- 双击/单击脚注 token → 打开 `InlineContentEditorDialog` 编辑 body。

### 9.5 渲染（LaTeXPlanner）

```cpp
// VisitInlineContent / Inline(content) 增加 Footnote 分支：
if (const auto* fn = std::get_if<Footnote>(&node)) {
    return Cmd("footnote", {Inline(fn->body->content)});
}
```

列表：

```cpp
if (const auto* list = std::get_if<ListBlock>(&block)) {
    Nodes items;
    for (const auto& item : list->items) {
        items.push_back(Cmd("item", Inline(item.content)));
        items.push_back(Lit("\n"));
    }
    auto env = Env(list->style == ListStyle::Bullet ? "itemize" : "enumerate",
                   std::move(items));
    ...
}
```

### 9.6 剪贴板与脚注

现有 `InlineToRichText` / `InlineFromRichText` 是“可读文本编码”，用于渲染纯文本
与旧路径；脚注 body 不适合塞进这个文本格式。建议新增版本化的剪贴板编解码：

**修改** `document/InlineText.h`：

```cpp
// 用于 application/x-strlatex-inline;v=2 的 round-trip 编码。
// 与 InlineToRichText 不同，它保留 Footnote 的 body。
std::string InlineToClipboard(const InlineContent& content);
std::optional<InlineContent> InlineFromClipboard(const std::string& text);
```

`InlineEditor::createMimeDataFromSelection` / `insertFromMimeData` 优先使用
v2；`InlineToRichText` 只用于 PlainText/搜索，遇到 Footnote 输出 `[^n]` 占位。
这样复制/粘贴不会丢脚注，也不会把脚注内容压成普通文本。

### 9.7 持久化（v5）

```json
{
  "type": "paragraph",
  "id": "n12",
  "content": [
    {"type": "text", "text": "See "},
    {"type": "footnote", "body": [
      {"type": "text", "text": "A note with "},
      {"type": "inline_math", "latex": "\\alpha"}
    ]}
  ]
}
```

列表：

```json
{"type":"list","id":"n20","style":"bullet","items":[
  {"id":"n21","content":[{"type":"text","text":"First"}]},
  {"id":"n22","content":[{"type":"text","text":"Second"}]}
]}
```

迁移 V4→V5：只加版本戳；旧文档没有新类型，无需转换。读者对未知块类型返回
结构化错误（现有行为）。schema v4 已用于 Figure/Table label，因此列表/脚注使用 v5。

### 9.8 测试

- `TestListBlock`：插入/编辑/删除/移动/撤销；渲染 `itemize/enumerate`；
- `TestFootnote`：插入/编辑/删除；脚注中的 citation 编号；嵌套限制；
- `TestPersistence`：v4 round-trip；V3 文件迁移后保存再读；
- GUI：脚注弹窗、列表项编辑、Enter 新建 item；
- 真实 TeX：列表与脚注进入 PDF。

### 9.9 工作量与风险

- L（12–18 人日）。风险：`Footnote` 的递归/共享语义；剪贴板与 Undo 的快照
  一致性；列表与现有 `Block` variant 的遍历/定位全面改造（编译器会帮你找）。

### 9.10 实施记录（WP-06）

- 模型：`ListStyle`/`ListItem`/`ListBlock`、`Footnote`/`FootnoteBody` 与
  `NodeKind::List`；`Footnote` 用 `shared_ptr<const FootnoteBody>`，相等性按
  body 内容比较（`document/Document.cpp`）。
- 遍历：`VisitInlineContent` 递归脚注 body，列表项参与引用编号/校验；
  `VisitBlocks`/`VisitNodes` 覆盖 `ListBlock`。
- 编辑：`DocumentEditor` + `EditCommand` + `EditingSystem` 增加列表六条命令与
  脚注三条命令；History 继续使用整篇 snapshot，Undo/Redo 无需特殊分支。
- 渲染：`egin{itemize}`/`egin{enumerate}` + `\item `；脚注渲染为
  `ootnote{...}`，body 内的公式/引用/交叉引用走同一个 `Inline()`。
- 持久化：schema **v5** 写入 `{"type":"list","style":"bullet|numbered",
  "items":[...]}` 与 `{"type":"footnote","body":[...]}`；V4→V5 只加版本戳。
  行内剪贴板使用 `InlineToClipboard`/`InlineFromClipboard`（`PFINLINE2`），
  保留脚注 body。
- 校验：脚注嵌套报 `E-FOOTNOTE-NESTED`；脚注 body 内的 citation/数学继续由
  现有 Validator 覆盖。
- UI：`BlockEditor::MakeListCard`（条目编辑、上下移动、增删、样式切换）、
  `MakeFootnote` 工具按钮 + `InlineContentEditorDialog` 脚注正文弹窗；
  `InlineEditor` 的 footnote token 支持双击编辑与复制/粘贴。
- 测试：`TestListFootnote`（模型/编辑/渲染/校验/剪贴板/持久化）、
  `TestInlineMathGui` 的列表卡片与脚注弹窗/往返、真实 tectonic 构建包含
  列表与脚注；`TestStructure` 覆盖 V4→V5。

---

## 10. WP-07 定理/证明 + 代码块（P1，已实现）

### 10.1 模型

```cpp
enum class TheoremKind : std::uint8_t {
    Theorem, Lemma, Proposition, Corollary, Definition, Proof, Remark, Example
};

struct TheoremBlock {
    NodeId id;
    TheoremKind kind = TheoremKind::Theorem;
    InlineContent title;   // 可选，例如 "Main result"
    InlineContent body;    // v1 单段；多段在 WP-08 扩展
    bool numbered = true;  // Proof 默认 false
    std::string label;
};

struct CodeBlock {
    NodeId id;
    std::string language;      // "python"/"c++"/...
    std::string code;          // 原文，不转义
    InlineContent caption;     // 可选
    bool line_numbers = true;
    std::string label;
};
```

`Block` 追加两个 alternative；`NodeKind` 追加 `Theorem`/`Code`。

### 10.2 模板能力

`TemplateDefinition` 新增：

```cpp
struct TheoremEnvironmentDecl {
    TheoremKind kind = TheoremKind::Theorem;
    std::string latex_name = "theorem";
    bool numbered = true;
};
struct CodeListingDecl {
    std::string package = "listings"; // 或 "minted"
    std::string options;              // 例如 "numbers=left"
};

std::vector<TheoremEnvironmentDecl> theorem_environments;
CodeListingDecl code_listing;
```

内置模板注册默认 `amsthm` 环境；`minted` 需要 shell-escape，不默认启用，
默认用 `listings`（无需 shell-escape）。

> 依赖注意：`TheoremEnvironmentDecl` 引用 `TheoremKind`，`TemplateRegistry.h`
> 需要 include `document/Document.h`（`pf_template` 已依赖 `pf_document`，无环）。
> 若不希望模板依赖文档模型，可把字段改成 `std::string kind` 并在 planner 里
> 映射；本方案选择强类型枚举。
`TemplateCapabilities` 增加：

```cpp
bool theorems = false;
bool code_blocks = false;
```

### 10.3 命令载荷

```cpp
struct InsertTheoremPayload {
    NodeId parent;
    std::optional<size_t> index;
    TheoremKind kind = TheoremKind::Theorem;
    InlineContent title;
    InlineContent body;
    bool numbered = true;
    std::string label;
};
struct EditTheoremPayload { NodeId theorem; InlineContent body;
                            std::optional<InlineContent> title;
                            std::optional<bool> numbered; std::optional<std::string> label; };
struct InsertCodePayload { NodeId parent; std::optional<size_t> index;
                           std::string language; std::string code;
                           InlineContent caption; bool line_numbers = true; std::string label; };
struct EditCodePayload { NodeId code; std::optional<std::string> language;
                         std::optional<std::string> text; std::optional<InlineContent> caption; };
```

### 10.4 渲染

定理：

```text
\begin{theorem}[title]\label{thm:main}
... Inline(body) ...
\end{theorem}
```

模板导言区由 `theorem_environments` 生成 `\newtheorem{theorem}{Theorem}` 等；
`proof` 环境不需要 `\newtheorem`，使用 `\begin{proof}`。

代码：

```text
\begin{lstlisting}[language=Python,numbers=left,caption={...},label={lst:1}]
<code as-is>
\end{lstlisting}
```

安全：代码文本中若含 `\end{lstlisting}`，Validator 报 `E-CODE-END-MARKER`；
`language` 必须在白名单映射内，未知语言降级为 `[language={}]` 并警告。

### 10.5 GUI

- 插入菜单新增 Theorem/Proof/Code；
- 定理卡片：kind 下拉、title、body（InlineEditor）、numbered、label；
- 代码卡片：语言下拉、`QPlainTextEdit`（等宽、无富文本）、行号开关、题注
  （InlineEditor）、label；
- 代码块的正文提交要绕过 `ReflowHardWrappedText`。

### 10.6 测试

- 模型/命令/撤销/持久化；
- 渲染字符串断言；
- 真实 TeX：定理编号/证明/代码块进入 PDF；
- 代码块 `\end{lstlisting}` 注入被拒绝。

### 10.7 工作量与风险

- L（10–15 人日）。风险：定理环境需要模板导言区注入机制；minted 的
  shell-escape 安全边界；代码字体与 PDF 字体。

### 10.8 实施记录（WP-07）

- 模型：`TheoremKind`、`TheoremBlock`、`CodeBlock`；`Block`/`NodeKind`/traversal/
  persistence 全覆盖。
- 模板：`TheoremEnvironmentDecl`、`CodeListingDecl`、`TemplateCapabilities::
  theorems/code_blocks`；内置模板声明 amsthm 环境。自带 runtime 没有
  `listings.sty`，因此内置模板的代码块使用 `verbatim` + `\paperforgeListing`
  计数宏；外部模板可显式声明 `listings`，planner 会输出 `lstlisting`。
- 渲染：定理/引理/命题等按模板环境输出，Proof 使用 `proof`；未编号定理使用
  `theorem*`；代码块保留原文，fallback 支持行号前缀、Listing 编号与 `\ref` label。
- 校验：`E-CODE-END-MARKER` 拦截 `\end{lstlisting}`/`\end{verbatim}`；
  未知语言报 `W-CODE-LANGUAGE` 并由 planner 安全省略语言选项。
- GUI：插入菜单 Theorem/Proof/Code；定理卡片 kind/title/body/numbered/label；
  代码卡片语言、代码、行号、caption、label；代码正文不做 reflow。
- 测试：`TestTheoremCode`、GUI `TheoremAndCodeCardsProjectAndEdit`、真实
  tectonic 构建；模板/编号/label/picker 同步支持 Theorem/Listing。

---

## 11. WP-08 子图 + 表格高级能力（P2）

### 11.1 子图模型

```cpp
struct SubFigure {
    NodeId id;
    AssetId asset_id;
    InlineContent caption;
    std::string label;
};

struct Figure {
    // ... existing ...
    std::vector<SubFigure> subfigures; // 空 = 普通单图
};
```

迁移：旧 `Figure` 的 `asset_id/caption` 保持不变；当 `subfigures` 非空时，
渲染器使用 `subfigure` 环境（需要 `subcaption`），主 `caption` 作为总题注；
`asset_id` 在子图模式下可以不使用（或作为无法拆分时的 fallback）。

渲染：

```text
\begin{figure}
  \begin{subfigure}{0.48\linewidth}
    \includegraphics[...]{...}
    \caption{...}\label{fig:a}
  \end{subfigure}\hfill
  ...
  \caption{Overall}\label{fig:main}
\end{figure}
```

### 11.2 表格合并/高级

```cpp
struct TableCell {
    InlineContent content;
    std::uint8_t row_span = 1;
    std::uint8_t column_span = 1;
};
```

不变式从“矩形”扩展为“网格覆盖无重叠、span 不越界、每行总列数等于 columns”。
这是较大的模型改动，建议单独 schema v5；本 WP 只做列对齐/多行表头（不合并）。

### 11.3 测试/风险

- 子图渲染、label/编号、迁移；
- 合并单元格的网格校验；
- 风险：`subcaption` 与 `caption` 包冲突；表格不变式容易回归。

---

## 12. WP-09 中文模板 + FrontMatter 适配器（P1）

### 12.1 模板定义扩展

**修改** `template/TemplateRegistry.h`：

```cpp
struct TemplateLanguage {
    std::string bcp47 = "en";       // "zh-CN"
    std::string fontset;            // "fandol"/"noto"
};

struct TemplateDefinition {
    // ... existing ...
    TemplateLanguage language;
    bool cjk = false;
};
```

### 12.2 FrontMatter 适配器注册表

当前 `LatexPlanner` 用 `if (def.frontmatter == "article-v1") ... else if ("ieee-conference-v1")`。
改为接口注册，便于中文/期刊适配：

**新文件** `render/FrontMatterAdapter.h`：

```cpp
namespace pf {

struct FrontMatterContext {
    const FrontMatter& front;
    const TemplateDefinition& tmpl;
    const DocumentNumberer* numbers = nullptr; // 可选
};

class IFrontMatterAdapter {
  public:
    virtual ~IFrontMatterAdapter() = default;
    virtual std::string id() const = 0; // "article-v1" / "ieee-conference-v1" / "ctex-article-v1"
    virtual std::vector<LatexNode> Emit(const FrontMatterContext&) const = 0;
};

class FrontMatterAdapterRegistry {
  public:
    static FrontMatterAdapterRegistry& Instance();
    const IFrontMatterAdapter* Find(const std::string& id) const;
    void Register(std::unique_ptr<IFrontMatterAdapter> adapter);
};

} // namespace pf
```

实现：

- `ArticleV1Adapter`：迁移现有 `ApplyFrontMatter` 逻辑；
- `IeeeConferenceV1Adapter`：迁移现有 IEEE 逻辑；
- `CtexArticleV1Adapter`：`\title/\author/\begin{abstract}/\keywords`（按 ctex
  文档）；用户可见的“Keywords”行用中文标签；
- `LatexPlanner::Plan` 查注册表；未知 id 抛错（BuildCoordinator 异常边界）。

### 12.3 内置中文模板

```cpp
TemplateDefinition t;
t.id = "chinese-article";
t.name = "中文期刊/技术报告";
t.document_class = "ctexart";
t.class_options = {"a4paper", "UTF8", "fontset=fandol"};
t.language = {"zh-CN", "fandol"};
t.cjk = true;
t.frontmatter = "ctex-article-v1";
t.toolchain.engine = LatexEngine::XeLatex;
t.bibliography_style = "unsrtnat";
t.packages = {{"amsmath",""}, {"amssymb",""}, {"graphicx",""},
              {"booktabs",""}, {"hyperref","hidelinks"}};
// ctexart 类自身会加载 ctex，不需要再重复声明 ctex 包；
// fontset 通过 class_options 传入。
```

`RuntimeManager` 已要求 `xelatex`；`CompilerFactory` 按 toolchain 选择引擎；
`LatexPlanner` 的 citation/natbib 逻辑不变。

### 12.4 中文正文与字体

- GUI 已使用 Noto Sans CJK；
- PDF 字体由 ctex `fontset` 决定；若 runtime 缺字体，构建报
  `E-FONT-MISSING`（DiagnosticMapper 已有字体分类）；
- 迁移：旧项目模板不变；新项目默认仍英文模板，用户可切换。

### 12.5 测试

- `TemplateRegistry` 包含中文模板；`TemplateVerifier` 校验 `xelatex` 存在；
- 真实构建：`你好，世界。` + `\text{中文}` 出现在 PDF（用 pdftotext 断言）；
- FrontMatter 适配器单测：`\title`/`\author`/abstract/keywords 字符串；
- 切换英文/中文模板不改 Document。

### 12.6 工作量与风险

- M（5–8 人日）。风险：ctex 与 `geometry`/`caption` 包的兼容；中文字体在
  runtime 中的可用性；`pdftotext` 对中文的提取需验证。

---

## 13. WP-10 author-year / biblatex（P1）

### 13.1 模板契约枚举化

**修改** `template/TemplateRegistry.h`：

```cpp
enum class CitationContract {
    NatbibNumericFirstUse, // 当前默认
    NatbibAuthorYear,
    BiblatexAuthorYear,
};

struct TemplateDefinition {
    // 保留 manifest 字符串，构造时解析为枚举
    std::string citation_contract = "natbib-numeric-first-use-v1";
    CitationContract citation = CitationContract::NatbibNumericFirstUse;
};
```

`CiteContractFromString`/`ToString` 提供迁移；未知契约在注册模板时拒绝或回退
并警告。

### 13.2 Planner 分支

```cpp
// 导言区
if (contract == BiblatexAuthorYear) {
    packages.push_back({"biblatex", "style=authoryear,backend=biber"});
} else {
    packages.push_back({"natbib",
        contract == NatbibAuthorYear ? "authoryear" : "numbers,sort&compress"});
}
// 引用
if (contract == BiblatexAuthorYear) {
    mode == Narrative ? Cmd("textcite", keys) : Cmd("parencite", keys);
} else {
    mode == Narrative ? Cmd("citet", keys) : Cmd("citep", keys);
}
```

`\bibliography` vs `\printbibliography`、`\bibliographystyle` 按契约切换。

### 13.3 编译器与 runtime

**`build/Compiler.cpp` 当前硬拒绝 Biber**：

```cpp
if (request.toolchain.bibliography_engine == BibliographyEngine::Biber) {
    result.log = "The current citation contract requires BibTeX, not Biber";
    ...
}
```

改为按 toolchain 生成 latexmk 参数：

```cpp
const bool biber = request.toolchain.bibliography_engine == BibliographyEngine::Biber;
const std::string driver =
    QuoteShell(latexmk) + " " + engine_flag +
    (request.toolchain.bibliography_engine == BibliographyEngine::None ? " -bibtex-" : " -bibtex") +
    " -norc -no-shell-escape -interaction=nonstopmode -file-line-error -halt-on-error -recorder " +
    " -e " + QuoteShell(biber ? "$biber = 'biber';" : "$biber = 'false';") + " " +
    QuoteShell(request.package.entry_file);
```

**`build/RuntimeManager`** 增加按 toolchain 的检查：

```cpp
Result<void, std::string> RuntimeManager::VerifyFor(const BuildToolchain& toolchain) const;
```

按模板实际需要检查：PdfLatex → pdflatex/latexmk；XeLatex → xelatex/latexmk；
Biber → biber；缺失时 `CompileFailureKind::RuntimeMissing` 并给出明确提示。
`TemplateVerifier` 在导入模板时调用同一检查，避免“安装成功、构建才报错”。

### 13.4 展示层

`CitationNumberResolver` 已扩展 `CitationStyle`（WP-01）：

- `NumericFirstUse`：数字；
- `AuthorYear`：`FormatPill(keys, mode)` 用 author_label/year；
- `AuthorYear` 不需要编号，`Find(key).number = 0`，`cited` 仍可用。

`ProjectController::CitationResolver()` 根据当前模板的 `CitationStyle` 构建：
`NumericFirstUse` vs `AuthorYear`（biblatex author-year 同样归入 AuthorYear）。

需要的 session 接口：

```cpp
// 解析当前模板的 CitationContract 后给出的展示样式。
CitationStyle ProjectSession::citation_style() const;
```

它只读取 `state_.template_selection()` + `ResolveTemplate`，不触碰文档内容；
`ProjectController` 缓存 key 中包含该值，切换模板时缓存自动失效。

### 13.5 测试

- 模板注册/校验：旧字符串契约映射正确；
- Planner 字符串：natbib numeric/authoryear、biblatex+biber；
- 真实构建：一个 author-year 模板的 `(Smith, 2020)` 与 `Smith (2020)`；
- 切换模板后 pill 与 PDF 一致。

### 13.6 工作量与风险

- L（10–15 人日）。风险：Biber 在 runtime 中是否存在、`latexmk` 参数；
  author-year 的姓名消歧/多人格式；模板契约的向后兼容。

---

## 14. WP-11 导出 `.tex`/arXiv + 源码视图（P1）

### 14.1 导出接口

**新文件** `build/ProjectExporter.h`：

```cpp
namespace pf {

struct ExportOptions {
    bool include_assets = true;
    bool include_bibliography = true;
    bool add_readme = false;
};

struct ExportManifest {
    std::filesystem::path root;
    std::filesystem::path main_tex;
    std::vector<std::filesystem::path> files;
    std::string plan_digest;
};

// 使用与 build 完全相同的渲染路径，保证导出的 .tex 就是编译的 .tex。
Result<ExportManifest, std::string>
ExportLatexBundle(const BuildSnapshot& snapshot,
                  const std::filesystem::path& destination,
                  const ExportOptions& options = {});

} // namespace pf
```

实现：

- `LatexRenderer().Render(RenderRequest{snapshot...})`；
- 写 `main.tex`、`references.bib`、模板资源；
- 按 `snapshot.asset_sources` 复制 assets（保持 `assets/` 相对路径）；
- 可写 `README.txt`（编译说明：xelatex + biber 等）；
- 目标目录已存在时：只允许空目录或用户确认覆盖；写入前用临时目录 + rename
  避免半成品。

### 14.2 Session/UI 接口

```cpp
// ProjectSession
Result<ExportManifest, std::string>
ExportLatexBundle(const std::filesystem::path& destination) const;

// ProjectController
Result<ExportManifest, std::string>
ExportLatexBundle(const QString& destination) const;
```

菜单：`File → Export LaTeX Bundle…`；完成后状态栏显示路径并提供“打开目录”。

### 14.3 源码视图

```cpp
struct LatexSourcePreview {
    std::string main_tex;
    std::vector<std::string> file_names;
};

Result<LatexSourcePreview, std::string>
ProjectSession::PreviewLatexSource() const;
```

只读 `QPlainTextEdit` 对话框；可复制。对长文档按需生成（与 build 快照一致），
不要在主窗口常驻。

### 14.4 测试

- 导出目录包含 `main.tex`/`references.bib`/assets；在临时目录用
  `tectonic` 或 `latexmk` 编译成功；
- 导出内容与 build 的 `package.files` 逐字节一致；
- 目标非空/无权限时结构化错误，不留下半成品。

### 14.5 工作量与风险

- M（5–8 人日）。风险：assets 路径重写、模板资源冲突、外部模板依赖；
  `main.tex` 中的绝对路径必须全部转换为相对路径。

---

## 15. WP-12 查找替换 / PDF 文本层 / 双向定位（P2）

### 15.1 查找替换接口

**新文件** `document/DocumentSearch.h`（纯 C++）：

```cpp
namespace pf {

struct SearchOptions {
    bool case_sensitive = false;
    bool whole_word = false;
    bool use_regex = false;
    bool include_captions = true;
    bool include_titles = true;
};

struct SearchHit {
    NodeId node;
    std::size_t inline_index = 0; // InlineContent 内的节点下标
    std::size_t byte_offset = 0;  // TextRun 内的 UTF-8 偏移
    std::size_t byte_length = 0;
    std::string preview;
};

class DocumentSearch {
  public:
    static std::vector<SearchHit> Find(const Document& document,
                                       const std::string& query,
                                       const SearchOptions& options = {});
};

} // namespace pf
```

替换：

- 找到 `SearchHit` 后复制对应的 `InlineContent`，替换 `TextRun` 内的字节区间，
  通过 `EditParagraphPayload` / `EditCaptionPayload` / `SetTableCellPayload`
  提交；不引入新的“批量替换直接改文档”路径。
- 批量替换 = 逐条命令（Undo 可逐条撤销）；或新增 `ReplaceAllPayload`（不推荐，
  破坏 undo 粒度）。

GUI：`FindReplaceBar`（非模态，Ctrl+F/Ctrl+H），高亮 `SearchHit` 所在行并滚动；
Replace/Replace All。

### 15.2 PDF 文档接口（文本层）

**新文件** `app/PdfDocument.h`：

```cpp
namespace pf::gui {

struct PdfTextLine {
    QRectF bounds_pt; // 页面坐标（pt）
    QString text;
};

class IPdfDocument {
  public:
    virtual ~IPdfDocument() = default;
    virtual int PageCount() const = 0;
    virtual QSizeF PageSizePt(int page) const = 0;
    virtual QImage RenderPage(int page, double dpi) const = 0;
    virtual QString PageText(int page) const = 0;
    virtual std::vector<PdfTextLine> TextLines(int page) const = 0;
};

} // namespace pf::gui
```

`PdfPreview` 增加：

```cpp
void SetDocument(std::shared_ptr<IPdfDocument> document);
QString SelectedText() const;
void FindText(const QString& text, bool forward = true);
```

异步光栅化接口（与 `MathRenderService` 同构）：

```cpp
class PdfRenderService : public QObject {
    Q_OBJECT
  public:
    static PdfRenderService* Shared();
    // 返回 request id；过期请求由 generation 丢弃。
    std::uint64_t RequestRender(std::shared_ptr<const IPdfDocument> document,
                                int page, int dpi);
  signals:
    void rendered(std::uint64_t request_id, int page, QImage image);
};
```

实现分两步：先接 poppler（`pdftotext -layout`/`pdftohtml -xml`）提供文本层；
再把渲染移到 `PdfRenderService`（异步 generation，WP-13）。

### 15.3 双向定位

需要 SyncTeX：

- 编译时加 `-synctex=1`（Compiler 参数）；
- 解析 `.synctex.gz`，提供：

```cpp
class ISyncTexResolver {
  public:
    virtual std::optional<NodeId> NodeAt(int page, QPointF pdf_point) const = 0;
    virtual std::optional<PdfLocation> PdfFor(const NodeId&) const = 0;
};
```

`SourceMap` 是“生成行 → 节点”，SyncTeX 是“PDF 坐标 ↔ 源行”；两者组合才能
实现点击跳转。该功能依赖 LaTeX 引擎输出，建议单独排期。

### 15.4 工作量与风险

- L（15+ 人日）。风险：poppler 依赖、SyncTeX 与模板/引擎兼容、页坐标缩放。

---

## 16. WP-13 派生数据缓存与性能（跨工作包）

### 16.1 缓存接口（`ProjectController`）

```cpp
private:
    struct DerivedCache {
        ProjectRevision revision;
        std::uint64_t doc_version = 0;
        std::uint64_t bib_revision = 0;
        std::string template_id;
        std::shared_ptr<const pf::CitationNumberResolver> citations;
        std::shared_ptr<const pf::DocumentNumberer> numbering;
    };
    mutable DerivedCache cache_;
```

```cpp
std::shared_ptr<const pf::CitationNumberResolver> CitationResolver() const;
std::shared_ptr<const pf::DocumentNumberer> Numbering() const;
```

- key = `(document_version, bibliography_revision, template_id, citation_style)`；
- 命中直接返回同一 `shared_ptr`，`BlockEditor::SetCitationCatalog` 可比较指针/
  revision 跳过全量刷新；
- `CountWords` 改用 `VisitInlineContent`，按 Unicode 词/字符统计，缓存结果。

### 16.2 UI 刷新

- `MainWindow::RefreshDocumentView` 只在 `document_version` 变化时重建行；
- citation/numbering 缓存命中时只更新 picker 数据，不重绘每行 pill；
- `RefreshSidePanels` 的 Outline/References 支持增量更新（至少不重建全部项）。

### 16.3 验收

- 1000 段文档单次提交，`CitationNumberResolver::Build` 调用次数为 0（命中）；
- 单次提交派生数据计算 < 16ms；
- 中文摘要字数 > 1。

---

## 17. schema v5 汇总

### 17.1 版本常量

```cpp
inline constexpr const char* kSchemaVersionV4 = "4";
inline constexpr const char* kSchemaVersion = "5";
```

`ProjectMigrator`：V3→V4（Figure/Table label）与 V4→V5（List/Footnote）
均为只加版本戳；旧字段默认值由读取器提供。

### 17.2 JSON 字段增补

| 位置 | 新增 |
| --- | --- |
| `figure` | `"label"`（string，可空）、`"subfigures"`（v4.1） |
| `table` | `"label"`（string，可空） |
| `paragraph/content` | `{"type":"footnote","body":[...]}` |
| `block` | `{"type":"list","style":"bullet|numbered","items":[...]}`、`{"type":"theorem","kind":"Theorem","title":[...],"body":[...],"numbered":true,"label":"thm:main"}`、`{"type":"code","language":"python","text":"...","caption":[...],"lineNumbers":true,"label":"lst:main"}` |
| `citation` | `"prefix"`/`"suffix"`/`"locator"`（WP-10 可选） |
| `project` | `"language"`、`"citationStyle"`（可选覆盖） |
| `template`（非 project.paper） | `theorem_environments`、`code_listing`、`language`、`cross_reference`、`citation_contract` 枚举 |

### 17.3 迁移测试

- V3/V4 文件：加载后所有旧字段不变，`migration.migrated == true`，保存后为 V5
  （V5 同时承载列表/脚注/定理/代码）；
- 重复迁移幂等；
- 未知块类型/字段：加载报结构化错误，不崩溃；
- V5 文件被旧版本读取：旧版本应拒绝（现有 `IsKnownVersion` 会报 unknown 并
  按原样加载——需要评估：建议旧版本对更高版本报错而不是按原样加载）。

---

## 18. 命令载荷汇总（新增）

| Payload | 用途 | 目标 WP |
| --- | --- | --- |
| `EditFigureLabelPayload` / `EditTableLabelPayload` | 自定义 label | WP-03/04 |
| `EditFigureWidthPayload` / `SetFigureAssetPayload` | 图宽/替换 | WP-05 |
| `SetTableCellPayload` / `InsertTableRowPayload` / ... | 表格编辑 | WP-04 |
| `InsertListPayload` / `InsertListItemPayload` / ... | 列表 | WP-06 |
| `InsertFootnotePayload` / `EditFootnotePayload` / `DeleteFootnotePayload` | 脚注 | WP-06 |
| `InsertTheoremPayload` / `EditTheoremPayload` | 定理 | WP-07 |
| `InsertCodePayload` / `EditCodePayload` | 代码 | WP-07 |
| `SetCitationsPayload`（可选，批量替换 keys/mode） | 重编辑/多选 | WP-01 |

所有 payload 追加到 `EditPayload`/`FullEditPayload` 末尾；`ApplyDocumentPayload`
的 `std::visit` 分派必须覆盖，未覆盖时返回 `UnknownPayload`（现有行为）。

---

## 19. 文件改动矩阵

### 19.1 新增文件

| 文件 | 内容 | WP |
| --- | --- | --- |
| `document/InlineTokenCodec.{h,cpp}` | citation token 编解码 | WP-01 |
| `numbering/DocumentNumberer.{h,cpp}` | 编号服务 | WP-03 |
| `math/MathCommandCatalog.{h,cpp}` | 数学命令目录 | WP-02 |
| `app/MathPaletteWidget.{h,cpp}` | 数学面板 | WP-02 |
| `app/MathCommandCompleter.{h,cpp}` | 命令补全 | WP-02 |
| `app/InlineContentEditorDialog.{h,cpp}` | 行内内容弹窗 | WP-01/04/06 |
| `build/ProjectExporter.{h,cpp}` | .tex/arXiv 导出 | WP-11 |
| `document/DocumentSearch.{h,cpp}` | 查找 | WP-12 |
| `app/PdfDocument.h` / `PdfRenderService.{h,cpp}` | PDF 文本层/异步 | WP-12 |
| `render/FrontMatterAdapter.{h,cpp}` | 模板 front matter 适配器 | WP-09 |

### 19.2 修改文件（主要）

| 文件 | 改动 | WP |
| --- | --- | --- |
| `document/Document.h` | Footnote/List/Theorem/Code、Figure/Table label | WP-04~07 |
| `document/DocumentEditor.{h,cpp}` | 新块/单元格/脚注操作 | WP-04~07 |
| `document/DocumentTraversal.{h,cpp}` | 新 NodeKind、脚注递归、列表 items | WP-06/07 |
| `editing/EditCommand.h` / `EditingSystem.cpp` | 新 payload 与分派 | 全部 |
| `persistence/ProjectPersistence.cpp` | v5 读写、嵌套脚注/列表/定理/代码 | WP-06/07 |
| `persistence/SchemaVersions.h` / `ProjectMigrator.cpp` | V4→V5 | WP-06/07 |
| `render/LatexPlanner.cpp` | 新块渲染、label、cref、biblatex | WP-03/06/07/10 |
| `template/TemplateRegistry.{h,cpp}` | 能力/契约/中文模板 | WP-03/07/09/10 |
| `validation/Validator.cpp` | 新诊断（重复 label、缺包、代码注入） | WP-02/03/07 |
| `app/PopupList.{h,cpp}` | 两行/多选/开关 | WP-01 |
| `app/BlockEditor.{h,cpp}` | citation catalog、富题注、表格、图片、列表/定理/代码卡片 | WP-01/04/05/06/07 |
| `app/InlineEditor.{h,cpp}` | mode、重编辑、脚注、note | WP-01/06 |
| `app/ProjectController.{h,cpp}` | 新编辑方法、缓存、导出 | 全部 |
| `app/MainWindow.{h,cpp}` | 菜单/连接/刷新/导出 UI | 全部 |
| `asset/AssetManager.{h,cpp}` | 矢量格式、converter | WP-05 |
| `numbering/CitationNumberResolver.{h,cpp}` | style/views/mode | WP-01/10 |

---

## 20. 测试策略与验收

### 20.1 分层

| 层 | 测试 | 工具 |
| --- | --- | --- |
| 值对象/编解码 | `InlineTokenCodec`、`DocumentNumberer`、`DocumentSearch`、`MathCommandCatalog` | `pf_tests` |
| 命令/模型 | 每个新 payload 的 Apply/Undo/序列化 round-trip | `pf_tests` |
| 渲染 | LaTeX 字符串断言 + 真实 TeX 构建 | `pf_tests`（`TestRuntime`/`TestEndToEnd`） |
| GUI | `paperforge-inline-editor-test` / `paperforge-gui-persistence-test` | Qt offscreen |
| 迁移 | V3 golden 文件加载/保存/再读 | `pf_tests` |
| 导出 | 导出目录编译、逐字节比较 | `pf_tests` |

### 20.2 每个 WP 的 Definition of Done

1. 公共接口有头文件注释与单元测试；
2. 旧项目能打开（V3/V4→V5），保存后仍是当前版本；
3. Undo/Redo 覆盖所有新操作；
4. 真实 TeX 至少一个构建用例；
5. 文档（README/docs）同步；
6. GUI 测试不依赖真实显示器（offscreen）。

### 20.3 关键验收（摘自 feature-gaps §7）

- citation picker 显示标题/编号/已引分组，多选/叙述式/重编辑正确；
- math palette 插入花体/黑板体，缺包时给出诊断；
- 交叉引用覆盖 Equation/Table/Subsection，胶囊与 PDF 编号一致；
- 题注富文本与表格单元格编辑往返无损；
- 中文模板构建出含中文的 PDF；
- 导出的 `.tex` 可在 Overleaf 编译；
- 列表/脚注/定理/代码进入 PDF。

---

## 21. 实施顺序与依赖图

```text
WP-00（一致性）
  └─ WP-01（citation）──→ WP-10（author-year/biblatex）
  └─ WP-02（math palette）
  └─ WP-03（numbering/xref）
        └─ WP-04（caption/table）
              └─ WP-05（figure controls/vector）
              └─ WP-08（subfig/table advanced）
  └─ WP-09（Chinese/FrontMatter）
        └─ WP-06（list/footnote）──→ WP-07（theorem/code）
  └─ WP-11（export）
  └─ WP-12（find/PDF text）  [P2]
  └─ WP-13（cache/perf）     [持续]
```

**阶段划分**（与 feature-gaps §6 对齐）：

| 阶段 | WP | 里程碑 |
| --- | --- | --- |
| M1（1–2 周） | WP-00、WP-01 | citation picker 可用；重编辑/叙述式闭环 |
| M2（2–4 周） | WP-02、WP-03 | 数学面板；交叉引用编号 |
| M3（4–6 周） | WP-04、WP-05 | 题注/表格/图片可用 |
| M4（6–9 周） | WP-09、WP-06、WP-07 | 中文、列表/脚注、定理/代码 |
| M5（后续） | WP-08、WP-10、WP-11、WP-12、WP-13 | 子图/样式/导出/查找/PDF 文本 |

每个 M 结束时必须保持：构建通过、全测试通过、V3 项目可打开、schema 迁移
可逆（重新导出 V3 golden 文件）。

---

## 22. 风险、回滚与开放问题

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| schema v5 迁移破坏旧项目 | 高 | golden 文件回归；迁移只在内存；保存前备份/原子写 |
| `Footnote` 递归/共享语义 | 高 | 限制嵌套；剪贴板显式编码；测试循环引用 |
| 新块类型扩散到所有 switch | 中 | 编译器 `-Wswitch`；为 `NodeKind`/`Block` 提供集中访问器 |
| 模板包缺失导致 PDF 失败 | 中 | `MathCommandCatalog`/`TemplateVerifier` 提前诊断 |
| 多选/两行 PopupList 影响旧菜单 | 中 | `chosen` 兼容；旧调用不传 Options；GUI 回归 |
| 表格编辑器焦点/撤销 | 中 | 复用 `BeginProtectedInsert`；单元格命令走统一协议 |
| 导出与 build 不一致 | 中 | 导出复用 `LatexRenderer::Render`，逐字节比较 |
| 中文模板字体缺失 | 中 | `E-FONT-MISSING`；启动/构建前检查 fontset |
| biblatex/Biber 不在 runtime | 中 | `TemplateVerifier` 检查；模板不可用时禁用 |

**开放问题**（需要产品决策）：

1. `@` 菜单是否恢复，还是从 README 删除？
2. 脚注是否允许嵌套？v1 建议禁止。
3. 表格合并单元格是否进入 v1/v2？
4. 原始 LaTeX 逃生口是否提供？安全与可移植性权衡。
5. 作者-年份样式是否作为 v1 必做，还是模板导入后再做？

---

## 23. 附录：关键头文件草案

### A. `numbering/DocumentNumberer.h`

见 §6.2（完整）。

### B. `math/MathCommandCatalog.h`

见 §2.4（完整）。

### C. `app/PopupList.h` 增量

见 §2.1（完整）。

### D. `document/InlineTokenCodec.h`

见 §2.3（完整）。

### E. `build/ProjectExporter.h`

见 §14.1（完整）。

### F. `app/PdfDocument.h`

见 §15.2（完整）。

---

## 24. 一句话总结

按本文实施时，先做**不改 schema 的接口层**（WP-00~WP-05），把“已有模型但
GUI 不会用”的能力补齐；再以 **schema v5** 为单位加入列表/脚注/定理/代码；
最后做样式、模板、导出与 PDF 文本层。每个 WP 都以可评审的头文件、命令载荷和
迁移测试为边界，避免把“图形界面补全”做成一次不可回滚的大重构。
