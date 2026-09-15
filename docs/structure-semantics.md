# Stage A + C：结构语义统一、DocumentTraversal 与三级标题

本文档记录这一阶段落地了什么、为什么这样切，以及每个约定在代码里的位置。
线程/异步模型见 [architecture-async.md](architecture-async.md)，仓库布局见
[REPOSITORY.md](REPOSITORY.md)。

## 1. 语义模型：结构节点与 Block 分离

```
Structure Node            Block
├── Section               ├── Paragraph
├── Subsection            ├── Figure
└── Subsubsection         ├── Table
                          └── DisplayEquation
```

* 领域模型保持这套区分不变；GUI 用 `EditorItemKind`（`src/app/EditorItemKind.h`）
  重新命名表达层：

| GUI 名称 | 领域类型 | 说明 |
| --- | --- | --- |
| Paper Title / Authors / Affiliations / Abstract / Keywords | `FrontMatter` | 不属于正文树 |
| Section Title | `Section` | depth 1 |
| Subsection Title | `Subsection` | depth 2 |
| Subsubsection Title | `Subsubsection` | depth 3 |
| Text | `Paragraph` | 领域模型名不变，只改 GUI 表达 |
| Equation / Figure / Table | `DisplayEquation` / `Figure` / `Table` | |

* `EditorItemKind` 同时提供人读标签（`EditorItemLabel`）和稳定机器名
  （`EditorItemKindName`，如 `"text"`、`"subsubsection"`）。插入菜单与 `/`
  命令菜单的 payload 都用机器名，`MainWindow` 再映射到具体 EditCommand。

## 2. DocumentTraversal：遍历只写一份

之前 Renderer、Validator、DocumentIndex、Outline、删除/移动定位各自维护一套
`for section / for subsection`，块类型的 switch 被手抄了三遍。加入第三层标题
意味着要同时改五处。现在嵌套关系只存在于
`src/document/DocumentTraversal.{h,cpp}`：

```cpp
struct NodeAddress {
    NodeId node;
    NodeKind kind;
    std::optional<size_t> section, subsection, subsubsection, block;
    int depth() const;          // 1..3 for headings, 0 for blocks
    bool is_heading() const;
};

std::optional<NodeAddress> LocateNode(const Document&, const NodeId&);
void VisitNodes(const Document&, fn(const NodeAddress&));
void VisitBlocks(const Document&, fn(const Block&, const NodeAddress&));
void VisitHeadings(const Document&, fn(const NodeAddress&));
void VisitInlineContent(const Document&, fn(const InlineContent&, const NodeAddress&));
std::vector<NodeId> CollectAllNodeIds(const Document&);
```

约定：

* **顺序即文档顺序**：section → 该 section 自己的 blocks → subsection →
  该 subsection 自己的 blocks → subsubsection → 其 blocks。渲染顺序与此一致。
* `LocateNode` 返回的索引是**可变的**（`DocumentEditor` 直接用它定位并修改
  容器），这是 `NodeAddress` 携带 `optional<size_t>` 而不只是 id 的原因。
* 访问 Document 私有容器只有一条路：`DocumentMutableAccess`（也定义在
  DocumentTraversal.h），便于审计"谁能摸到 Document 内部"。

已迁移到 Traversal 的模块：`Document`（ContainsNode/GetNodeKind/
CollectNodeIds）、`DocumentEditor`（Find*/FindBlockPosition/InsertBlock/
DeleteBlock/MoveBlock）、`DocumentIndex`、`Validator`（语义 + 悬空交叉引用 +
标题深度）、`LatexRenderer`（正文与资源收集）、`ProjectController`
（DeleteNode / MoveNodeAfter / DeleteSubsubsection）、`BlockEditor`
（插入菜单容器判定）、`OutlinePanel`。

## 3. Subsubsection（第三级标题）

* `Subsection` 增加 `std::vector<Subsubsection> subsubsections`；`NodeKind`
  增加 `Subsubsection`；`HeadingDepth()`/`IsHeadingKind()` 提供统一层级判断。
* 编辑协议新增 payload（`editing/EditCommand.h`）：
  `InsertSubsubsectionPayload` / `InsertSubsubsectionAfterPayload` /
  `RenameSubsubsectionPayload` / `MoveSubsubsectionPayload` /
  `DeleteSubsubsectionPayload`，全部接入 `EditingSystem` 的快照式 Undo/Redo。
