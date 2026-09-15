# 仓库布局约定

目标很简单：`git diff` 里出现的每一行都应该是**人有意识改的代码或数据**，而不是
某次运行留下的产物。

## 目录职责

```
STRTex/
├── src/                    源码（唯一被编译的东西）
│   ├── core/               StrongId / Result / Diagnostic / Json
│   ├── document/           Document schema + DocumentEditor + DocumentIndex
│   ├── editing/            EditCommand / EditingSystem / UndoHistory / AnchorResolver
│   ├── project/            ProjectState / SnapshotFactory / ProjectSession
│   │                       / PreviewGate / ApplicationEvent / PreviewUpdate
│   ├── build/              ICompiler / TectonicCompiler / MockCompiler
│   │                       / BuildCoordinator / DiagnosticMapper
│   ├── render/             LatexRenderer / SourceMap
│   ├── validation/         Validator
│   ├── persistence/        ProjectSerializer / ProjectPersistence / SaveCoordinator
│   ├── asset/              AssetManager
│   ├── bibliography/       BibTeX 解析 / BibliographyService
│   ├── template/           TemplateRegistry
│   ├── app/                Qt6 GUI（可选构建）
│   └── cli/                paperforge 命令行
├── tests/                  单元 + 端到端 + 场景回归测试
├── examples/               可提交的示例项目（demo-paper）
├── tools/
│   ├── bin/tectonic        静态 tectonic 二进制（刻意提交，离线构建用）
│   └── tectonic-cache/     固定的 LaTeX bundle 缓存（刻意提交）
├── docs/                   架构与仓库约定文档
├── build/                  构建输出（忽略）
└── generated/              任何生成物（忽略）
```

## 什么必须提交

| 内容 | 位置 |
| --- | --- |
| 源码 / 测试 / CMake | `src/` `tests/` `CMakeLists.txt` |
| 可复现的示例项目 | `examples/**` |
| 离线构建依赖 | `tools/bin/tectonic`、`tools/tectonic-cache/`（刻意保留） |
| 架构文档 | `docs/`、`README.md` |

## 什么绝不提交

| 内容 | 典型路径 | 正确位置 |
| --- | --- | --- |
| 用户实验项目 | 根目录 `project.paper`、`GMVG/` | `/tmp/<name>` |
| 自动保存快照 | `.paperforge/autosave/autosave.paper` | 项目目录内，随项目忽略 |
| 构建工作区 | `.paperforge/build/`、tectonic 运行产物 | `/tmp/paperforge-*-builds` |
| 运行产生的 PDF | `*.pdf`（除测试断言外） | `/tmp` |
| 截图 / 临时抓图 | `Screenshot*.png`、`*.log` | `/tmp` 或 PR 描述 |
| 构建输出 | `build/`、`generated/` | — |

`.gitignore` 已按上面的分类写好。**注意**：如果某次不小心把运行产物提交了，
修正方式是 `git rm -r --cached <path>`（只从索引移除，本地文件保留），
而不是直接删除文件。

## 提交前自检

```bash
git status --short          # 期望：只有源码/测试/文档变更
git diff --stat             # 期望：没有 project.paper / *.pdf / 截图
```

如果 `git diff` 里出现了看起来像数据的 JSON（`project.paper`），先确认它是
`examples/` 下刻意的示例，而不是刚才那次 GUI 运行留下的。
