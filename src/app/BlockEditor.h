#pragma once
// BlockEditor：把文档呈现为垂直堆叠的 block 卡片（设计 #3-#8、#61-#62）。
// 每个 block 显示仅在悬停时出现的头部（拖拽手柄 + 类型标签 + 操作按钮）、
// 聚焦时左侧的强调线，以及自适应高度的编辑器。输入 "/" 打开 block 命令
// 弹窗（设计 #6）。正文、引用和交叉引用都通过富 InlineEditor 路径提交
// （引用方案 §5）。
//
// 视觉状态（设计 #4、UI 方案 §9——统一在 UpdateCardState 中处理）：
//   idle     - 纯白，无可见装饰，仅内容
//   hover    - #FAFBFC 淡底；出现手柄 + 类型标签 + 更多按钮
//   focused  - 左边缘 2px 强调线，背景接近白色
//   missing  - 2px 错误线 + 淡淡的 ErrorSoft 底色（必填但为空）
//   problem  - 2px 强调线 + 短暂的 AccentSoft 闪烁（RevealNode）
// Text 行的类型标签仅在悬停/聚焦时显示（UI 方案 §2）；Abstract 与
// Keywords 保留一个小号大写身份标签（UI 方案 §8）。

#include <QScrollArea>
#include <QWidget>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "app/PopupList.h"
#include "app/Theme.h"
#include "document/Document.h"
#include "document/DocumentTraversal.h"
#include "numbering/CitationNumberResolver.h"

class QPlainTextEdit;
class QVBoxLayout;

#include "app/InlineEditor.h"

namespace pf::gui {

class BlockEditor : public QWidget {
  Q_OBJECT

public:
  explicit BlockEditor(QWidget *parent = nullptr);

  // 用文档内容替换所有行；保留焦点/光标。
  void RebuildFromDocument(const Document &doc);

  // 当前模板支持的最大标题层级（方案 §9）。控制插入菜单和 "/" 菜单是否
  // 提供 Subsubsection Title。0 = 不限制。
  void SetMaxHeadingDepth(int depth) { max_heading_depth_ = depth; }

  struct RequiredHints {
    bool title = false;
    bool authors = false;
    bool affiliations = false;
    bool abstract_text = false;
    bool keywords = false;
  };
  void SetRequiredHints(const RequiredHints &hints);

  // 滚动使给定节点的 block 可见，并短暂高亮它。
  void RevealNode(const QString &node_id);

  // "@" 弹窗的引用条目（label、detail、payload=key|node）。
  void SetReferenceItems(std::vector<PopupList::Item> items);

  // 文档级引用编号（引用方案 §3）。Text 行中的 pill 是此映射的视觉投影；
  // 文档本身仍然只存储 key。用 shared_ptr 是因为每一行都读取同一份映射。
  void
  SetCitationNumbers(std::shared_ptr<const pf::CitationNumberResolver> numbers);

  // 把文档中的 AssetId 解析为本地图片路径，用于图片预览。
  void SetAssetPathResolver(std::function<QString(const AssetId &)> resolver);

  std::optional<QString> FocusedNodeId() const;

  // 当聚焦行中存有用户已输入但尚未提交到文档的文本时为 true。此时调用方
  // 不得重建行，否则正在输入的内容会被销毁。
  bool HasUncommittedFocus() const;

  // 依据当前模板提示重新设置行样式，不触碰编辑器。
  void RefreshHints();

