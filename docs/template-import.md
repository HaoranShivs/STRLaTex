# 模板导入与生成计划（API 1）

## 使用方式

1. 打开论文项目，选择 **File → Import Template (.zip)…**。
2. 选择压缩包。应用会检查文件清单，并在后台为每个 Profile 试编译测试论文；可以取消。
3. 验证通过后选择单栏或双栏等 Profile，点击 **Import and use**。
4. 顶部模板选择器可以随时切换。切换保留文档语义内容，增加项目 revision，支持撤销和重做。
5. 保存项目。下次打开时从该项目的资源目录加载固定版本，不需要原 ZIP。

导入 `tools/templates/els-cas-templates.zip` 时，内置 CAS 适配器识别包内真实的
`cas-sc.cls`、`cas-dc.cls`、`cas-common.sty` 和 `cas-model2-names.bst`，
提供 **Elsevier CAS Single Column**、**Elsevier CAS Double Column** 两个选项。
不会运行压缩包中的 Makefile、脚本或示例论文。

CAS 的作者、机构、邮件、摘要和关键词按其原生命令生成；摘要及关键词位于
`\maketitle` 之前。类文件自行控制页面尺寸、字体和单双栏，不叠加 Generic Article
的 `geometry` 或 `caption` 设置。IEEE 使用原生 `\IEEEauthorblockN/A` 与
`IEEEkeywords`，会议模式不再用被锁定的 `\thanks` 表达机构。

当前编辑器按首次出现的顺序显示数字引用。CAS 使用其支持的 natbib 数字模式和
`unsrtnat`；IEEE 使用官方 `IEEEtranN`，同时支持 `\citep` 和 `\citet`。
CAS 包自带的作者年份排序样式保留在资源中；首版没有作者年份引用界面，也不把该模式
当成数字编号模式使用。期刊要求作者年份格式时，还需要扩展引用契约和编辑器显示。

## 项目资源与固定版本

```text
project/
  project.paper                         # templateBinding 是权威绑定
  template.lock.json                    # 保存成功后派生的审计视图
  template-resources/<完整 SHA-256>/
    manifest.json                       # 所有保存文件的摘要
    files/...                           # 原始模板文件及说明，不改写出版社文件
  .paperforge/build/                    # 调试用生成文件；可重新生成
  project.pdf                          # 最后一次成功且通过预览门禁的 PDF
```

备份和分享项目时连同 `template-resources/`、`assets/`、参考文献文件一起复制。
资源先发布到按摘要命名的不可变目录，再由正常的项目保存写入绑定。
根目录 lock 是派生文件；删除它不会使应用选择其他模板。

绑定记录 `id`、`version`、`digest`、`profile` 和 `options`。Profile ID 含包摘要的
前 12 位以区分版本，完整摘要仍参与校验。重新打开或发起外部模板构建时会校验资源。
文件缺失、内容变化、固定版本不匹配会明确失败，不自动退回 article 或更换为最新版。
导入新版会保留旧资源，以供当前绑定、撤销历史和在途构建继续使用。
首版不自动清理旧模板包，一个项目最多加载 32 个包。
为限制多 Profile 及多个包的内存放大，单包所有 Profile 的构建资源合计、项目目录
加载的原始文件加构建资源合计分别限制为 64 MiB。

旧项目只有 `template` 字符串时，仍能打开内置模板；应用在内存里补上绑定，在下次
保存时写入。`templateBinding` 是现有项目 JSON 的附加字段；打开不会改写原文件。
切换/关闭项目会清空当前编辑历史，避免跨项目撤销。

## 其他模板包的 manifest

没有现成适配器的出版社 ZIP 会显示 `NeedsAdapter`，不会把任意示例 `.tex` 当成
已经支持的模板。能使用已有 frontmatter 组件的包可放入 `paperforge-template.json`：

```json
{
  "apiVersion": 1,
  "version": "1.0",
  "profiles": [
    {
      "id": "my-journal",
      "name": "My Journal",
      "class": "myjournal",
      "frontmatter": "article-v1",
      "classOptions": ["11pt"],
      "twoColumn": false,
      "bibliographyStyle": "unsrtnat"
    }
  ]
}
```

ZIP 根目录同时提供 `myjournal.cls` 及依赖资源；允许外面包一层共同目录。
ID、版本、类名和类选项以英文字母或数字开头，其后可用字母、数字、点和连字符。
`class` 不含 `.cls` 扩展名。API 1 可选组件为 `article-v1`、
`ieee-conference-v1`、`elsevier-cas-v1`。文献样式支持 `unsrtnat`、`IEEEtranN`，
后端固定为 pdfLaTeX + BibTeX。Profile 中只能使用文档示例列出的字段，未知 API、
组件或构建命令直接拒绝。

