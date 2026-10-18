# 模板兼容性与插入警告方案

> 状态：方案草案，尚未实现。  
> 目标：所有会进入 PDF 的 GUI 添加操作，在插入前判断当前模板是否支持；不支持时弹窗说明。
> 用户选择继续时，由 PaperForge 通过受控的 LaTeX 扩展强行支持；没有可靠扩展时禁止插入。

## 1. 目标与范围

任何会反映到 PDF 的 GUI 添加操作都必须经过兼容性检查：

- 原生支持 → 直接插入，无弹窗；
- 模板未声明支持，但存在受控的 LaTeX 扩展（`Emulated`）→ 弹窗警告；
  - 用户取消 → 文档不变；
  - 用户继续 → 插入内容，渲染阶段自动注入扩展包/导言区声明；
- 没有可靠扩展（`Unsupported`）→ 弹窗说明，禁止插入，避免生成必然构建失败的文档。

覆盖范围：

| 类别 | 内容 |
| --- | --- |
| 结构块 | Paragraph、Equation、Figure、Table、List、Footnote、Theorem/Proof、Code |
| 标题 | Section / Subsection / Subsubsection 的层级 |
| 块属性 | Figure 双栏/宽度、Theorem 类型、Code 行号、列表样式 |
| 行内内容 | Inline Math、Citation、CrossReference、Footnote 引用 |
| 普通文本 | 永远支持，不检查 |

## 2. 核心原则

1. **内容模型只存语义，不存“用户点了继续”的标志**
   - 是否注入扩展由文档实际内容推导；
   - 同一文档换到支持该功能的模板后，扩展自动消失。

2. **插入判断和渲染注入共用同一套 Feature/Support 数据**
   - 不能 GUI 判断一套、Planner 再猜一套。

3. **“强行支持”必须是白名单扩展**
   - 扩展由 PaperForge 提供，不允许模板或用户拼接任意 `\usepackage`/LaTeX 代码；
   - 外部模板仍视为不可信输入。

4. **区分三种状态**
   - `Native`：模板原生支持；
   - `Emulated`：通过受控扩展支持，需要警告；
   - `Unsupported`：无可信扩展，禁止插入。

5. **警告不能静默降级**
   - 如果会使用 fallback，弹窗要明确写出：
     - 缺少什么能力；
     - 将注入哪些包/声明；
     - 可能造成什么视觉差异。

6. **Native 必须有证据**
   - 模板声明只是候选；
   - 必须通过导入期 feature probe 或内置模板的 CI 实测；
   - 没有 `VerifiedFeatureProfile` 的模板不默认 Native。

## 3. 架构分层

```text
template/
  TemplateFeatures.h/.cpp       // FeatureId、FeatureRequest、TemplateSupport
document/
  FeatureUsage.h/.cpp           // 从 Document 中扫描实际使用的 Feature
render/
  RenderExtensions.h/.cpp       // 白名单扩展包/导言区声明
  RenderSupportResolver.h/.cpp  // TemplateSupport + FeatureUsage -> RenderSupportPlan
  LatexPlanner.cpp              // 消费 RenderSupportPlan
app/
  CompatibilityGate.h/.cpp      // UI 插入前检查 + 弹窗 Presenter
  MainWindow.cpp                // 统一插入入口
```

### 3.1 模板侧：Feature 与原生支持

```cpp
enum class RenderFeatureId {
    Heading,
    Paragraph,
    Equation,
    Figure,
    FigureDoubleColumn,
    Table,
    List,
    Footnote,
    Theorem,
    Proof,
    CodeBlock,
    InlineMath,
    Citation,
    CrossReference,
};

struct FeatureRequest {
    RenderFeatureId feature;
    int heading_depth = 0;                 // Heading 使用
    TheoremKind theorem_kind = TheoremKind::Theorem;
    FigureSpan figure_span = FigureSpan::SingleColumn;
};

enum class SupportLevel {
    Native,
    Emulated,
    Unsupported,
};

struct TemplateSupport {
    SupportLevel level = SupportLevel::Unsupported;
    std::string reason_code; // 稳定 key，例如 "no-theorem-environments"
};

TemplateSupport CheckNativeSupport(
    const TemplateDefinition& template_definition,
    const FeatureRequest& request);
```