  // 立即提交聚焦行（用于结构性编辑和 build 之前）。
  void CommitFocused();

signals:
  // 字段编辑（在失焦 / 回车时提交）。
  void TitleEdited(const QString &text);
  void AuthorsEdited(const QString &text);
  void AffiliationsEdited(const QString &text);
  void AbstractEdited(const QString &text);
  void KeywordsEdited(const QString &text);
  // 来自 InlineEditor 行的富文本提交：整个 InlineContent 以结构化形式到达
  // （mark、citation、reference、行内公式）。这是唯一的正文数据路径
  // （引用方案 §5）——旧的纯文本 ParagraphEdited/[cite:key] 编码已移除。
  void ParagraphContentEdited(const QString &node_id,
                              const InlineContent &content);
  void EquationEdited(const QString &node_id, const QString &math,
                      bool numbered, const QString &label);
  void SectionRenamed(const QString &node_id, const QString &text);
  void SubsectionRenamed(const QString &node_id, const QString &text);
  void SubsubsectionRenamed(const QString &node_id, const QString &text);
  void CaptionEdited(const QString &node_id, const QString &text);
  // 图片布局：true = 图片横跨双栏模板的两栏（无论当前模板如何都会存储）。
  void FigureSpanChanged(const QString &node_id, bool double_column);

  // 在行提交其文本后发出（无论是否改变）。让窗口可以执行在用户仍在输入时
  // 被推迟的刷新。
  void RowCommitted();
  // 光标所在的行发生了移动（UI 方案 §10）。key 指向拥有该行的 Outline 条目：
  // 最内层标题的 node id，或摘要行的 "front:abstract"，或空字符串（outline
  // 不显示的行）。MainWindow 将其转发给 OutlinePanel::SelectNode，使 outline
  // 始终告知用户正在编辑哪一节。
  void FocusOutlineChanged(const QString &outline_key);
  // 作者 <-> 机构绑定从图形面板发生变化。
  void AuthorAffiliationToggled(int author_index, const QString &affiliation_id,
                                bool linked);
  // 拖放重排：把 `node_id` 直接放到 `anchor` 之后。
  void MoveBlockToRequested(const QString &node_id, const QString &anchor);

  // 来自 "/" 菜单、block 之间的 "+" 以及 block 菜单的结构操作。
  void InsertBlockRequested(const QString &block_type,
                            const QString &after_node);
  void DeleteBlockRequested(const QString &node_id);
  void MoveBlockRequested(const QString &node_id, int direction); // -1 / +1

public:
  // 针对某一正文段落的语义化引用插入，供工具栏选择器和 OutlinePanel 双击
  // 使用。它经由该行的 InlineEditor 立即提交，因此唯一的数据路径是
  // InlineEditor -> ParagraphContentEdited -> EditParagraphRich
  // （引用方案 §4/§5）。当该行不是 Text 行时返回 false。
  bool InsertCitationIntoParagraph(const QString &node_id,
                                   const QString &citation_key,
                                   int insert_offset = -1);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  struct Block {
    QString node_id; // 对于 front-matter 字段为空
    QString kind;    // Paper Title/Authors/…/Section Title/
                     // Subsection Title/Subsubsection Title/Text/
                     // Equation/Figure/Table
    QWidget *card = nullptr;
    QPlainTextEdit *editor = nullptr;      // 仅 figure/table 的 block 为 null
    InlineEditor *inline_editor = nullptr; // 在 Text 行上设置
    QString commit_role;                   // 提交时发出哪个信号
    // 该行在文档中最后一次已知的文本。会写入相同值的提交被丢弃，正是这样
    // 才能避免程序化重设样式陷入 edit -> rebuild -> edit 的循环。
    QString committed_text;
    // 文档为某个 Text 行保存的内容，保留它以便在内容未变时跳过提交，而无需
    // 把 mark/token 压平。
    pf::InlineContent committed_content;
    // Equation 行携带紧邻源文本的属性。
    bool equation_numbered = true;
    QString equation_label;
    bool required = false;
    // 拥有此行的 Outline 条目（标题 node id、"front:abstract" 或空）；
    // 当该行获得焦点时随 FocusOutlineChanged 一起发出。
    QString outline_key;
  };

