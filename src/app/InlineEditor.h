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
//
// Citations and cross references (citation plan §1) follow the same
// architecture: each is a real inline object (object replacement character +
// CitationObjectRenderer) showing a compact pill - "[1]", "[1, 3]", "[?]" for
// a citation, the target's label for a reference. The pill is only a visual
// projection of the key set the document stores; numbers come from the
// CitationNumberResolver the window feeds in, never from user text.

#include <QTextEdit>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "app/CitationObjectRenderer.h"
#include "document/Document.h"
#include "numbering/CitationNumberResolver.h"

class QAction;
class QMimeData;
class QTextCursor;

namespace pf::gui {

class InlineMathObjectRenderer;
class CitationObjectRenderer;
class InlineEditor : public QTextEdit {
    Q_OBJECT

public:
    explicit InlineEditor(QWidget* parent = nullptr);
    ~InlineEditor() override;

    // Establish the row's font environment *before* SetContent (UI plan §6):
    // inline math objects measure document()->defaultFont() at insertion
    // time, so the body font and line height must already be in place when
    // the content is loaded, or pills and math keep the old metrics.
    void SetBodyTypography(const QFont& font, int line_height_percent);

    // Load from the document. Never marks the row dirty.
    void SetContent(const InlineContent& content);
    // What the document should store after this edit.
    InlineContent Content() const;

    // Plain text the surrounding UI shows in tooltips/outline.
    QString PlainText() const;

    bool IsDirty() const { return dirty_; }
    bool IsMathEditorOpen() const { return math_editor_open_; }
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

    // Citation-order numbering for the pills (citation plan §3). Ownership
    // is a shared_ptr so every row can resolve display numbers against the
    // same document-wide map; reloading the row repaints the pills.
    void SetCitationNumbers(std::shared_ptr<const CitationNumberResolver> numbers);
    // Node id -> display label for cross-reference pills (the "@" list).
    void SetCrossReferenceLabels(std::map<QString, QString> labels);

    // Format the current selection (or, when the selection is collapsed, turn
    // typing on/off) - the [B] / [I] toolbar and Ctrl+B / Ctrl+I entry points.
    void ToggleBold();
    void ToggleItalic();
    bool IsBoldActive() const;
    bool IsItalicActive() const;

    // Insert a semantic inline object at the cursor. Each builds a real
    // renderable object (citation plan §2): no shared "InsertToken(kind,
    // payload, label)" pseudo interface.
    void InsertCitationObject(const QStringList& keys);
    void InsertCrossReferenceObject(const QString& target_node);
    // Insert an inline math object from a LaTeX body (no delimiters).
    void InsertInlineMath(const QString& latex);
    // Toolbar entry point: ask the user for a math body, then insert it.
    void BeginInlineMath();
    // Open the math editor for the object at `position` and replace it when
    // the user accepts.
    void EditMathAt(int position);

    // A picker (citation / reference popup) is about to take focus and insert
    // an object into this row. While it is open, focusOut must not be
    // mistaken for "the user finished editing the body" (citation plan §4):
    // the popup's focus round trip must not commit the mid-edit state.
    void BeginProtectedInsert() { ++protected_inserts_; }
    void EndProtectedInsert() { protected_inserts_ = qMax(0, protected_inserts_ - 1); }
    bool IsProtectedInsertOpen() const { return protected_inserts_ > 0; }

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
    // ---- semantic inline objects ----
    // Every inline object occupies exactly one character in the QTextDocument
    // - the object replacement character - and paints through a dedicated
    // renderer (math: InlineMathObjectRenderer; citation / cross reference:
    // CitationObjectRenderer). Its identity (kind + payload) lives in the
    // character format, so Backspace/Delete removes the whole object and text
    // editing never touches its interior. kTokenChar is the private-use
    // character the old pill encoding used; it can still arrive via a stale
    // paste and is stripped.
    static constexpr QChar kTokenChar{0xE000};
    static constexpr QChar kObjectChar{0xFFFC};
    static constexpr int kTokenKindProperty =
        inline_object_format::kKindProperty;
    static constexpr int kTokenPayloadProperty =
        inline_object_format::kPayloadProperty;
    // Clipboard type that preserves math objects, marks and semantic tokens.
    static const char* InlineMimeType();

    // Insert a citation/reference pill rendering `display` for `payload`.
    void InsertPillObject(QTextCursor& cursor, TokenKind kind,
                          const QString& payload, const QString& display);
    // Insert a rendered math object carrying its LaTeX body as payload.
    void InsertMathObject(QTextCursor& cursor, const QString& latex);
    // Pill texts resolved against the current numbering / label maps.
    QString CitationDisplayText(const QStringList& keys) const;
    QString CrossReferenceDisplayText(const QString& target) const;
    // Repaint the pills of this row without touching the dirty state or the
    // caret (used when the numbering map or the label set changed).
    void RefreshObjectDisplays();
    // Insert already-structured content at the cursor.
    void InsertContent(const InlineContent& content);
    // The token under the cursor, if any (caret must be *inside* it).
    struct TokenHit {
        TokenKind kind;
        QString payload;
        int position;  // position of the token character
    };
    std::optional<TokenHit> TokenAt(int position) const;
    // Re-apply the stored proportional line height to every block (called
    // after the document was reloaded, under the loading_ guard).
    void ApplyLineHeight();
    void OpenMathEditor(const std::optional<TokenHit>& hit);
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
    bool refreshing_displays_ = false;
    bool math_editor_open_ = false;
    int protected_inserts_ = 0;
    // Body typography (UI plan §5): proportional line height re-applied on
    // every reload so 150% survives SetContent/clear.
    int line_height_percent_ = 0;
    std::unique_ptr<InlineMathObjectRenderer> math_object_renderer_;
    std::unique_ptr<CitationObjectRenderer> citation_object_renderer_;
    std::shared_ptr<const CitationNumberResolver> citation_numbers_;
    std::map<QString, QString> xref_labels_;
};

}  // namespace pf::gui
