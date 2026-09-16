#include "app/InlineEditor.h"

#include <QApplication>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextFragment>
#include <QMimeData>
#include <QMouseEvent>

#include "app/Theme.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
constexpr int kKindCitation = static_cast<int>(InlineEditor::TokenKind::Citation);
constexpr int kKindReference =
    static_cast<int>(InlineEditor::TokenKind::CrossReference);
constexpr int kKindEquation = static_cast<int>(InlineEditor::TokenKind::Equation);
}  // namespace

InlineEditor::InlineEditor(QWidget* parent) : QTextEdit(parent) {
    setAcceptRichText(false);
    setWordWrapMode(QTextOption::WordWrap);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(6);
    connect(this, &QTextEdit::textChanged, this, [this]() {
        SanitizeTokens();
        ResizeToContent();
        if (!loading_) dirty_ = true;
    });
    ResizeToContent();
}

void InlineEditor::ResizeToContent() {
    // Measure against the width the editor actually has. viewport() width is
    // transient - it collapses while the widget is being laid out or has lost
    // focus during a toolbar click - and measuring against 1px wraps every
    // word onto its own line, which is the "a big blank area appears, Delete
    // restores it" symptom.
    const int wrap_width = qMax(1, width() - 2 * frameWidth());
    const qreal measured = fontMetrics()
                               .boundingRect(QRect(0, 0, wrap_width, 0),
                                             Qt::TextWordWrap, toPlainText())
                               .height();
    const int one_line = fontMetrics().height() + 12;
    const int target = qMax(
        static_cast<int>(qCeil(measured)) +
            2 * static_cast<int>(document()->documentMargin()) + 6,
        one_line);
    if (height() != target) setFixedHeight(target);
    updateGeometry();
}

void InlineEditor::SetReferenceItems(std::vector<ReferenceItem> items) {
    reference_items_ = std::move(items);
}

// ---------------- Model -> editor ----------------

void InlineEditor::SetContent(const InlineContent& content) {
    loading_ = true;
    clear();
    QTextCursor cursor(document());
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            QTextCharFormat format;
            format.setFontWeight(HasMark(run->marks, TextMark::Strong)
                                     ? QFont::Bold
                                     : QFont::Normal);
            format.setFontItalic(HasMark(run->marks, TextMark::Emphasis));
            cursor.setCharFormat(format);
            cursor.insertText(QString::fromStdString(run->text));
        } else if (const auto* eq = std::get_if<InlineEquation>(&node)) {
            InsertToken(TokenKind::Equation,
                        QString::fromStdString(eq->math_source),
                        QString::fromStdString(eq->math_source));
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            QString keys;
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i) keys += QStringLiteral(",");
                keys += QString::fromStdString(cit->keys[i]);
            }
            InsertToken(TokenKind::Citation, keys, keys);
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            const QString target = QString::fromStdString(ref->target.value());
            InsertToken(TokenKind::CrossReference, target, target);
        }
    }
    loading_ = false;
    dirty_ = false;
    SanitizeTokens();
    ResizeToContent();
}

void InlineEditor::SetContentClean(const InlineContent& content) {
    SetContent(content);
    dirty_ = false;
}

InlineContent InlineEditor::Content() const {
    // Read the marks from the document's own structure instead of asking
    // charFormat() at every caret position: charFormat() reports the format of
    // the character *before* the position, so a per-character scan shifted
    // every run by one character - bold "worked" by luck on long runs and
    // italic lost its first character, which is exactly why italics never
    // reached the PDF. A QTextFragment is a maximal run of one format, so
    // walking fragments reproduces the run boundaries exactly.
    InlineContent content;
    for (QTextBlock block = document()->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.length() == 0) continue;
            const QTextCharFormat format = fragment.charFormat();
            const QString text = fragment.text();

            int offset = 0;
            while (offset < text.length()) {
                if (text.at(offset) == kTokenChar) {
                    // The fragment's own format is the authoritative token
                    // description; TokenAt() would report the *previous*
                    // character's format and misread every token.
                    const QVariant kind_variant = format.property(kTokenKindProperty);
                    if (kind_variant.isValid()) {
                        const TokenKind kind =
                            static_cast<TokenKind>(kind_variant.toInt());
                        const QString payload =
                            format.property(kTokenPayloadProperty).toString();
                        if (kind == TokenKind::Citation) {
                            Citation citation;
                            const QStringList keys = payload.split(',');
                            for (const QString& key : keys) {
                                const QString trimmed = key.trimmed();
                                if (!trimmed.isEmpty()) {
                                    citation.keys.push_back(trimmed.toStdString());
                                }
                            }
                            if (!citation.keys.empty()) {
                                content.push_back(std::move(citation));
                            }
                        } else if (kind == TokenKind::CrossReference) {
                            CrossReference ref;
                            ref.target = NodeId(payload.toStdString());
                            content.push_back(std::move(ref));
                        } else {
                            InlineEquation eq;
                            eq.math_source = payload.toStdString();
                            content.push_back(std::move(eq));
                        }
                        ++offset;
                        continue;
                    }
                    // A token character without a payload is not a token.
                    ++offset;
                    continue;
                }

                std::uint8_t marks = 0;
                SetMark(marks, TextMark::Strong,
                        format.fontWeight() >= QFont::Bold);
                SetMark(marks, TextMark::Emphasis, format.fontItalic());

                QString run;
                while (offset < text.length() &&
                       text.at(offset) != kTokenChar) {
                    run += text.at(offset);
                    ++offset;
                }
                if (!run.isEmpty()) {
                    content.push_back(TextRun{run.toStdString(), marks});
                }
            }
        }
        // Paragraph breaks do not exist inside a single paragraph block: a
        // newline becomes a space, which is what the document model wants.
        if (block.next().isValid() && !content.empty()) {
            if (auto* run = std::get_if<TextRun>(&content.back())) {
                run->text += " ";
            }
        }
    }
    return content;
}

