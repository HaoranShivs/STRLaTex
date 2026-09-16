#include "app/InlineEditor.h"

#include <QApplication>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>

#include "app/MathEditorDialog.h"
#include "app/MathPreviewRenderer.h"
#include "app/Theme.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
constexpr int kInlineMathFontPx = 17;
constexpr int kInlineMathMaxHeight = 56;
constexpr int kInlineMathMaxWidth = 420;
}  // namespace

const char* InlineEditor::InlineMimeType() {
    return "application/x-strlatex-inline";
}

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

qreal InlineEditor::MaxInlineImageHeight() const {
    qreal tallest = 0.0;
    for (QTextBlock block = document()->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            if (!fragment.charFormat().property(kTokenKindProperty).isValid()) {
                continue;
            }
            const QTextImageFormat image = fragment.charFormat().toImageFormat();
            if (image.isValid() && image.height() > tallest) {
                tallest = image.height();
            }
        }
    }
    return tallest;
}

void InlineEditor::ResizeToContent() { ResizeToWidth(width()); }

void InlineEditor::SetReferenceItems(std::vector<ReferenceItem> items) {
    reference_items_ = std::move(items);
}

// ---------------- Model -> editor ----------------

void InlineEditor::InsertContent(const InlineContent& content) {
    QTextCursor cursor = textCursor();
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            QTextCharFormat format;
            format.setFontWeight(HasMark(run->marks, TextMark::Strong)
                                     ? QFont::Bold
                                     : QFont::Normal);
            format.setFontItalic(HasMark(run->marks, TextMark::Emphasis));
            cursor.setCharFormat(format);
            cursor.insertText(QString::fromStdString(run->text));
        } else if (const auto* math = std::get_if<InlineMath>(&node)) {
            InsertMathObject(cursor, QString::fromStdString(math->expression.latex));
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            QString keys;
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i) keys += QStringLiteral(",");
                keys += QString::fromStdString(cit->keys[i]);
            }
            InsertToken(cursor, TokenKind::Citation, keys, keys);
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            const QString target = QString::fromStdString(ref->target.value());
            InsertToken(cursor, TokenKind::CrossReference, target, target);
        }
    }
    setTextCursor(cursor);
}

void InlineEditor::SetContent(const InlineContent& content) {
    loading_ = true;
    clear();
    QTextCursor cursor(document());
    setTextCursor(cursor);
    InsertContent(content);
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
    return ContentInRange(0, document()->characterCount());
}