`CheckNativeSupport` 只看模板自身声明：

- `Heading(depth)`：`max_heading_depth >= depth`
- `Theorem` / `Proof`：`capabilities.theorems`
- `TheoremKind`：`theorem_environments` 中是否包含该 kind
- `CodeBlock`：`capabilities.code_blocks`
- `FigureDoubleColumn`：`two_column`
- 其余基础能力默认 `Native`

外部模板没有声明到的新能力，默认视为 `Emulated` 或 `Unsupported`，不能默认 `Native`。

> 注意：`CheckNativeSupport` 只回答“模板声明是否支持”。最终是否标记为
> `Native`，还必须通过 §3.2 的导入期 feature probe 证据；没有证据时保守降级。

### 3.2 原生支持如何确定：VerifiedFeatureProfile

#### 3.2.1 操作性定义

不存在“仅通过静态解析任意 `.cls/.sty` 就能 100% 确定原生支持”的方法。
本项目把 `Native` 定义为一个可验证合同：

```text
Native(feature) :=
    模板声明支持该 feature
    AND 对应 profile 的 feature probe 编译成功
    AND 语义断言通过
    AND 证据绑定到当前模板 digest/version
```

不满足任一条，只能标记为 `Emulated` 或 `Unsupported`，不能默认 `Native`。

#### 3.2.2 为什么不能只做静态解析

任意 LaTeX 模板都可能：

- 在 `.cls/.sty` 中动态定义、重定义命令；
- 用 class option 改变环境语义；
- 依赖包的加载顺序；
- 同名环境在不同 class 下编号/字体/双栏行为不同；
- 在版本更新中改变接口；
- 存在“命令存在但排版不可用”的情况。

因此 `.cls` 中出现 `theorem`、`lstlisting`、`figure*` 等字样，只能作为
预检线索，不能作为 Native 证据。

#### 3.2.3 四层判定流程

**第 1 层：显式模板声明**

内置模板使用 C++ `TemplateDefinition`；外部模板使用
`paperforge-template.json`（或适配器）声明：

```json
{
  "features": {
    "headings": { "maxDepth": 3 },
    "theorems": {
      "supported": true,
      "environments": ["theorem", "lemma", "proof"]
    },
    "code": { "supported": true, "engine": "verbatim" },
    "figures": { "doubleColumn": true },
    "citations": { "engine": "natbib" }
  }
}
```

这一层描述模板作者“打算支持什么”，不是最终 Native 证据。

**第 2 层：静态预检**

仅用于快速筛选和生成探测配置：

- `kpsewhich amsthm.sty` / `listings.sty` 检查包是否存在；
- 检查 manifest 声明的包名；
- 检查环境名、class 名是否安全；
- 生成待验证 feature 列表。

这一层不能单独决定 Native。

**第 3 层：导入期 feature probe（关键）**

现有 `TemplateVerifier` 已经会为每个 profile 编译一个探针，并检查：

- 编译是否成功；
- `.fls` 是否加载了导入的 class；
- `.bbl` 中 citation 顺序是否与编辑器一致。

扩展为**按 feature 独立探测**。每种 feature 使用独立的最小文档；某个 feature
失败不阻止模板导入，只把该 feature 标记为 unsupported。

**第 4 层：运行时只读已验证 profile**

插入与渲染时不再重新编译探针，只查询缓存的 `VerifiedFeatureProfile`：