manifest 选择已有适配器，不能嵌入 LaTeX 片段、脚本、原生插件或任意构建命令。
当前绑定 `options` 必须为空；具体类选项由受检 manifest 的 `classOptions` 给出。
更复杂的期刊先新增有测试的 C++ 组件和所需语义字段，再扩展 API。

## 构建过程和诊断

`LatexRenderer` 是兼容门面，执行：

```text
Document + 已解析模板定义 + 文献/资源
  → LatexPlanner（组件覆盖、依赖和能力检查）
  → shared_ptr<const LatexGenerationPlan>
  → LatexEmitter（只执行计划、转义文本、计算实际行范围）
  → BuildPackage + SourceMap
  → 计划指定的 TeX 引擎 / 文献后端
```

通用规划器负责富文本、数学、引用、标签、图表及正文；每个 frontmatter 组件负责
完整的前置信息语法和顺序。宏包声明可以标记“已由类提供”，避免重复加载和选项冲突。
输出器不查询模板 ID、注册表、活文档或适配器回调。构建线程收到冻结的定义和外部
模板资源字节，不受后续 GUI 切换或目录更新影响。旧结果仍由 revision/snapshot/build
门禁拦截。

每个构建工作区保留：

- `main.tex`、使用到的类文件及宏包资源；
- `render-plan.json`：带来源的最终 IR、宏包、引用契约、编译要求和生成摘要；
- `template.lock.json`：本次计划所用模板绑定及资源摘要；
- `compile.lock.json`：实际引擎、运行时类/字体等 recorder 输入及编译摘要；
- TeX、latexmk、BibTeX 日志及 `main.fls`，用于确认实际加载的类文件。

生成摘要排除请求编号、revision、时间和工作区绝对路径；编译摘要补充实际输入内容。
这些文件用于审计，首版不据此复用 PDF 缓存。运行时整体版本锁定及迁移策略仍是后续工作。
正文错误只映射到实际生成范围；类/宏包错误保留其文件位置，前置信息带有字段来源。

## 运行依赖与导入检查

C++ 构建需要 C++20、OpenSSL Crypto、zlib；桌面界面需要 Qt6 Widgets。
外部模板编译当前支持 Linux，需要 `/usr/bin/bwrap`、`/usr/bin/prlimit`，以及系统
允许创建用户、进程和网络命名空间。隔离不可用时明确拒绝，不降级为直接执行。

ZIP 检查拒绝路径穿越、绝对路径、链接/设备、重复目标、文件与目录冲突、损坏 CRC、
加密及不支持的 ZIP 类型。ZIP32 的 stored/deflate 是首版支持格式；不支持 ZIP64
和多卷。限制为 256 个条目、16 MiB/文件、64 MiB/压缩包和总解压内容，并限制高压缩比。

外部模板的试编译及正式构建均关闭 shell escape 和 latexmk 配置自动加载，隔离网络、
用户目录与其他项目，只开放运行时只读、系统工具/库和本次工作区读写。
编译有时间、内存、单文件大小与进程数限制。资源摘要用于固定内容，不证明出版社身份。

CAS 依赖列表已加入 `tools/build-runtime/packages.txt`，包括 makecell、multirow、
sttools、xstring、footmisc、xcolor、moreverb、wrapfig、preprint、STIX 等。
依赖应在发布运行时中预装；用户导入时不会自动安装或更改全局 TeX。
`balance` 随 [preprint](https://www.ctan.org/pkg/balance) 提供，
`stfloats` 随 [sttools](https://ctan.org/pkg/stfloats) 提供。

## 验证

`pf_template_tests` 包含生成计划、IEEE 原生前置信息、转义/富文本、摘要稳定性、
ZIP 边界、manifest 拒绝策略、资源改动检测、绑定保存与撤销等测试。
存在上述 CAS ZIP 时，还会真实编译两个 Profile，检查 `.fls` 证明加载的是导入类，
并验证引用顺序、PDF 中的作者/机构/关键词及保存后重新打开。

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

自动化环境需要给 `pf_template_tests` 开放上述命名空间能力；GUI 常规测试可使用
`QT_QPA_PLATFORM=offscreen`。GUI 测试也覆盖导入入口启用状态、失败时不安装资源、
选择器随模板切换/撤销/重新打开同步。

2026-10-17 的本机验证已通过：188 项核心测试、14 项模板测试、78 项编辑器 GUI
测试，以及持久化、块投影和 GUI 冒烟测试。真实 xcb 界面完成了导入、单双栏切换、
撤销/重做、保存重开及旧 PDF 恢复。同一测试论文在 CAS 单栏输出 3 页、双栏输出
2 页；构建所用 `.cls` 和 `cas-common.sty` 的 SHA-256 与输入 ZIP 的原文件一致。
本地 PDF、截图和验证记录位于 `build/template-import-check-final/`。
