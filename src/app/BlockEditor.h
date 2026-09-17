#pragma once
// BlockEditor: the document as a vertical stack of block cards (design
// #3-#8, #61-#62).
// Each block shows a hover-only header (drag handle + type label + actions),
// a focused accent line on the left, and a self-sizing editor. "/" opens the
// block command popup (design #6). Body text, citations and references all
// commit through the rich InlineEditor path (citation plan §5).
//
// Visual states (design #4, UI plan §9 - unified in UpdateCardState):
//   idle     - pure white, no visible chrome, just content
//   hover    - #FAFBFC wash; handle + type label + more button appear
//   focused  - 2px accent line on the left edge, background near-white
//   missing  - 2px error line + faint ErrorSoft wash (required-but-empty)
//   problem  - 2px accent line + brief AccentSoft flash (RevealNode)
// The Text row's type label is hover/focus-only (UI plan §2); Abstract and
// Keywords keep a small uppercase identity label (UI plan §8).

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

  // Replace all rows with the document's content; preserves focus/cursor.
  void RebuildFromDocument(const Document &doc);

  // Heading depth the current template supports (plan §9). Controls whether
  // the insert and "/" menus offer Subsubsection Title. 0 = unrestricted.
  void SetMaxHeadingDepth(int depth) { max_heading_depth_ = depth; }

  struct RequiredHints {
    bool title = false;
    bool authors = false;
    bool affiliations = false;
    bool abstract_text = false;
    bool keywords = false;
  };
  void SetRequiredHints(const RequiredHints &hints);

  // Scroll so the given node's block is visible, briefly highlight it.
  void RevealNode(const QString &node_id);

  // Reference items for the "@" popup (label, detail, payload=key|node).
  void SetReferenceItems(std::vector<PopupList::Item> items);

  // Document-wide citation numbering (citation plan §3). The pills in the
  // Text rows are the visual projection of this map; the document still
  // only stores keys. Shared_ptr because every row reads the same map.
  void
  SetCitationNumbers(std::shared_ptr<const pf::CitationNumberResolver> numbers);

  // Resolve a document AssetId to a local image path for figure previews.
  void SetAssetPathResolver(std::function<QString(const AssetId &)> resolver);

  std::optional<QString> FocusedNodeId() const;

  // True while the focused row holds text the user has typed but that has
  // not been committed to the document yet. Callers must not rebuild the
  // rows while this is true, or the in-progress input would be destroyed.
  bool HasUncommittedFocus() const;

  // Restyle rows from the current template hints without touching editors.
  void RefreshHints();

  // Commit the focused row now (used before structural edits and builds).
  void CommitFocused();

signals:
  // Field edits (committed on focus-out / Enter).
  void TitleEdited(const QString &text);
  void AuthorsEdited(const QString &text);
  void AffiliationsEdited(const QString &text);
  void AbstractEdited(const QString &text);
  void KeywordsEdited(const QString &text);
  // Rich commit from an InlineEditor row: the whole InlineContent arrives
  // structured (marks, citations, references, inline equations). This is
  // the only body-text data path (citation plan §5) - the old plain
  // ParagraphEdited/[cite:key] encoding is gone.
  void ParagraphContentEdited(const QString &node_id,
                              const InlineContent &content);
  void EquationEdited(const QString &node_id, const QString &math,
                      bool numbered, const QString &label);
  void SectionRenamed(const QString &node_id, const QString &text);
  void SubsectionRenamed(const QString &node_id, const QString &text);
  void SubsubsectionRenamed(const QString &node_id, const QString &text);
  void CaptionEdited(const QString &node_id, const QString &text);
  // Figure layout: true = the figure spans both columns of a two-column
  // template (stored regardless of the current template).
  void FigureSpanChanged(const QString &node_id, bool double_column);

  // Emitted after a row commits its text (changed or not). Lets the window
  // run a refresh it deferred while the user was still typing.
  void RowCommitted();
  // The row under the caret moved (UI plan §10). The key addresses the
  // Outline entry that owns the row: the innermost heading node id, or
  // "front:abstract" for the abstract row, or empty for rows the outline
  // does not show. MainWindow forwards this to OutlinePanel::SelectNode so
  // the outline always tells the user which section is being edited.
  void FocusOutlineChanged(const QString &outline_key);
  // Author <-> institution binding changed from the graphical panel.
  void AuthorAffiliationToggled(int author_index, const QString &affiliation_id,
                                bool linked);
  // Drag-and-drop reorder: put `node_id` directly after `anchor`.
  void MoveBlockToRequested(const QString &node_id, const QString &anchor);

  // Structure ops from "/" menu, "+" between blocks, and block menus.
  void InsertBlockRequested(const QString &block_type,
                            const QString &after_node);
  void DeleteBlockRequested(const QString &node_id);
  void MoveBlockRequested(const QString &node_id, int direction); // -1 / +1