```cpp
struct FeatureEvidence {
    RenderFeatureId feature;
    SupportLevel status;      // Native / Emulated / Unsupported
    std::string probe_digest; // feature probe 的源码/配置摘要
    std::string evidence;     // log marker / aux / PDF 断言摘要
};

struct VerifiedFeatureProfile {
    std::string template_digest;
    std::string template_version;
    std::map<RenderFeatureId, FeatureEvidence> features;
};
```

模板导入成功后，把 profile 写入：

- `template.lock.json` 的扩展字段；或
- 独立的 `template-capabilities.json`。

打开项目时只读取 profile，不在每次插入或构建时跑 TeX。

如果模板 resources digest 改变：

- profile 失效；
- 重新验证；
- 或退化为保守的 `Emulated/Unsupported` 判定。

#### 3.2.4 Feature probe 矩阵

| Feature probe | 探针内容 | 必须通过的断言 |
| --- | --- | --- |
| Heading 1/2/3 | 各级标题 + 正文 | 编译成功；PDF 文本包含标题；`.aux` 编号正确 |
| Theorem kind | 每种 `TheoremKind` 的 theorem 环境 | 编译成功；PDF 文本包含 `Theorem 1` 等 |
| Proof | `proof` 环境 | 编译成功 |
| Code | `verbatim` / `listings` 哨兵代码 | 编译成功；PDF 文本包含哨兵串 |
| Figure | 普通 `figure` + 图片 | 编译成功 |
| Double column figure | `figure*` 探针 | 编译成功且 class 实际为双栏 |
| Table | tabular/booktabs 探针 | 编译成功 |
| List | itemize/enumerate | 编译成功 |
| Footnote | `\footnote` | 编译成功 |
| Inline Math | amsmath/amssymb 探针 | 编译成功 |
| Citation | natbib + `.bib` | 编译成功；`.bbl` 顺序正确 |
| CrossReference | label + ref 双次编译 | 第二次 log 无 `??`，`.aux` 有 label |

语义断言优先使用：

- `pdftotext` 搜索唯一哨兵字符串；
- `.aux` 检查 `\newlabel` 和编号；
- `.bbl` 检查 citation 顺序；
- `.log` 检查 feature marker（探针可用 `\typeout{PF-FEATURE:...}` 输出）；
- 两次编译检查 `??`。

`figure*` 在单栏类中可能“编译成功但没有双栏语义”，因此双栏 Figure 的
Native 判定至少需要“class 实际为双栏”的证据；否则只能标记为 `Emulated`。

#### 3.2.5 状态映射

| 模板声明 | probe 结果 | 最终状态 |
| --- | --- | --- |
| supported | 成功 + 语义断言通过 | `Native` |
| supported | 失败 | 降级为 `Emulated` / `Unsupported`，记录证据 |
| unsupported | 失败 | `Emulated`（有 fallback）或 `Unsupported` |
| 未声明 / 未知 | 未探测 | 保守为 `Unsupported` / `Emulated` |
| 未声明但 probe 成功 | 成功 | 不直接标 Native；标 `Emulated/Provisional` 并警告 |

最后一行很重要：模板作者没有声明支持时，即使碰巧能编译，也不应承诺原生语义。
可以允许“强行支持”，但必须警告用户。

#### 3.2.6 内置模板与外部模板

- **内置模板**：C++ `TemplateDefinition` 是可信声明；CI 的真实 TeX 测试
  就是证据，可提供静态 `VerifiedFeatureProfile`，不需要用户每次都探测。
- **导入的外部模板**：`TemplateVerifier` 在隔离 workspace 中执行 feature
  probes，生成 profile；个别 feature 失败不阻止导入，只影响该 feature 的
  支持状态。
- **没有 profile 的旧外部模板**：一律保守处理，不默认 Native；已有
  theorem/code 内容由渲染 fallback 继续支持，但插入新内容时警告。

#### 3.2.7 与当前实现的衔接