  // Text 行：基于 InlineContent 的 InlineEditor，带格式工具栏。
  QWidget *MakeTextCard(const QString &node_id, const InlineContent &content,
                        const QString &outline_key);
  // Equation 行：LaTeX 源码 + 预览 + 编号 + 标签（设计 §4）。
  QWidget *MakeEquationCard(const QString &node_id,
                            const pf::EquationBlock &equation,
                            const QString &outline_key);
  // Figure 行：题注编辑器、图片预览和跨栏开关（P0-05）。
  QWidget *MakeFigureCard(const pf::Figure &figure, const QString &outline_key);
  // Table 行：网格预览加题注编辑器（P0-05）。
  QWidget *MakeTableCard(const pf::Table &table, const QString &outline_key);
  // P0-05：为每个标题层级提供唯一的 block 卡片工厂和唯一的追加路径。
  // 此前 Section 循环知道全部四种 block 类型，而 Subsection/Subsubsection
  // 循环只处理 Paragraph 和 Equation，因此嵌套在 Section 之下的 Figure 或
  // Table 根本不会渲染。
  QWidget *CreateBlockCard(const pf::Block &block, const QString &outline_key);
  void AppendBlocks(const std::vector<pf::Block> &blocks,
                    const QString &outline_key);
  // 文本行聚焦时显示的 [B] [I] [Inline Math] [Citation] [Reference] 条带
  // （方案 §4.4）。
  QWidget *BuildFormatToolbar(InlineEditor *editor);
  // Citation / Reference 按钮背后的引用选择器。选择器在插入对象后立即提交
  // 该行（引用方案 §4）——而不是「以后某次 focusOut 会提交它」。
  void ShowCitationPicker(InlineEditor *editor);
  void ShowReferencePicker(InlineEditor *editor);
  // 交叉引用 pill 的 node id -> 显示标签，来自当前的 "@" 引用条目。
  std::map<QString, QString> CrossReferenceLabels() const;

  QWidget *MakeCard(const QString &node_id, const QString &kind,
                    const QString &commit_role, bool header_inline);
  // 编辑器窗格宽于正文栏时，使正文栏保持居中并限制在主题的内容宽度内。
  void CenterContentColumn();
  void AddEditorToCard(QWidget *card, QPlainTextEdit *edit);
  QPlainTextEdit *NewEditor(QWidget *card, const QString &text, int min_lines,
                            theme::BlockVisualRole role,
                            bool single_line = false);
  // 依据卡片的 card_hover / card_focus / card_missing / card_flash 属性重新
  // 组合其状态线 / 背景和头部装饰（UI 方案 §9：使用统一状态机，而非各处
  // 临时拼凑的样式表）。
  void UpdateCardState(QWidget *card);
  void CommitBlock(Block &block);
  // 立即提交 Text 行的富内容（供选择器使用）。
  void CommitInlineRow(InlineEditor *editor);
  // 柔化某一行中粘贴段落的硬换行并提交结果（供 block 菜单的
  // "Reflow Text" 使用）。
  void ReflowRow(QWidget *card, const QString &node_id);
  // block 之间的悬停提示：创建条带及其打开的菜单。
  QWidget *MakeGap(const QString &anchor);
  void ShowInsertMenu(const QString &anchor, QWidget *source);
  // Authors 卡片下每位作者一行：选择机构。
  void BuildAuthorBindingPanel(QWidget *card, const FrontMatter &front);
  void OpenSlashMenu(QPlainTextEdit *origin);
  void ApplyHints();

  QScrollArea *scroll_;
  QWidget *host_;
  QVBoxLayout *layout_;
  std::vector<Block> blocks_;
  RequiredHints hints_;
  std::vector<PopupList::Item> reference_items_;
  std::shared_ptr<const pf::CitationNumberResolver> citation_numbers_;
  std::function<QString(const AssetId &)> asset_path_resolver_;
  // 插入菜单用于查询锚点所在容器的文档。仅在 RebuildFromDocument 驱动的
  // 使用期间有效；每次重建时刷新。
  const Document *container_document_ = nullptr;
  int max_heading_depth_ = 3;
  bool rebuilding_ = false;

  // 跨重建的焦点保持。
  QString focus_node_;
  int focus_pos_ = 0;
};

} // namespace pf::gui
