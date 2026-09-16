#pragma once
// InlineEditor: edits InlineContent directly (plan §4.1).
//
// The old paragraph row was a QPlainTextEdit over InlineToPlainText(), which
// forced the whole paragraph through the "[cite:key]" string encoding and
// flattened bold/italic entirely. InlineEditor instead keeps a QTextEdit whose
// character format *is* the mark state, and whose semantic nodes (citation,
// cross reference, inline equation) are rendered as read-only tokens inside
// the text. Committing produces the InlineContent the document stores.
//
// Token invariants (plan §5):
//   * the identifier inside a token is not user-editable,
//   * Backspace/Delete removes the whole token,
//   * clicking selects it,
//   * a citation/reference token can be re-picked,
//   * committing, undo and redo preserve the semantic structure.

#include <QTextEdit>

#include <functional>
#include <optional>

#include "document/Document.h"

class QAction;

namespace pf::gui {

class InlineEditor : public QTextEdit {
    Q_OBJECT

public:
    explicit InlineEditor(QWidget* parent = nullptr);

    // Load from the document. Never marks the row dirty.
    void SetContent(const InlineContent& content);
    // What the document should store after this edit.
    InlineContent Content() const;

    // Plain text the surrounding UI shows in tooltips/outline.
    QString PlainText() const;

    bool IsDirty() const { return dirty_; }
    void MarkClean() { dirty_ = false; }
    // Load without touching the dirty flag (used by a programmatic restyle).
    void SetContentClean(const InlineContent& content);

    // Re-fit the widget to its content; call after the width changes.
    void ResizeToContent();
    // Re-fit using a width that has not been applied to the widget yet.
    void ResizeToWidth(int width);

    enum class TokenKind : int { Citation = 1, CrossReference = 2, Equation = 3 };

    // Reference items for the citation / cross-reference pickers.
    // label, detail, payload = citation key or node id.
    struct ReferenceItem {
        QString label;
        QString detail;
        QString payload;
    };
    void SetReferenceItems(std::vector<ReferenceItem> items);

    // Format the current selection (or, when the selection is collapsed, turn
    // typing on/off) - the [B] / [I] toolbar and Ctrl+B / Ctrl+I entry points.
    void ToggleBold();
    void ToggleItalic();
    bool IsBoldActive() const;
    bool IsItalicActive() const;

    // Insert a token at the cursor. Used by the toolbar and by re-picking.
    void InsertCitationToken(const QStringList& keys);
    void InsertCrossReferenceToken(const QString& target_node);
    void InsertInlineEquation(const QString& math);

    signals:
        // The user edited the content (committed on focus-out / Ctrl+Enter).
        void Committed();
    // The user asked to insert a block after this row (Ctrl+Enter).
    void NewBlockAfter();
    // A citation token was activated for re-picking (payload = keys).
    void CitationTokenActivated(const QString& keys);
    // A cross-reference token was activated for re-picking.
    void CrossReferenceTokenActivated(const QString& target);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    // Re-fit on width changes and when the row first becomes visible. Without
    // this the fixed height keeps the value computed before the layout gave
    // the widget its real width, which shows up as a large blank area; typing
    // (or Delete) then re-measures and "restores" it.
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    // ---- token model ----
    // A token occupies exactly one QChar in the text (U+E000 private use),
    // carrying its payload in the character format's anchor/string property.
    static constexpr QChar kTokenChar{0xE000};
    static constexpr int kTokenKindProperty = QTextFormat::UserProperty + 1;
    static constexpr int kTokenPayloadProperty = QTextFormat::UserProperty + 2;

    void InsertToken(TokenKind kind, const QString& payload,
                     const QString& label);
    // The token under the cursor, if any (cursor must be *inside* it).
    struct TokenHit {
        TokenKind kind;
        QString payload;
        int position;  // position of the token character
    };
    std::optional<TokenHit> TokenAt(int position) const;
    // Remove the whole token, wherever the caret is inside it.
    void RemoveTokenAt(int position);
    // The editor text may not contain stray token characters that lost their
    // format (e.g. after a paste from a foreign source).
    void SanitizeTokens();

    void ApplyPaste(const QMimeData* source, bool rich);

    std::vector<ReferenceItem> reference_items_;
    bool dirty_ = false;
    bool loading_ = false;
};

}  // namespace pf::gui