| 当前实现 | 扩展方式 |
| --- | --- |
| `TemplateVerifier::VerifyTemplateArchive` | 从单一探针扩展为 `profile + feature probes` |
| `TemplateProbe` | 增加 `features` / `feature_evidence` / `probe_digest` |
| `VerifiedTemplatePackage` | 携带每个 profile 的 `VerifiedFeatureProfile` |
| `TemplateBinding` / `template.lock.json` | 持久化 profile，绑定模板 digest |
| `TemplateRegistry` | 提供 `NativeSupport(profile, feature)` 的唯一查询入口 |
| `LatexPlanner` | 只消费 profile + feature usage，不在 planner 里重复猜测 |

这一步完成后，“哪些是模板原生支持”就有确定答案：查 profile；profile 里
没有证据的，全部按未支持处理。

### 3.3 渲染侧：扩展包与导言区声明

```cpp
struct RenderExtension {
    std::string id; // "amsthm", "verbatim-listing", ...
    std::vector<LatexPackageRequirement> packages;
    std::string preamble; // 白名单内的导言区片段
    int order = 100;
    bool safe_to_force = true;
};

class RenderExtensionRegistry {
  public:
    static RenderExtensionRegistry& Instance();
    const RenderExtension* Find(const std::string& id) const;
};
```

建议的扩展：

| Feature | 扩展 |
| --- | --- |
| Theorem/Proof | `amsthm` + `\newtheorem` / `\newtheorem*` |
| Theorem kind | 对应 `\newtheorem{lemma}` 等声明 |
| Code | `listings`；runtime 没有时用 `verbatim` + Listing 计数宏 |
| Heading 超出模板深度 | 自定义 heading 宏/`\subsubsection`/`\paragraph` 兼容声明；若无法保持一致语义则标记 Unsupported |
| Inline Math | `amsmath` / `amssymb` / 按 `MathValidation.required_packages` 注入 |
| Citation | `natbib`；若模板 citation contract 不支持则 Unsupported |
| Double-column Figure | 只能使用 `figure*`；单栏类中无法真正“强行双栏” → 只能警告并退化为单栏，或标记 Unsupported |
| Table | `booktabs`；缺失时退化为普通 tabular |
| Footnote | 基础 `\footnote`；某些类禁用时用 `\footnotemark/\footnotetext` shim |

注意：**不是所有视觉差异都能靠加包解决**。双栏 Figure 在单栏类、或者三级标题在只支持到二级的模板中，如果扩展不能保持语义，就应该 `Unsupported`，而不是宣称“已强行支持”。

### 3.4 渲染计划

```cpp
struct AppliedExtension {
    std::string id;
    std::vector<LatexPackageRequirement> packages;
    std::string preamble;
};

struct RenderSupportPlan {
    std::vector<AppliedExtension> extensions;
    std::vector<Diagnostic> diagnostics; // W-TEMPLATE-EMULATED / E-TEMPLATE-UNSUPPORTED
};

class RenderSupportResolver {
  public:
    RenderSupportPlan Resolve(const Document& document,
                              const TemplateDefinition& template_definition) const;
};
```

`LatexPlanner` 的流程改为：

```text
Document
  -> FeatureUsageScanner::Scan
  -> RenderSupportResolver::Resolve
  -> 按顺序合并 packages
  -> 注入白名单 preamble
  -> 原有 LaTeX 生成
```

扩展合并要求：

- 包去重；
- 包顺序稳定：
  - `amsmath` / `amssymb` 靠前；
  - `amsthm` 在 theorem 声明之前；
  - `hyperref` 最后；
  - `cleveref` 在 `hyperref` 之后；
- `\newtheorem` / `\newcounter` 只生成一次；
- `render-plan.json` 的 audit 增加 `extensions` 字段，便于 Build Log 和测试断言。

当前 `LatexPlanner` 已经有一套“检测到 Code/Theorem 就注入 fallback”的逻辑，建议把这些逻辑收编进 `RenderSupportResolver`，避免以后出现第三套判断。

## 4. 插入流程