QString InlineEditor::PlainText() const { return toPlainText(); }

// ---------------- Marks ----------------

void InlineEditor::ToggleBold() {
    QTextCursor cursor = textCursor();
    const bool on = !IsBoldActive();
    QTextCharFormat format;
    format.setFontWeight(on ? QFont::Bold : QFont::Normal);
    if (cursor.hasSelection()) cursor.mergeCharFormat(format);
    mergeCurrentCharFormat(format);
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::ToggleItalic() {
    QTextCursor cursor = textCursor();
    const bool on = !IsItalicActive();
    QTextCharFormat format;
    format.setFontItalic(on);
    if (cursor.hasSelection()) cursor.mergeCharFormat(format);
    mergeCurrentCharFormat(format);
    dirty_ = true;
    ResizeToContent();
}

bool InlineEditor::IsBoldActive() const {
    return const_cast<InlineEditor*>(this)->textCursor().charFormat().fontWeight() >=
           QFont::Bold;
}

bool InlineEditor::IsItalicActive() const {
    return const_cast<InlineEditor*>(this)->textCursor().charFormat().fontItalic();
}

// ---------------- Tokens ----------------

void InlineEditor::InsertToken(TokenKind kind, const QString& payload,
                               const QString& label) {
    QTextCharFormat format;
    format.setProperty(kTokenKindProperty, static_cast<int>(kind));
    format.setProperty(kTokenPayloadProperty, payload);
    format.setForeground(QColor(theme::kAccent));
    format.setBackground(QColor(theme::kAccentSoft));
    format.setFontWeight(QFont::DemiBold);
    // One character, so Backspace/Delete removes it whole.
    textCursor().insertText(QString(kTokenChar), format);
    (void)label;
}

std::optional<InlineEditor::TokenHit> InlineEditor::TokenAt(int position) const {
    const int last = document()->characterCount() - 1;
    if (position < 0 || position > last) return std::nullopt;
    QTextCursor cursor(document());
    cursor.setPosition(position);
    const QTextCharFormat format = cursor.charFormat();
    const QVariant kind = format.property(kTokenKindProperty);
    if (!kind.isValid()) return std::nullopt;
    TokenHit hit;
    hit.kind = static_cast<TokenKind>(kind.toInt());
    hit.payload = format.property(kTokenPayloadProperty).toString();
    hit.position = position;
    return hit;
}

void InlineEditor::RemoveTokenAt(int position) {
    const auto hit = TokenAt(position);
    if (!hit) return;
    QTextCursor cursor(document());
    cursor.setPosition(hit->position);
    cursor.deleteChar();
}

void InlineEditor::SanitizeTokens() {
    // A token character that lost its payload (foreign paste) becomes plain
    // text rather than a token that Content() would silently drop.
    for (QTextBlock block = document()->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QString fragment_text = fragment.text();
            if (!fragment_text.contains(kTokenChar)) continue;
            if (fragment.charFormat().property(kTokenKindProperty).isValid()) {
                continue;
            }
            QTextCursor cursor(document());
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(),
                               QTextCursor::KeepAnchor);
            QString replacement = fragment_text;
            replacement.remove(kTokenChar);
            cursor.insertText(replacement);
            return;  // textChanged re-runs the scan
        }
    }
}

void InlineEditor::InsertCitationToken(const QStringList& keys) {
    QString joined;
    for (int i = 0; i < keys.size(); ++i) {
        if (i) joined += QStringLiteral(",");
        joined += keys.at(i);
    }
    if (joined.isEmpty()) return;
    InsertToken(TokenKind::Citation, joined, joined);
    dirty_ = true;
}

