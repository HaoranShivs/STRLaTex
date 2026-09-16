#pragma once
// InlineEditor: edits InlineContent directly (plan §4.1).
//
// The old paragraph row was a QPlainTextEdit over InlineToPlainText(), which
// forced the whole paragraph through the "[cite:key]" string encoding and
// flattened bold/italic entirely. InlineEditor instead keeps a QTextEdit whose
// character format *is* the mark state, and whose semantic nodes (citation,
// cross reference, inline math) are rendered as read-only objects inside the
// text. Committing produces the InlineContent the document stores.
//
// Token invariants (plan §5):
//   * the identifier inside a token is not user-editable,
//   * Backspace/Delete removes the whole token,
//   * clicking selects it,
//   * a citation/reference token can be re-picked,
//   * committing, undo and redo preserve the semantic structure.
//
// Inline math (math-input design §3): the token is an inline preview object.
// Double-clicking it (or the toolbar's Inline Math action) opens the math
// editor, which shows the LaTeX body and a live preview; the stored document
// value is always the bare body - the \(...\) delimiters are generated later.

#include <QTextEdit>

#include <functional>
#include <memory>
#include <optional>

#include "document/Document.h"

class QAction;
class QMimeData;
class QTextCursor;

namespace pf::gui {

class InlineMathObjectRenderer;

class InlineEditor : public QTextEdit {
    Q_OBJECT

public:
    explicit InlineEditor(QWidget* parent = nullptr);
    ~InlineEditor() override;

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

    enum class TokenKind : int { Citation = 1, CrossReference = 2, Math = 3 };

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
    // Insert an inline math object from a LaTeX body (no delimiters).
    void InsertInlineMath(const QString& latex);
    // Toolbar entry point: ask the user for a math body, then insert it.
    void BeginInlineMath();
    // Open the math editor for the object at `position` and replace it when
    // the user accepts.
    void EditMathAt(int position);

    // Test seams for the private clipboard flavour that carries marks, tokens
    // and math objects across copy/paste.
    QMimeData* MimeDataForSelection() const {
        return createMimeDataFromSelection();
    }
    void InsertMimeDataForTest(const QMimeData* data) {
        insertFromMimeData(data);
    }

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
    QMimeData* createMimeDataFromSelection() const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    // ---- token model ----
    // A token occupies exactly one QChar: the private-use marker for a
    // citation/reference pill, or the object replacement character for an
    // inline math preview image. Its payload lives in the char format.
    static constexpr QChar kTokenChar{0xE000};
    static constexpr QChar kObjectChar{0xFFFC};
    static constexpr int kTokenKindProperty = QTextFormat::UserProperty + 1;
    static constexpr int kTokenPayloadProperty = QTextFormat::UserProperty + 2;
    // Clipboard type that preserves math objects, marks and semantic tokens.
    static const char* InlineMimeType();

    void InsertToken(QTextCursor& cursor, TokenKind kind,
                     const QString& payload, const QString& label);
    // Insert a rendered math object carrying its LaTeX body as payload.
    void InsertMathObject(QTextCursor& cursor, const QString& latex);
    // Insert already-structured content at the cursor.
    void InsertContent(const InlineContent& content);
    // The token under the cursor, if any (caret must be *inside* it).
    struct TokenHit {
        TokenKind kind;
        QString payload;
        int position;  // position of the token character
    };
    std::optional<TokenHit> TokenAt(int position) const;
    // Remove the whole token starting at `position`.
    void RemoveTokenAt(int position);
    // The editor text may not contain stray token characters that lost their
    // format (e.g. after a paste from a foreign source).
    void SanitizeTokens();
    // Extract structured content from a [begin, end) range of the document.
    InlineContent ContentInRange(int begin, int end) const;
    std::vector<ReferenceItem> reference_items_;
    bool dirty_ = false;
    bool loading_ = false;
    std::unique_ptr<InlineMathObjectRenderer> math_object_renderer_;
};

}  // namespace pf::gui