所有插入统一经过 `CompatibilityGate`，并查询当前模板的
`VerifiedFeatureProfile`（内置模板使用静态 profile，外部模板使用导入期
probe 生成的 profile）：

```cpp
struct CompatibilityReport {
    SupportLevel level;
    FeatureRequest request;
    std::string template_name;
    std::string reason_code;
    std::vector<std::string> extension_names;
    std::string feature_evidence; // 例如 "probe:thm/2026-10-18"
};

class CompatibilityGate {
  public:
    enum class Decision { Cancel, Allow, AllowWithExtension };

    using Presenter =
        std::function<Decision(const CompatibilityReport&)>;

    Decision Check(const CompatibilityReport& report) const;
    void SetPresenterForTest(Presenter presenter);
};
```

生产 Presenter 使用 `QMessageBox`，测试注入脚本 Presenter。

### 4.1 结构块与标题

`MainWindow::InsertBlockRequested` 是统一入口，增加：

```cpp
if (!ConfirmInsertion(FeatureForBlockType(type, anchor)))
    return;
```

- `section` / `subsection` / `subsubsection` → `Heading(depth)`
- `theorem` / `proof` → `Theorem(kind)`
- `code` → `CodeBlock`
- 其他 block 按 feature 映射

用户选择继续后，直接走原有 `controller_->Insert...`，文档里只增加语义 block。

### 4.2 双栏 Figure

`FigureSpanChanged`：

- `FigureDoubleColumn` 不兼容时弹窗；
- 用户继续 → 存储 `FigureSpan::DoubleColumn`；
- renderer 根据 `RenderSupportResolver` 选择 `figure*` 或单栏 fallback；
- 如果当前模板完全无法表达双栏，扩展注册表应返回 `Unsupported`，不能假装支持。

### 4.3 行内内容

行内内容的入口不在 `InsertBlockRequested`：

- `InlineEditor::BeginInlineMath`
- `ShowCitationPicker`
- `ShowReferencePicker`
- `ShowFootnoteEditor`

建议给 `InlineEditor`/`BlockEditor` 注入一个：

```cpp
using FeatureGate =
    std::function<bool(const FeatureRequest&)>;

void SetFeatureGate(FeatureGate gate);
```

`BlockEditor` 在打开数学编辑器、citation picker、footnote 对话框前调用 gate。若拒绝，不打开对话框、不插入 token。

这一块可以放到 Phase 2，避免第一期改动面过大。

## 5. 弹窗 UX

### 可扩展（Emulated）

```text
Template limitation

The current template "Elsevier CAS" does not declare Code block support.
PaperForge can add a verbatim listing fallback, but the PDF may differ
from the editor.

[Insert Anyway] [Cancel]
```

### 不可扩展（Unsupported）

```text
Template limitation

The current template "IEEE Conference" only supports headings up to level 2.
A Subsubsection cannot be rendered consistently.

[OK]
```

建议策略：

- 默认按钮：`Cancel`
- 一次插入只弹一次
- 可选 session 级“本模板不再提示”复选框（不写入文档）
- 不要在打开项目、恢复 autosave、切换模板时弹窗；只在用户主动添加时弹

## 6. 持久化策略

**建议不增加 schema 字段。**

原因：

- 文档只保存“有什么内容”；
- 扩展由 `FeatureUsageScanner + RenderSupportResolver` 从内容实时推导；
- 用户点“继续”不是文档语义，不应该持久化；
- 同一项目换模板后扩展自动重算，不会残留“某次强制支持”的旧状态。

只有一种情况需要持久化：如果未来允许用户**永久关闭某模板的能力警告**，那应放在 `QSettings`（UI 偏好），而不是 `project.paper`。

## 7. 诊断与可观测性

增加稳定诊断码：

```text
W-TEMPLATE-FEATURE-EMULATED
E-TEMPLATE-FEATURE-UNSUPPORTED
E-TEMPLATE-EXTENSION-MISSING
```

Build Problems / Build Log 显示：