void InlineEditor::InsertCrossReferenceToken(const QString& target_node) {
    if (target_node.isEmpty()) return;
    InsertToken(TokenKind::CrossReference, target_node, target_node);
    dirty_ = true;
}

void InlineEditor::InsertInlineEquation(const QString& math) {
    if (math.isEmpty()) return;
    InsertToken(TokenKind::Equation, math, math);
    dirty_ = true;
}

// ---------------- Events ----------------

void InlineEditor::keyPressEvent(QKeyEvent* event) {
    // Backspace/Delete removes a whole token, never half of one.
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        const QString text = toPlainText();
        const int position = textCursor().position();
        const int probe =
            event->key() == Qt::Key_Backspace ? position - 1 : position;
        if (probe >= 0 && probe < text.length() &&
            text.at(probe) == kTokenChar && !textCursor().hasSelection()) {
            RemoveTokenAt(probe);
            return;
        }
    }
    if ((event->modifiers() & Qt::ControlModifier) &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        emit Committed();
        emit NewBlockAfter();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) &&
        event->key() == Qt::Key_B) {
        ToggleBold();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) &&
        event->key() == Qt::Key_I) {
        ToggleItalic();
        return;
    }
    QTextEdit::keyPressEvent(event);
    ResizeToContent();
}

bool InlineEditor::canInsertFromMimeData(const QMimeData*) const { return true; }

void InlineEditor::insertFromMimeData(const QMimeData* source) {
    if (!source) return;
    // Ctrl+Shift+V: text only (plan §14). Plain Ctrl+V keeps bold/italic and
    // drops fonts, sizes, colors and spacing.
    const bool plain_only =
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (plain_only || !source->hasHtml()) {
        const QString pasted = source->text();
        if (pasted.isEmpty()) return;
        const QString reflowed = QString::fromStdString(
            ReflowHardWrappedText(pasted.toStdString()));
        textCursor().insertText(reflowed);
        dirty_ = true;
        ResizeToContent();
        return;
    }
    // Rich paste: convert HTML and keep only weight/italic.
    QTextDocument converted;
    converted.setHtml(source->html());
    QTextCursor cursor = textCursor();
    for (QTextBlock block = converted.begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            QTextCharFormat allowed;
            allowed.setFontWeight(fragment.charFormat().fontWeight());
            allowed.setFontItalic(fragment.charFormat().fontItalic());
            cursor.insertText(fragment.text(), allowed);
        }
        if (block.next().isValid()) cursor.insertText(QStringLiteral("\n"));
    }
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::mousePressEvent(QMouseEvent* event) {
    // Clicking a token selects it whole (plan §5).
    const QString text = toPlainText();
    const int position =
        document()->documentLayout()->hitTest(event->pos(), Qt::FuzzyHit);
    if (position >= 0 && position < text.length() &&
        text.at(position) == kTokenChar) {
        QTextCursor cursor(document());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
        return;
    }
    QTextEdit::mousePressEvent(event);
}

void InlineEditor::mouseReleaseEvent(QMouseEvent* event) {
    const auto hit = TokenAt(textCursor().position());
    if (hit) {
        if (hit->kind == TokenKind::Citation) {
            emit CitationTokenActivated(hit->payload);
        } else if (hit->kind == TokenKind::CrossReference) {
            emit CrossReferenceTokenActivated(hit->payload);
        }
    }
    QTextEdit::mouseReleaseEvent(event);
}

void InlineEditor::resizeEvent(QResizeEvent* event) {
    QTextEdit::resizeEvent(event);
    // Measure with the incoming width: width() still reports the old value
    // while the resize event is being delivered.
    ResizeToWidth(event->size().width());
}

void InlineEditor::showEvent(QShowEvent* event) {
    QTextEdit::showEvent(event);
    ResizeToContent();
}

void InlineEditor::ResizeToWidth(int width) {
    const int wrap_width = qMax(1, width - 2 * frameWidth());
    const qreal measured = fontMetrics()
                               .boundingRect(QRect(0, 0, wrap_width, 0),
                                             Qt::TextWordWrap, toPlainText())
                               .height();
    const int one_line = fontMetrics().height() + 12;
    const int target = qMax(
        static_cast<int>(qCeil(measured)) +
            2 * static_cast<int>(document()->documentMargin()) + 6,
        one_line);
    if (height() != target) setFixedHeight(target);
    updateGeometry();
}

void InlineEditor::focusOutEvent(QFocusEvent* event) {
    QTextEdit::focusOutEvent(event);
    emit Committed();
}

}  // namespace pf::gui
