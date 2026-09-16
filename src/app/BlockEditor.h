#pragma once
// BlockEditor: the document as a vertical stack of block cards (design
// #3-#8, #61-#62).
// Each block shows a hover-only header (drag handle + type label + actions),
// a focused accent line on the left, and a self-sizing editor. "/" opens the
// block command popup, "@" opens the reference popup (design #6, #9).
//
// Visual states (design #4):
//   idle    - no visible chrome, just content
//   hover   - handle + type label + more button appear (space pre-reserved)
//   focused - 2px accent line on the left edge
//   missing - red dashed left edge for required-but-empty (template hints)

#include <QScrollArea>
#include <QWidget>
#include <functional>
#include <optional>

#include "app/PopupList.h"
#include "document/Document.h"
#include "document/DocumentTraversal.h"

class QPlainTextEdit;
class QVBoxLayout;

#include "app/InlineEditor.h"

namespace pf::gui {

class BlockEditor : public QWidget {
    Q_OBJECT

public:
    explicit BlockEditor(QWidget* parent = nullptr);

    // Replace all rows with the document's content; preserves focus/cursor.
    void RebuildFromDocument(const Document& doc);

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
    void SetRequiredHints(const RequiredHints& hints);

    // Scroll so the given node's block is visible, briefly highlight it.
    void RevealNode(const QString& node_id);

    // Reference items for the "@" popup (label, detail, payload=key|node).
    void SetReferenceItems(std::vector<PopupList::Item> items);

    // Resolve a document AssetId to a local image path for figure previews.
    void SetAssetPathResolver(
        std::function<QString(const AssetId&)> resolver);

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
    void TitleEdited(const QString& text);
    void AuthorsEdited(const QString& text);
    void AffiliationsEdited(const QString& text);
    void AbstractEdited(const QString& text);
    void KeywordsEdited(const QString& text);
    void ParagraphEdited(const QString& node_id, const QString& text);
    // Rich commit from an InlineEditor row: the whole InlineContent arrives
    // structured (marks, citations, references, inline equations).
    void ParagraphContentEdited(const QString& node_id,
                                const InlineContent& content);
    void EquationEdited(const QString& node_id, const QString& math,
                        bool numbered, const QString& label);
    void SectionRenamed(const QString& node_id, const QString& text);
    void SubsectionRenamed(const QString& node_id, const QString& text);
    void SubsubsectionRenamed(const QString& node_id, const QString& text);
    void CaptionEdited(const QString& node_id, const QString& text);

    // Emitted after a row commits its text (changed or not). Lets the window
    // run a refresh it deferred while the user was still typing.
    void RowCommitted();
    // Author <-> institution binding changed from the graphical panel.
    void AuthorAffiliationToggled(int author_index, const QString& affiliation_id,
                                  bool linked);
    // Drag-and-drop reorder: put `node_id` directly after `anchor`.
    void MoveBlockToRequested(const QString& node_id, const QString& anchor);

    // Structure ops from "/" menu, "+" between blocks, and block menus.
    void InsertBlockRequested(const QString& block_type, const QString& after_node);
    void DeleteBlockRequested(const QString& node_id);
    void MoveBlockRequested(const QString& node_id, int direction);  // -1 / +1
    void InsertCitationRequested(const QString& paragraph_node, const QString& key);
    void InsertCrossRefRequested(const QString& paragraph_node, const QString& target_node);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Block {
        QString node_id;      // empty for front-matter fields
        QString kind;         // Paper Title/Authors/…/Section Title/
                              // Subsection Title/Subsubsection Title/Text/
                              // Equation/Figure/Table
        QWidget* card = nullptr;
        QPlainTextEdit* editor = nullptr;  // null for figure/table-only blocks
        InlineEditor* inline_editor = nullptr;  // set on Text rows
        QString commit_role;  // which signal to emit on commit
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
    };

    // A Text row: InlineEditor over InlineContent, with the format toolbar.
    QWidget* MakeTextCard(const QString& node_id, const InlineContent& content);
    // An Equation row: LaTeX source + preview + numbered + label (design §4).
    QWidget* MakeEquationCard(const QString& node_id,
                              const pf::EquationBlock& equation);
    // The [B] [I] [Inline Math] [Citation] [Reference] strip shown while a
    // text row is focused (plan §4.4).
    QWidget* BuildFormatToolbar(InlineEditor* editor);
    // Reference pickers behind the Citation / Reference buttons.
    void ShowCitationPicker(InlineEditor* editor);
    void ShowReferencePicker(InlineEditor* editor);

    QWidget* MakeCard(const QString& node_id, const QString& kind,
                      const QString& commit_role, bool header_inline);
    void AddEditorToCard(QWidget* card, QPlainTextEdit* edit);
    QPlainTextEdit* NewEditor(QWidget* card, const QString& text, bool mono,
                              int min_lines, bool single_line = false);
    void CommitBlock(Block& block);
    // Softens the hard line breaks of a pasted paragraph in one row and
    // commits the result (used by the block menu's "Reflow Text").
    void ReflowRow(QWidget* card, const QString& node_id);
    // Hover affordance between blocks: creates the strip and the menu it opens.
    QWidget* MakeGap(const QString& anchor);
    void ShowInsertMenu(const QString& anchor, QWidget* source);
    // One row per author under the Authors card: pick the institutions.
    void BuildAuthorBindingPanel(QWidget* card, const FrontMatter& front);
    void OpenSlashMenu(QPlainTextEdit* origin);
    void OpenAtMenu(QPlainTextEdit* origin);
    void ApplyHints();

    QScrollArea* scroll_;
    QWidget* host_;
    QVBoxLayout* layout_;
    std::vector<Block> blocks_;
    RequiredHints hints_;
    std::vector<PopupList::Item> reference_items_;
    std::function<QString(const AssetId&)> asset_path_resolver_;
    // Document the insert menus consult for the container of an anchor. Only
    // valid during RebuildFromDocument-driven use; refreshed on each rebuild.
    const Document* container_document_ = nullptr;
    int max_heading_depth_ = 3;
    bool rebuilding_ = false;

    // Focus preservation across rebuilds.
    QString focus_node_;
    int focus_pos_ = 0;
};

}  // namespace pf::gui