```text
Template "elsevier-cas-dc" does not natively support Code blocks.
Applied extension: verbatim-listing (numbering + label shim).
```

`render-plan.json` 增加：

```json
{
  "extensions": ["amsthm", "verbatim-listing"],
  "emulatedFeatures": ["CodeBlock"]
}
```

这样测试和用户都能解释“为什么 PDF 和模板原本样式不完全一致”。

## 8. 测试方案

### 8.1 单元测试

`TestTemplateSupport`：

- 内置 generic → 所有基础 feature Native
- 内置 IEEE → Heading 3、Theorem、Code 等 Native
- 外部 CAS（capabilities=false）→ Theorem/Code Emulated，Heading 3 Unsupported
- 具体 theorem kind 不在 `theorem_environments` → Emulated 或 Unsupported
- FigureDoubleColumn 在单栏类 → Unsupported/fallback

`TestTemplateVerifier` / `TestVerifiedFeatureProfile`：

- 内部模板的静态 profile 与 CI 证据一致；
- 外部模板逐 feature probe；
- 某 feature 失败不阻止模板导入，只把该 feature 标为 unsupported；
- profile 绑定 template digest，digest 改变后失效；
- 伪造 manifest 声明但 probe 失败时，最终状态不能是 Native。

`TestRenderSupportResolver`：

- 文档含 Code → 只注入一次 verbatim fallback
- 文档含 Theorem → 只注入一次 amsthm + 声明
- 文档无相关 feature → 不注入任何额外内容
- 多个 Code block → 计数器/宏不重复
- 扩展包顺序稳定

### 8.2 Golden render 测试

对同一文档断言：

- 支持模板下不出现额外包；
- 不支持模板 + 用户强制后出现指定扩展；
- 无扩展时 planner 抛结构化错误；
- `render-plan.json` 的 `extensions` 与预期一致。

### 8.3 GUI 测试

使用 `CompatibilityGate` 的 Presenter 注入，避免真实 modal 阻塞：

```cpp
gate.SetPresenterForTest([](const CompatibilityReport& report) {
    return CompatibilityGate::Decision::Cancel;
});
```

测试：

- Cancel：文档 revision 不变、block 不增加；
- AllowWithExtension：block 增加，renderer 注入扩展；
- Native：Presenter 不被调用；
- 标题超深度：只允许 Cancel；
- Figure 双栏：单栏模板下弹窗。

### 8.4 真实 TeX

保留并扩展现有：

- generic-article
- IEEEtran
- imported Elsevier CAS
- Code/Theorem/Subsubsection 的 forced support
- 每个模板至少一次真实 PDF 构建

### 8.5 回归/兼容性

- 现有项目无 unsupported feature 时，PDF 字节级行为不因新子系统改变；
- V5 项目无需迁移；
- 外部模板 capability 缺失时，已有 code/theorem 项目继续可构建；
- `pf_tests` / GUI / template tests 全绿。

## 9. 实施顺序与工作量

### Phase 1：底层能力模型（2–3 天）

1. `RenderFeatureId` / `FeatureRequest` / `TemplateSupport`
2. `FeatureEvidence` / `VerifiedFeatureProfile`
3. `TemplateVerifier` 的 feature probe 扩展
4. `FeatureUsageScanner`
5. `RenderExtensionRegistry`
6. `RenderSupportResolver`
7. 把 `LatexPlanner` 现有 theorem/code fallback 收编
8. 单元测试 + golden render + feature probe 证据测试

此阶段不改 UI 行为，先让“能力判断”和“扩展注入”成为唯一事实来源。

### Phase 2：结构块插入门禁（1–2 天）

1. `CompatibilityGate` + Presenter
2. `MainWindow::InsertBlockRequested` 统一检查
3. 菜单不再隐藏 theorem/code，增加 limitation 标记
4. Heading 超深度处理
5. Figure 双栏检查
6. GUI 测试

### Phase 3：行内内容门禁（1–2 天）

