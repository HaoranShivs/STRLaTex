# 图表题注与表格编辑（WP-04）

Figure/Table 题注和表格单元格直接保存 `InlineContent`，支持粗体、斜体、行内公式、
citation 与交叉引用。此次改动使用现有 v4 文件格式，无需迁移字段。

## 使用

- 在 Figure/Table 的题注中输入或选中文字，使用与正文相同的格式工具栏。
  `Ctrl+Shift+C` 插入文献引用，`Ctrl+Shift+R` 或空白后的 `@` 插入交叉引用。
  已有的公式和引用对象可以重新编辑。
- 在块插入菜单选择 `Table…`，指定行数、列数和是否把首行作为表头。行数包含
  表头行；默认 3×3，最多 100 行、50 列。
- 双击单元格打开编辑弹窗。弹窗内可以编辑文本、公式和引用；按 `Save` 后才修改
  文档，`Cancel`/Esc 放弃本次草稿。打开数学编辑器或引用选择器不会提前提交。
- 在网格中选择单元格，用 `+ Row`/`− Row` 和 `+ Column`/`− Column` 增删行列。
  新行插在当前行下方，新列插在当前列右侧。删除按钮至少保留一行、一列；已到
  尺寸上限时对应新增按钮禁用。
- `Left`/`Center`/`Right` 修改选中列的对齐；`Header row` 切换首行表头。
  网格显示对齐和表头字重，双击后仍可编辑完整富内容。
- 图表的 `Label` 用于 PDF 中的引用，留空时回退到节点 id；`Figure 1`、`Table 1`
  等编号由 `DocumentNumberer` 生成。
- Figure 的 `Width` 可选 25/50/75/100%，相对于单栏或通栏浮动体。宽度选择同时
  应用于卡片预览和生成的 LaTeX。

题注、单元格、行列结构、对齐、表头、label 和图宽都通过文档命令提交，支持撤销、
重做和保存重开。行列操作会先提交正在编辑的题注。

## 实现

- `BlockEditor::NewInlineEditor`：正文和题注共用排版/公式/引用初始化。
- `CaptionContentEdited` → `ProjectController::EditCaptionRich` → `EditCaptionPayload`：
  题注全程传递富内容。
- `InlineContentEditorDialog`：独立的可取消草稿；表格弹窗打开期间保护原卡片，
  异步文档通知只刷新派生信息。引用选择器是该弹窗的子窗口。
- `SetTableCellPayload`、增删行列、列对齐和表头载荷：全部走 `EditingSystem` 和
  `DocumentEditor`，由既有快照历史提供 Undo/Redo。
- `Table::kMaxRows` / `kMaxColumns`：创建、编辑和文件读取的共同边界。
- `Validator`：单元格中的公式和引用与正文同样参与宏包/key 校验。
- `ProjectPersistence`：各层标题均可作为引用目标；使用统一遍历检查正文、题注、
  单元格中的引用，避免正确的章节引用导致保存失败。

## 验证入口

本次验证：完整 CTest 6/6 通过；WP-04 定向核心测试 8/8、GUI 测试 7/7 通过。
同一组 7 项 GUI 测试在 AddressSanitizer 构建下通过（`ASAN_OPTIONS=detect_leaks=0`）。

```sh
cmake --build build --target pf_tests paperforge-inline-editor-test
PF_TEST_FILTER=WP04 ./build/tests/pf_tests
QT_QPA_PLATFORM=offscreen PF_TEST_FILTER=WP04 ./build/src/app/paperforge-inline-editor-test
ctest --test-dir build --output-on-failure
```

`TestTableEditing.cpp` 覆盖矩形不变量、错误目标/边界不产生 revision、富题注与单元格
保存重开、撤销/重做、数学/引用诊断和真实 TeX PDF 构建；`TestCaptionTableGui.cpp`
覆盖窗口级双击、取消/确认、嵌套引用选择器、文档变化时的草稿保护、工具栏和插入
对话框。嵌套标题下的图表投影仍由 `block_projection` 回归测试覆盖。

合并单元格与单元格内嵌编辑属于 WP-08；图片替换、拖放/粘贴及矢量格式已由
WP-05 实现，见 [图片与矢量图](figures-and-assets.md)。
