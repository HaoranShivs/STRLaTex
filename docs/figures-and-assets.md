# 图片与矢量图（WP-05）

## 使用

- 在块插入菜单选择 Figure，可选择 PNG/JPEG/GIF、PDF、SVG 或 EPS。
  PDF 原样存入项目，不会为 LaTeX 导出而光栅化。
- 把本地图片文件拖到卡片或块间隙，会在该节点后插入 Figure；一次拖入多个文件
  时保持原顺序。拖入子编辑器视口同样有效。
- 在编辑区按 `Ctrl+V` 粘贴剪贴板图像，或使用正文/题注编辑器的 Paste，生成
  独立 Figure。已有正文先提交，图片保存到项目后删除临时 PNG。普通文字和富文本
  粘贴继续使用原有路径。正文至少需要一个 Section，空正文会提示先添加章节。
- Figure 卡片的 `Replace image…` 更换图片，保留节点 id、富文本题注、label、
  宽度和跨栏设置。撤销/重做会恢复对应的图片，保存重开后保持替换结果。
- `Width` 的 25/50/75/100% 同时控制卡片与 LaTeX 输出；双栏开关和 label 仍沿用
  WP-03/WP-04 的实现。

## 可选工具

| 格式/功能 | 工具 | 缺失时 |
| --- | --- | --- |
| PDF 导入与 TeX 嵌入 | 无额外转换工具 | 直接保存原 PDF |
| PDF 首页缩略图 | Poppler 的 `pdftoppm`，在 PATH 上 | 卡片显示占位符；PDF 仍可导入和构建 |
| SVG 转 PDF | librsvg 的 `rsvg-convert`，在 PATH 上 | 提示安装工具，或先从绘图软件导出 PDF |
| EPS 转 PDF | `epstopdf` 与 Ghostscript，在 PATH 上 | 提示安装工具，或先导出 PDF |

转换器每次最多等待 15 秒，失败原因显示在状态栏。转换成功后只保存最终 PDF，
重新打开项目和构建不再依赖源 SVG/EPS 或转换工具。安装缩略图工具后重启编辑器
可重新尝试预览。

## 数据与实现

- 领域层保持 Qt-free；`AssetManager` 接受可选 `IAssetConverter`，没有转换器
  时明确拒绝 SVG/EPS。`Stage(request, false)` 可限制为原有位图格式。
- `AssetMetadata::kind` 表示存储文件的格式，从现有 `mediaType` 推导；转换后通常
  为 `Pdf`，`original_name` 保留源文件名。v4 JSON 无需新增字段或迁移。
- 文件名清洗、内容哈希、尺寸嗅探和项目路径校验沿用现有资源链路。转换使用
  独立临时目录，成功与失败均清理。
- `ProjectSession::ApplyImportedAsset` 先执行文档命令，成功后注册资源，再捕获
  构建快照；失败清理暂存文件，不修改历史或 revision。替换仅改变 AssetId，
  旧文件保留给撤销历史。`ProjectController` 只在成功后通知 GUI。
- 图片输入在 Qt 事件结束后处理，避免卡片重建破坏事件接收者；模态单元格/数学
  弹窗不接收这种块插入。粘贴临时文件覆盖完整导入调用，随后自动删除。
- `FigurePreviewRenderer` 异步调用 `pdftoppm`，只渲染第一页；单进程、15 秒超时、
  最长边最多 1600 像素、32 MiB 缓存，按路径/大小/修改时间/目标尺寸区分结果。
  失败也缓存，避免缺少工具时循环重试；完成时仅更新对应图片，不重建文字编辑器。

## 验证

本轮核心定向测试 6/6、GUI 定向测试 6/6 通过；同一组 GUI 测试在
AddressSanitizer 下通过（`ASAN_OPTIONS=detect_leaks=0`）。完整 CTest 6/6 通过，
其中 GUI 测试 116 项、核心测试 222 项、模板测试 14 项，均为零失败。

```sh
cmake --build build --target pf_tests paperforge-inline-editor-test paperforge-gui
PF_TEST_FILTER=WP05 ./build/tests/pf_tests
QT_QPA_PLATFORM=offscreen PF_TEST_FILTER=WP05 ./build/src/app/paperforge-inline-editor-test
ctest --test-dir build --output-on-failure
```

`TestAssetVector.cpp` 覆盖 PDF 原始字节、类型/安全文件名、无转换器提示、转换目录
清理、失败不产生状态变化、替换 Undo/Redo 与持久化，并通过真实 TeX 编译验证
graphicx 使用原始 PDF。`TestFigureGui.cpp` 覆盖 PNG/PDF 多文件拖放、子视口定位、
快捷键和编辑器 Paste、临时文件清理、替换对话框、卡片预览/图宽及保存重开、异步
缓存、工具缺失、进程参数/失败/超时。

裁剪、浮动位置和子图仍待后续工作包。