public:
  // Semantic citation insertion for one paragraph row, used by the toolbar
  // picker and by OutlinePanel double-clicks. It goes through the row's
  // InlineEditor and commits immediately, so the only data path is
  // InlineEditor -> ParagraphContentEdited -> EditParagraphRich
  // (citation plan §4/§5). Returns false when the row is not a Text row.
  bool InsertCitationIntoParagraph(const QString &node_id,
                                   const QString &citation_key,
                                   int insert_offset = -1);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  struct Block {
    QString node_id; // empty for front-matter fields
    QString kind;    // Paper Title/Authors/…/Section Title/
                     // Subsection Title/Subsubsection Title/Text/
                     // Equation/Figure/Table
    QWidget *card = nullptr;
    QPlainTextEdit *editor = nullptr;      // null for figure/table-only blocks
    InlineEditor *inline_editor = nullptr; // set on Text rows
    QString commit_role;                   // which signal to emit on commit
    // Text last known to be in the document for this row. Commits that
    // would write the same value are dropped, which is what keeps a
    // programmatic restyle from cycling into edit -> rebuild -> edit.
    QString committed_text;
    // What the document holds for a Text row, kept so an unchanged commit
    // can be skipped without flattening marks/tokens.
    pf::InlineContent committed_content;
    // Equation rows carry the attributes that live next to the source.
    bool equation_numbered = true;
    QString equation_label;
    bool required = false;
    // Outline entry that owns this row (heading node id, "front:abstract"
    // or empty); emitted with FocusOutlineChanged when the row is focused.
    QString outline_key;
  };

  // A Text row: InlineEditor over InlineContent, with the format toolbar.
  QWidget *MakeTextCard(const QString &node_id, const InlineContent &content,
                        const QString &outline_key);
  // An Equation row: LaTeX source + preview + numbered + label (design §4).
  QWidget *MakeEquationCard(const QString &node_id,
                            const pf::EquationBlock &equation,
                            const QString &outline_key);
  // The [B] [I] [Inline Math] [Citation] [Reference] strip shown while a
  // text row is focused (plan §4.4).
  QWidget *BuildFormatToolbar(InlineEditor *editor);
  // Reference pickers behind the Citation / Reference buttons. The picker
  // commits the row immediately after inserting the object (citation plan
  // §4) - no "some later focusOut will submit it".
  void ShowCitationPicker(InlineEditor *editor);
  void ShowReferencePicker(InlineEditor *editor);
  // Node id -> display label for cross-reference pills, from the current
  // "@" reference items.
  std::map<QString, QString> CrossReferenceLabels() const;

  QWidget *MakeCard(const QString &node_id, const QString &kind,
                    const QString &commit_role, bool header_inline);
  // Keeps the manuscript column centered and capped at the theme's content
  // width when the editor pane is wider than the column.
  void CenterContentColumn();
  void AddEditorToCard(QWidget *card, QPlainTextEdit *edit);
  QPlainTextEdit *NewEditor(QWidget *card, const QString &text, int min_lines,
                            theme::BlockVisualRole role,
                            bool single_line = false);
  // Recompose a card's state line / background and header chrome from its
  // card_hover / card_focus / card_missing / card_flash properties (UI
  // plan §9: one state machine instead of ad-hoc per-site stylesheets).
  void UpdateCardState(QWidget *card);
  void CommitBlock(Block &block);
  // Immediately commit a Text row's rich content (used by the pickers).
  void CommitInlineRow(InlineEditor *editor);
  // Softens the hard line breaks of a pasted paragraph in one row and
  // commits the result (used by the block menu's "Reflow Text").
  void ReflowRow(QWidget *card, const QString &node_id);
  // Hover affordance between blocks: creates the strip and the menu it opens.
  QWidget *MakeGap(const QString &anchor);
  void ShowInsertMenu(const QString &anchor, QWidget *source);
  // One row per author under the Authors card: pick the institutions.
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
  // Document the insert menus consult for the container of an anchor. Only
  // valid during RebuildFromDocument-driven use; refreshed on each rebuild.
  const Document *container_document_ = nullptr;
  int max_heading_depth_ = 3;
  bool rebuilding_ = false;

  // Focus preservation across rebuilds.
  QString focus_node_;
  int focus_pos_ = 0;
};

} // namespace pf::gui
