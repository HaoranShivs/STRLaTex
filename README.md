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
                         ↓
      Validator → LatexRenderer → BuildPackage + SourceMap
                         ↓
                 TectonicCompiler (后台线程)
                         ↓
      DiagnosticMapper → BuildResult → PDF / Problems
```

## 模块结构

```
src/
├── core/          StrongId / ProjectRevision / Result / Diagnostic / Json / IdGenerator
├── document/      Document / DocumentEditor / DocumentIndex / InlineText
├── editing/       EditCommand / EditingSystem / UndoHistory / AnchorResolver
├── project/       ProjectState / SnapshotFactory / ProjectSession
├── template/      TemplateRegistry（Generic Article / IEEE Conference）
├── asset/         AssetManager（staging → 注册分离，内容哈希，图片尺寸嗅探）
├── bibliography/  BibTeX 解析 / BibliographyService / 搜索
├── validation/    Validator（语义 / 模板层 Diagnostic）
├── render/        LatexRenderer / SourceMap / BuildPackage
├── build/         ICompiler / TectonicCompiler / MockCompiler / DiagnosticMapper
│                  / BuildCoordinator（debounce + latest-wins + 两层取消）
├── persistence/   ProjectSerializer（project.paper JSON）/ 原子保存 / SaveCoordinator
│                  / 自动保存定时器 + 崩溃恢复
└── cli/           paperforge 命令行工具
src/app/           Qt6 GUI（paperforge-gui）：结构化编辑器 + Outline + Problems
│                  + Build 状态 + PDF 预览（pdftoppm 渲染）
tests/             53 个单元 + 端到端测试（含真实 Tectonic 构建、自动保存恢复、
│                  模板必填校验、GUI 控制器全流程冒烟）
tools/
├── bin/tectonic         静态链接的 tectonic 0.15.0
└── tectonic-cache/      LaTeX bundle 缓存（离线构建可用）
```

## 关键架构约束的落实

| 基线要求 | 实现 |
| --- | --- |
| Document Tree 永远 structurally valid | 闭式 schema：`Section/Subsection/Block` 为固定 `struct` + `std::variant`；Table 由 `MakeTable` 工厂保证矩形 |
| NodeId 唯一且稳定 | `StrongId<Tag>` 强类型 ID；序列化/反序列化保留原 ID |
| ProjectRevision 单调递增 | `ProjectState::BumpRevision()` 是唯一入口；Undo/Redo 也产生新 Revision（测试验证） |
| Template Change 不改 Document | `DocumentVersion` 不变、`ProjectRevision +1`、可进入 Undo（测试验证） |
| 后台不读 Live Document | `SnapshotFactory` 深拷贝出 `shared_ptr<const Document>`；跨线程只传 Snapshot/Request/Result/Event |
| 最多 1 Active + 1 Pending | `BuildCoordinator` 用 `optional<Active/Pending>` 而非队列；测试验证 Rev1 被取代后永不完成 |
| Stale Result 不进 UI | `OnBuildFinished` 按 `ProjectRevision` 过滤，stale 直接丢弃 |
| Compiler 不知道 Document | Compiler 只见 `BuildPackage`；错误经 `DiagnosticMapper + SourceMap` 映射回 NodeId |
| LaTeX 隔离 | Document Core 无任何 `\section`/`\label` 字符串知识；全部在 Renderer |
| 保存串行 + 旧不覆盖新 | `SaveCoordinator` 互斥 + revision 比较；原子写 = 临时文件 → 校验 → rename |
| Asset 导入与注册分离 | `AssetManager::Stage` 只暂存；真正插入文档时才 `Register` |
| 异步插入用 Stable Anchor | `AnchorResolver` 按当前 Revision 重新解析插入点 |
| 全操作可撤销 | Undo 历史存储操作前后的**全文快照**（before/after），删除 Section 等结构性操作也能完整恢复，NodeId 跨 Undo 保持稳定 |
| 自动保存不干扰状态机 | 后台定时器只在 Dirty 时写 `.paperforge/autosave/`，不改变 Clean/Dirty 状态；重开项目时若 autosave 比 project.paper 新则提示恢复 |

## 构建

```bash
cmake -B build -G Ninja
cmake --build build
```

要求：CMake ≥ 3.16，C++20 编译器（GCC 12+ 已验证），Qt6（可选——未安装时自动
跳过 GUI，核心库与 CLI 不受影响），poppler-utils（GUI 的 PDF 预览用 `pdftoppm`，
可选）。

## 使用

### 桌面应用（Qt6 GUI）

```bash
./build/src/app/paperforge-gui
```

结构化论文编辑工作台——用户只编辑论文语义，不编辑排版代码：

- **三栏布局**（可拖动）：Outline | Editor | Preview + Problems（18/52/30）
- **Block 卡片编辑器**：Title / Authors / Institution / Abstract / Keywords /
  Section / Paragraph / Equation 都是卡片，行高随内容自适应，居中限宽 820px
  - hover 显示拖拽手柄 + 类型标签 + `⋯` 菜单（Move Up / Down / Delete），
    空间预留不跳动
  - 聚焦块左侧强调线；模板必填但为空的卡片红边高亮
- **`/` 斜杠命令**：空行输入 `/` 弹出块创建菜单（Basic/Academic 分组，
  键盘 Up/Down/Enter/Esc + 文本过滤），完全非模态
- **`@` 引用菜单**：输入 `@` 弹出 References / Sections / Figures 分组菜单，
  双击 Outline 的 References Tab 条目也可插入引用
- **Outline 双 Tab**：Document（章节树，点击滚动定位）/ References（文献库，
  可搜索）
- **结构化 Problems**：severity 分组 + All/Errors/Warnings 过滤 + Build Log Tab
- **Build 按钮**：`Build ▶` → `Building ◌` → `✓ Built`（绿）/ `! Build failed`
  （红），状态栏显示构建阶段与 Revision
- **Welcome 页**：New / Open / 最近项目（QSettings 持久化）
- **状态栏**：✓Saved / ●Unsaved、Build 状态、字数统计
- **自动保存**：30 秒后台保存；崩溃后重开提示恢复
- **PDF 预览**：构建成功后 pdftoppm 渲染页面图像

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
./build/tests/pf_tests          # 53 个测试
ctest --test-dir build          # 或通过 CTest
```

覆盖：Document schema/编辑器约束/Table 矩形性、Editing 协议（stale 拒绝、
undo/redo、revision 单调性、Template 变更语义、**删除恢复型 Undo**）、
Renderer（转义、SourceMap 映射、bib 打包）、持久化（round-trip、原子写、
stale-save 拒绝、**自动保存/崩溃恢复**）、BibTeX 解析/搜索、BuildCoordinator
（latest-wins、诊断映射）、Validator（缺失引用/悬空交叉引用/空标题）、
ProjectSession 工作流、**真实 Tectonic 端到端 PDF 构建**，以及 GUI 控制器
全流程冒烟测试：

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-gui-smoke
```

## V1 范围外（按基线）

Subsubsection、List、Footnote、复杂表格、Subfigure、Word 导入、Zotero 同步、
AI 等均按基线排除。