1. `InlineEditor/BlockEditor` 注入 FeatureGate
2. Inline Math 包检查
3. Citation / CrossReference / Footnote 检查
4. GUI 测试

### Phase 4：诊断、文档、QA（1 天）

1. Build diagnostics
2. `VerifiedFeatureProfile` 持久化/失效策略
3. render-plan audit
4. README / design doc
5. real TeX matrix

总计约 **5–7 个开发日**。如果只做结构块和 theorem/code 的最小闭环，可压缩到 **2–3 天**。

## 10. 风险与控制

| 风险 | 控制 |
| --- | --- |
| “加包就支持”造成错误承诺 | 扩展只能来自白名单；无法保证语义时标 `Unsupported` |
| 外部模板缺少包 | 构建诊断 `E-TEMPLATE-EXTENSION-MISSING`，不要静默失败 |
| 警告太频繁 | 每次主动插入只提示一次；后续可加 session 级“不再提示” |
| 扩展导致 PDF 视觉变化 | 弹窗明确说明；Problems/Build Log 记录 emulated feature |
| 现有项目行为回归 | 对无新增 feature 的文档，不改变生成结果；golden 回归 |
| GUI 弹窗阻塞测试 | Presenter 注入，不直接依赖 `QMessageBox` |
| 行内内容和结构块逻辑分叉 | 统一使用同一个 `FeatureRequest`/`RenderSupportPlan` |

## 11. 验收标准

1. 插入模板原生支持的内容：无弹窗、无额外 LaTeX。
2. 插入 Emulated 内容：出现警告；选择继续后成功插入，renderer 注入正确扩展。
3. 插入 Unsupported 内容：出现解释性警告；取消后文档完全不变。
4. 扩展包去重、顺序稳定、`render-plan.json` 可审计。
5. 内置 generic/IEEE、外部 CAS 均能通过真实 TeX 构建。
6. 不需要 schema 迁移。
7. 现有测试全绿，新增单元、GUI、golden、真实 TeX 测试覆盖上述矩阵。

## 12. 需要确认的产品决策

1. **默认按钮**：建议 `Cancel`，避免误插。
2. **标题超深度**：建议默认禁止，不强行降级为 `\paragraph`，因为会破坏论文结构。
3. **双栏 Figure**：单栏模板下建议只允许单栏 fallback，不宣称“强行双栏”。
4. **行内内容**是否纳入第一期？建议第二期，先完成 block/heading 的统一门禁。
5. 是否需要“本模板不再提示”？建议先不做持久化，最多做 session 级。

## 13. 与现有代码的衔接点

| 现有位置 | 计划改动 |
| --- | --- |
| `template/TemplateRegistry.h` 的 `TemplateCapabilities` | 保留兼容字段，新增 feature 查询层 |
| `build/TemplateVerifier.cpp` | 从单一探针扩展为 profile + 逐 feature probe；生成 `VerifiedFeatureProfile` |
| `template/TemplatePackage.h` / manifest | 声明 features；profile 与模板 digest 绑定 |
| `template/TemplateBinding.h` / `template.lock.json` | 持久化验证证据；digest 变化时失效 |
| `render/LatexPlanner.cpp` 的 theorem/code fallback | 收编进 `RenderSupportResolver` |
| `app/MainWindow.cpp` 的 `InsertBlockRequested` | 统一结构块插入门禁 |
| `app/MainWindow.cpp` 的 `FigureSpanChanged` | 双栏 Figure 检查 |
| `app/BlockEditor.cpp` 的 `SetContentCapabilities` | 从“隐藏”改为“标记 limitation + 允许触发警告” |
| `app/InlineEditor` / `BlockEditor` 的行内插入入口 | Phase 3 注入 `FeatureGate` |
| `validation/Validator.cpp` | 增加 emulated/unsupported 诊断 |
| `docs/feature-implementation-design.md` | 增加本方案的链接和状态 |
