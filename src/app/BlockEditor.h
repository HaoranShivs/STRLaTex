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

class QPlainTextEdit;
class QVBoxLayout;

namespace pf::gui {

class BlockEditor : public QWidget {
    Q_OBJECT

public:
    explicit BlockEditor(QWidget* parent = nullptr);

    // Replace all rows with the document's content; preserves focus/cursor.
    void RebuildFromDocument(const Document& doc);

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

    std::optional<QString> FocusedNodeId() const;

signals:
    // Field edits (committed on focus-out / Enter).
    void TitleEdited(const QString& text);
    void AuthorsEdited(const QString& text);
    void AffiliationsEdited(const QString& text);
    void AbstractEdited(const QString& text);
    void KeywordsEdited(const QString& text);
    void ParagraphEdited(const QString& node_id, const QString& text);
    void EquationEdited(const QString& node_id, const QString& math);
    void SectionRenamed(const QString& node_id, const QString& text);
    void SubsectionRenamed(const QString& node_id, const QString& text);
    void CaptionEdited(const QString& node_id, const QString& text);

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
        QString kind;         // Title/Authors/Institution/Abstract/Keywords/
                              // Section/Subsection/Paragraph/Equation/Figure/Table
        QWidget* card = nullptr;
        QPlainTextEdit* editor = nullptr;  // null for figure/table-only blocks
        QString commit_role;  // which signal to emit on commit
        bool required = false;
    };

    QWidget* MakeCard(const QString& node_id, const QString& kind,
                      const QString& commit_role, bool header_inline);
    void AddEditorToCard(QWidget* card, QPlainTextEdit* edit);
    QPlainTextEdit* NewEditor(QWidget* card, const QString& text, bool mono,
                              int min_lines);
    void CommitBlock(Block& block);
    void OpenSlashMenu(QPlainTextEdit* origin);
    void OpenAtMenu(QPlainTextEdit* origin);
    void ApplyHints();

    QScrollArea* scroll_;
    QWidget* host_;
    QVBoxLayout* layout_;
    std::vector<Block> blocks_;
    RequiredHints hints_;
    std::vector<PopupList::Item> reference_items_;
    bool rebuilding_ = false;

    // Focus preservation across rebuilds.
    QString focus_node_;
    int focus_pos_ = 0;
};

}  // namespace pf::gui
