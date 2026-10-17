# 论文项目

本目录集中存放使用 PaperForge 编辑的论文，与 `src/` 源码和 `examples/` 演示数据分开。

| 项目 | 说明 |
| --- | --- |
| `GMVG2/` | 从仓库根目录迁入的本地空项目目录 |
| `GMVG3/` | 从仓库根目录迁入的已有论文；正文、附件、参考文献和 `.paperforge/` 自动保存均随目录保留 |

在 GUI 中打开 `projects/GMVG3/` 即可继续编辑；命令行查看项目：

```bash
./build/src/paperforge info projects/GMVG3
```

根目录 `GMVG3` 保留为指向 `projects/GMVG3/` 的本地兼容链接，供已打开的编辑器
继续保存；论文实际只存放在新目录中。

新建论文使用 `./build/src/paperforge new projects/<项目名>`。新项目目录默认被
`.gitignore` 忽略；`GMVG3/` 沿用迁移前的版本管理，供审查目录移动和现有正文修改。
各项目的 `.paperforge/` 构建产物与自动保存仍被忽略。