InlineContent InlineEditor::ContentInRange(int begin, int end) const {
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
            const int fragment_start = fragment.position();
            const int fragment_end = fragment_start + fragment.length();
            if (fragment_end <= begin || fragment_start >= end) continue;

            const QTextCharFormat format = fragment.charFormat();
            const QString text = fragment.text();

            // Semantic tokens. The fragment's own format is the authoritative
            // token description; TokenAt() would report the *previous*
            // character's format and misread every token.
            const QVariant kind_variant = format.property(kTokenKindProperty);
            if (kind_variant.isValid()) {
                const TokenKind kind =
                    static_cast<TokenKind>(kind_variant.toInt());
                const QString payload =
                    format.property(kTokenPayloadProperty).toString();
                // Usually one token per fragment; a merged fragment may hold
                // several identical ones.
                int token_count =
                    text.count(kTokenChar) + text.count(kObjectChar);
                if (token_count == 0) token_count = 1;
                for (int t = 0; t < token_count; ++t) {
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
                        InlineMath math;
                        math.expression.latex = payload.toStdString();
                        content.push_back(std::move(math));
                    }
                }
                continue;
            }

            // Ordinary text: take the part that overlaps [begin, end) and drop
            // any token character that lost its format.
            const int from = qMax(fragment_start, begin);
            const int to = qMin(fragment_end, end);
            if (to <= from) continue;
            QString run = text.mid(from - fragment_start, to - from);
            run.remove(kTokenChar);
            run.remove(kObjectChar);
            if (run.isEmpty()) continue;

            std::uint8_t marks = 0;
            SetMark(marks, TextMark::Strong, format.fontWeight() >= QFont::Bold);
            SetMark(marks, TextMark::Emphasis, format.fontItalic());

            if (!content.empty()) {
                if (auto* previous = std::get_if<TextRun>(&content.back());
                    previous && previous->marks == marks) {
                    previous->text += run.toStdString();
                    continue;
                }
            }
            content.push_back(TextRun{run.toStdString(), marks});
        }
        // Paragraph breaks do not exist inside a single paragraph block: a
        // newline becomes a space, which is what the document model wants.
        if (block.next().isValid() && !content.empty() &&
            block.next().position() < end) {
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

void InlineEditor::InsertToken(QTextCursor& cursor, TokenKind kind,
                               const QString& payload, const QString& label) {
    QTextCharFormat format;
    format.setProperty(kTokenKindProperty, static_cast<int>(kind));
    format.setProperty(kTokenPayloadProperty, payload);
    format.setForeground(QColor(theme::kAccent));
    format.setBackground(QColor(theme::kAccentSoft));
    format.setFontWeight(QFont::DemiBold);
    // One character, so Backspace/Delete removes it whole.
    cursor.insertText(QString(kTokenChar), format);
    (void)label;
}

void InlineEditor::InsertMathObject(QTextCursor& cursor, const QString& latex) {
    MathRenderStyle style;
    style.font_px = kInlineMathFontPx;
    style.color = QColor(theme::kPrimaryText);
    MathRenderResult rendered = RenderMathPreview(latex, style);
    if (rendered.pixmap.isNull()) return;

    // Keep an inline formula from dwarfing the line it sits in.
    int width = rendered.width;
    int height = rendered.height;
    QPixmap pixmap = rendered.pixmap;
    if (height > kInlineMathMaxHeight || width > kInlineMathMaxWidth) {
        pixmap = pixmap.scaled(qMin(width, kInlineMathMaxWidth),
                               qMin(height, kInlineMathMaxHeight),
                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(rendered.pixmap.devicePixelRatio());
        width = qRound(pixmap.width() / pixmap.devicePixelRatio());
        height = qRound(pixmap.height() / pixmap.devicePixelRatio());
    }

    const QString name =
        QStringLiteral("pf-math:%1").arg(++math_resource_counter_);
    document()->addResource(QTextDocument::ImageResource, QUrl(name),
                            QVariant::fromValue(pixmap));

    QTextImageFormat format;
    format.setName(name);
    format.setWidth(width);
    format.setHeight(height);
    format.setProperty(kTokenKindProperty, static_cast<int>(TokenKind::Math));
    format.setProperty(kTokenPayloadProperty, latex);
    cursor.insertImage(format);
}

std::optional<InlineEditor::TokenHit> InlineEditor::TokenAt(int position) const {
    const int last = document()->characterCount() - 1;
    if (position < 0 || position >= last) return std::nullopt;
    QTextCursor cursor(document());
    cursor.setPosition(position);
    // charFormat() reports the character *before* the cursor unless something
    // is selected, so select exactly the character under test.
    cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
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
    const int start = qMax(0, position);
    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(start + 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
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
            const bool has_token_char = fragment_text.contains(kTokenChar);
            const bool has_object_char = fragment_text.contains(kObjectChar);
            if (!has_token_char && !has_object_char) continue;
            if (fragment.charFormat().property(kTokenKindProperty).isValid()) {
                continue;
            }
            QTextCursor cursor(document());
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(),
                               QTextCursor::KeepAnchor);
            QString replacement = fragment_text;
            replacement.remove(kTokenChar);
            replacement.remove(kObjectChar);
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
    QTextCursor cursor = textCursor();
    InsertToken(cursor, TokenKind::Citation, joined, joined);
    setTextCursor(cursor);
    dirty_ = true;
}

void InlineEditor::InsertCrossReferenceToken(const QString& target_node) {
    if (target_node.isEmpty()) return;
    QTextCursor cursor = textCursor();
    InsertToken(cursor, TokenKind::CrossReference, target_node, target_node);
    setTextCursor(cursor);
    dirty_ = true;
}

void InlineEditor::InsertInlineMath(const QString& latex) {
    if (latex.trimmed().isEmpty()) return;
    QTextCursor cursor = textCursor();
    InsertMathObject(cursor, latex);
    setTextCursor(cursor);
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::BeginInlineMath() {
    MathEditorDialog dialog(QString(), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString latex = dialog.latex();
    if (latex.trimmed().isEmpty()) return;
    InsertInlineMath(latex);
}

void InlineEditor::EditMathAt(int position) {
    const auto hit = TokenAt(position);
    if (!hit || hit->kind != TokenKind::Math) return;
    MathEditorDialog dialog(hit->payload, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString latex = dialog.latex();
    if (latex.trimmed().isEmpty()) return;

    QTextCursor cursor(document());
    cursor.setPosition(hit->position);
    cursor.setPosition(hit->position + 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    InsertMathObject(cursor, latex);
    setTextCursor(cursor);
    dirty_ = true;
    ResizeToContent();
}

// ---------------- Events ----------------

void InlineEditor::keyPressEvent(QKeyEvent* event) {
    // Backspace/Delete removes a whole token, never half of one.
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        const QString text = toPlainText();
        const int position = textCursor().position();
        const int probe =
            event->key() == Qt::Key_Backspace ? position - 1 : position;
        if (!textCursor().hasSelection() && probe >= 0 &&
            probe < text.length() &&
            (text.at(probe) == kTokenChar || text.at(probe) == kObjectChar)) {
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
    // The editor's own rich content (marks, tokens, math objects) survives a
    // copy/paste inside the document through its private clipboard flavour.
    if (source->hasFormat(InlineMimeType())) {
        const std::string encoded =
            source->data(InlineMimeType()).toStdString();
        const InlineContent content = InlineFromRichText(encoded);
        InsertContent(content);
        dirty_ = true;
        ResizeToContent();
        return;
    }
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

QMimeData* InlineEditor::createMimeDataFromSelection() const {
    // Build our own QMimeData rather than decorating the one the base class
    // returns: some platform clipboard backends hand back a wrapper whose
    // storage is not the plain QMimeData map, and writing to it is silently
    // lost. Re-creating the object keeps the standard plain/html flavours and
    // lets the private one survive.
    QMimeData* base = QTextEdit::createMimeDataFromSelection();
    auto* data = new QMimeData();
    if (base) {
        const QStringList formats = base->formats();
        for (const QString& format : formats) {
            if (format == QStringLiteral("text/plain") ||
                format == QStringLiteral("text/html")) {
                continue;  // restored through the accessors below
            }
            data->setData(format, base->data(format));
        }
        if (base->hasHtml()) data->setHtml(base->html());
        if (base->hasText()) data->setText(base->text());
        delete base;
    }
    const QTextCursor cursor = const_cast<InlineEditor*>(this)->textCursor();
    if (cursor.hasSelection()) {
        const InlineContent content =
            ContentInRange(cursor.selectionStart(), cursor.selectionEnd());
        const std::string encoded = InlineToRichText(content);
        data->setData(InlineMimeType(), QByteArray::fromStdString(encoded));
    }
    return data;
}

void InlineEditor::mousePressEvent(QMouseEvent* event) {
    // Clicking a token selects it whole (plan §5).
    const QString text = toPlainText();
    const int position =
        document()->documentLayout()->hitTest(event->pos(), Qt::FuzzyHit);
    if (position >= 0 && position < text.length() &&
        (text.at(position) == kTokenChar || text.at(position) == kObjectChar)) {
        QTextCursor cursor(document());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
        return;
    }
    QTextEdit::mousePressEvent(event);
}

void InlineEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-clicking an inline math object opens the LaTeX source editor.
    const QString text = toPlainText();
    const int position =
        document()->documentLayout()->hitTest(event->pos(), Qt::FuzzyHit);
    const int candidates[] = {position, position - 1};
    for (const int probe : candidates) {
        if (probe < 0 || probe >= text.length()) continue;
        if (text.at(probe) != kObjectChar) continue;
        const auto hit = TokenAt(probe);
        if (hit && hit->kind == TokenKind::Math) {
            EditMathAt(probe);
            return;
        }
    }
    QTextEdit::mouseDoubleClickEvent(event);
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
    qreal measured = fontMetrics()
                         .boundingRect(QRect(0, 0, wrap_width, 0),
                                       Qt::TextWordWrap, toPlainText())
                         .height();
    // Inline math images are objects, so plain-text metrics do not see them.
    const qreal image_height = MaxInlineImageHeight();
    if (image_height > 0) {
        measured = qMax(measured, image_height + fontMetrics().height());
    }
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
