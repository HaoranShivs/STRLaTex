# 仓库布局约定

源码、测试、示例、个人论文和构建产物各有固定位置，避免运行一次程序就把临时文件
散落到仓库根目录或源码中。

## 目录职责

```text
STRLaTex/
├── CMakeLists.txt           构建入口；禁止在源码目录直接构建
├── README.md               项目说明与常用命令
├── src/                    C++20 源码
│   ├── core/               类型、诊断、JSON、路径边界
│   ├── document/           文档模型、遍历与索引
│   ├── editing/            编辑命令、撤销与锚点
│   ├── project/            会话、快照与异步事件
│   ├── build/              编译器、运行环境与构建调度（这是源码）
│   ├── render/             LaTeX 渲染与 SourceMap
│   ├── validation/         文档校验
│   ├── persistence/        项目存储、迁移与保存调度
│   ├── asset/              附件管理
│   ├── bibliography/       参考文献解析与管理
│   ├── numbering/          引用编号
│   ├── math/               公式模型、校验与生成
│   ├── template/           论文模板
│   ├── app/                Qt6 GUI
│   └── cli/                命令行入口
├── tests/                  单元、集成、GUI 与端到端测试
│   └── runtime/            TeX runtime 手工复现文档
├── examples/               可提交的演示项目
├── projects/               个人论文；新项目默认忽略，见 projects/README.md
├── GMVG3 -> projects/GMVG3  本地兼容链接，供已打开的编辑器继续保存（忽略）
├── docs/                   架构、开发约定与审查报告
├── tools/
│   ├── build-runtime/      可复现构建 Portable TeX Live 的脚本
│   ├── bin/tectonic        离线构建所需二进制（刻意提交）
│   └── tectonic-cache/     固定的 LaTeX bundle 缓存（刻意提交）
├── runtime/                脚本安装的 Portable TeX Live（忽略）
└── build/                  当前默认构建输出（忽略）
    ├── asan/               按需创建的 ASan/UBSan 构建
    ├── tsan/               按需创建的 TSan 构建
    └── _archive/           旧构建与散落生成物的本地归档
```

## 文件放置规则

| 内容 | 位置 | 版本管理 |
| --- | --- | --- |
| 源码、测试、CMake | `src/`、`tests/`、根目录 `CMakeLists.txt` | 提交 |
| 可复现示例 | `examples/` | 提交 |
| 架构与开发文档 | `docs/`、`README.md` | 提交 |
| 离线编译依赖 | `tools/bin/tectonic`、`tools/tectonic-cache/` | 刻意保留 |
| 个人论文 | `projects/<项目名>/` | 新项目默认忽略；已有 `GMVG3/` 沿用原版本管理 |
| 自动保存与论文构建结果 | 项目内 `.paperforge/` | 忽略 |
| 最后一次成功构建的 PDF | 项目根目录 `project.pdf` | 忽略；重新打开时自动预览 |
| C++ 构建输出 | `build/`、`build/<配置>/` | 忽略 |
| TeX 运行环境 | `runtime/` | 忽略，由脚本重建 |
| 截图、日志、一次性实验 | `/tmp/` 或 `generated/` | 忽略 |

构建命令统一在仓库根目录执行：

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

其他配置使用 `-B build/<配置>`，例如 `build/asan`。`src/build/` 是编译管线源码，
与根目录的构建输出 `build/` 用途不同。不要在根目录运行 `cmake .`，也不要在
根目录直接运行 `ctest`；CMake 已加入检查，`.gitignore` 也覆盖了生成文件。

## 2026-10-16 目录整理记录

| 原位置 | 新位置 |
| --- | --- |
| `GMVG2/`、`GMVG3/` | `projects/GMVG2/`、`projects/GMVG3/` |
| `runtime-tests/` | `tests/runtime/` |
| `build-asan/`、`build-audit/`、`build-final/`、`build-gate/` | `build/_archive/2026-10-16-directory-cleanup/old-builds/` 下的同名目录 |
| 根目录 `CMakeFiles/`、`Testing/`，以及 `src/`、`src/app/`、`tests/` 中的 CMake 生成文件 | 同一归档的 `source-tree/` 下，保留原相对路径 |

所有迁移均保留原文件；`GMVG3/project.paper` 原有的未提交修改随项目搬迁。
整理期间旧路径仍有自动保存写入，因此根目录保留 `GMVG3 -> projects/GMVG3` 兼容
链接；新写入的相同自动保存副本也保留在归档的 `late-autosave/` 中。
归档内有 `manifest.json`（移动清单与文件校验信息）和原正文修改的补丁。
当前 `build/` 保持原路径，仍可增量构建。旧构建中的 CMake 缓存和 Ninja 文件含有
原绝对路径，只供留存日志与产物；需要再次构建时请在 `build/<配置>` 重新配置。
如需恢复旧构建，将归档中的对应同名目录移回原位置即可，目标目录必须不存在。
审查报告中的旧路径保留为历史记录。

## 提交前自检

```bash
git status --short
git diff --stat
```

目录移动在尚未暂存时可能显示为原路径删除、新路径未跟踪；审查时对照上面的
迁移表。CMake 生成文件已转入本地归档，提交整理结果后将不再跟踪。