* `DocumentEditor::InsertSubsubsectionAfter` 与 `InsertSubsectionAfter` 同一
  语义：**锚点之后的 blocks 移入新标题**，标题出现在用户要求的位置。
* 序列化：`subsections[].subsubsections[]`（id / title / blocks）；V1 文件没有
  这个字段，由 migration 补齐（见下）。
* Renderer 输出 `\subsubsection{...}\label{node-id}`。
* Outline 显示第三层（斜体），点击可定位。

## 4. 模板能力约束

```cpp
struct TemplateCapabilities { int max_heading_depth = 3; };
```

* GUI：`MainWindow::RefreshDocumentView` 把当前模板的 depth 传给
  `BlockEditor::SetMaxHeadingDepth`；插入菜单与 `/` 菜单据此**按位置**过滤：

| 当前位置 | 可插入 |
| --- | --- |
| 空 body | Section Title |
| Section | Text / Equation / Figure / Import Table / Subsection Title / Subsubsection Title |
| Subsection | Text / Equation / Figure / Import Table / Subsubsection Title |
| Subsubsection | Text / Equation / Figure / Import Table |

* Validator：标题深度超过模板能力时报 `W-HEADING-DEPTH`。
* 两套内置模板都支持 3 层；约束的真正意义在于将来加入只能到 2 层的模板时，
  菜单与校验自动跟随。

## 5. Schema Migration

```
Load project → detect schema version → migrate (in memory) → current Document
```

* `persistence/SchemaVersions.h`：`kSchemaVersionV1 = "1"`、`kSchemaVersion = "2"`
  以及 `MigrationResult`/`MigrationStep`。
* `ProjectMigrator::MigrateToCurrent`：幂等；未知版本给出 warning 并按当前
  版本继续；`LoadResult.migration` 让调用方知道"这个文件是旧 schema"。
* **磁盘文件在加载时绝不被改写**；只有保存才写入当前版本（测试验证）。
* V1 → V2 是无损的：V1 文档没有第三层字段，内存表示天然正确，迁移只是版本
  戳 + 说明。将来若有真正的数据变换（例如字段改名/拆分），在
  `MigrateToCurrent` 里加一个显式 step 并补测试即可。

## 6. 回归测试（tests/TestStructure.cpp）

| 测试 | 覆盖 |
| --- | --- |
| `TraversalLocatesEveryNodeWithItsAddress` | 每个节点都能定位，`NodeAddress` 携带完整坐标 |
| `TraversalVisitsInDocumentOrder` | 访问顺序 == 渲染顺序 |
| `TraversalHeadingsAndBlocksHelpers` | 标题/块分流、`CollectAllNodeIds` 与 Document 一致 |
| `TraversalInlineCoversParagraphsAndCaptions` | 标题 + 正文 + 图题都被 VisitInlineContent 覆盖 |
| `SubsubsectionInsertRenameDeleteThroughProtocol` | 五种 payload 走完 EditingSystem |
| `SubsubsectionAfterAnchorTakesFollowingBlocks` | "之后插入"把下方块移交新标题 |
| `SubsubsectionSurvivesUndoAndRedo` | 快照式 Undo/Redo 保留 id |
| `SubsubsectionBlocksMoveAndValidate` | 块在三级容器间移动后文档仍有效 |
| `RendererEmitsSubsubsectionWithLabel` | `\subsubsection` + `\label` |
| `DocumentIndexCoversThirdLevel` | 第三层坐标与 parent 指向 |
| `TemplateCapabilityLimitsHeadingDepth` / `ValidatorWarnsWhenHeadingExceedsTemplateDepth` | 模板能力 |
| `SchemaMigrationV1ToV2…` / `SchemaMigrationIsIdempotent…` / `LoadMigratesOldProjectFileInMemory` | migration |
| `SubsubsectionRoundTripsThroughProjectFile` | 序列化 id 稳定 |
| `SessionSubsubsectionEndToEnd` | ProjectSession 全链路（含保存/重开） |

## 7. 尚未实施（按方案属于后续 Stage）

* **Stage B**：`InlineEditor`、Bold/Italic、InlineEquation 编辑、Citation/
  CrossReference 结构化 Token、富文本粘贴。
* **Stage D**：xlsx/csv/tsv 导入、Import Table 对话框、TableSource 元数据。
* **Stage E**：TableImportRequest 的后台导入 worker。
  （Build/Save 的 Snapshot→worker→typed event→应用线程 accept 在上一阶段
  已完成，Stage E 只需为表格导入走同一条管道。）
