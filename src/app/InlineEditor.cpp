#include "app/InlineEditor.h"

#include <QApplication>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>

#include "app/InlineMathObjectRenderer.h"
#include "app/MathEditorDialog.h"
#include "app/MathPreviewRenderer.h"
#include "app/Theme.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
constexpr qreal kMaximumWidthFraction = 0.85;
}  // namespace

const char* InlineEditor::InlineMimeType() {
    return "application/x-strlatex-inline";
}

InlineEditor::InlineEditor(QWidget* parent)
    : QTextEdit(parent),
      math_object_renderer_(std::make_unique<InlineMathObjectRenderer>()),
      citation_object_renderer_(std::make_unique<CitationObjectRenderer>()) {
    setAcceptRichText(false);
    setWordWrapMode(QTextOption::WordWrap);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(6);
    document()->documentLayout()->registerHandler(
        inline_math_format::kObjectType, math_object_renderer_.get());
    document()->documentLayout()->registerHandler(
        citation_format::kObjectType, citation_object_renderer_.get());
    connect(this, &QTextEdit::textChanged, this, [this]() {
        SanitizeTokens();
        ResizeToContent();
        if (!loading_ && !refreshing_displays_) dirty_ = true;
    });
    ResizeToContent();
}

InlineEditor::~InlineEditor() {
    if (document() && document()->documentLayout()) {
        document()->documentLayout()->unregisterHandler(
            inline_math_format::kObjectType, math_object_renderer_.get());
        document()->documentLayout()->unregisterHandler(
            citation_format::kObjectType, citation_object_renderer_.get());
    }
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
            QStringList keys;
            for (const auto& key : cit->keys) {
                keys << QString::fromStdString(key);
            }
            InsertPillObject(cursor, TokenKind::Citation, keys.join(','),
                             CitationDisplayText(keys));
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            const QString target = QString::fromStdString(ref->target.value());
            InsertPillObject(cursor, TokenKind::CrossReference, target,
                             CrossReferenceDisplayText(target));
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
                const int token_count =
                    text.count(kTokenChar) + text.count(kObjectChar);
                if (token_count > 0) {
                    for (int t = 0; t < token_count; ++t) {
                        if (kind == TokenKind::Citation) {
                            Citation citation;
                            const QStringList keys = payload.split(',');
                            for (const QString& key : keys) {
                                const QString trimmed = key.trimmed();
                                if (!trimmed.isEmpty()) {
                                    citation.keys.push_back(
                                        trimmed.toStdString());
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
                // A token format spread over characters that are not token
                // markers is a format leak (e.g. a foreign cursor inheriting
                // the object format while typing). Never invent a semantic
                // node out of it - fall through and keep the text.
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

// ---------------- Semantic inline objects ----------------

// Citation / cross reference pills. One object replacement character painted
// by CitationObjectRenderer (citation plan §1); the payload (keys or target
// node) lives in the char format and the display text is resolved from the
// document-wide numbering map, never from what the character itself shows.
void InlineEditor::InsertPillObject(QTextCursor& cursor, TokenKind kind,
                                    const QString& payload,
                                    const QString& display) {
    QTextCharFormat format;
    format.setObjectType(citation_format::kObjectType);
    format.setFont(document()->defaultFont());
    // The pill must not grow the line: AlignNormal reserves exactly the
    // height the renderer reports.
    format.setVerticalAlignment(QTextCharFormat::AlignNormal);
    format.setProperty(kTokenKindProperty, static_cast<int>(kind));
    format.setProperty(kTokenPayloadProperty, payload);
    format.setProperty(citation_format::kDisplayTextProperty, display);
    cursor.insertText(QString(kObjectChar), format);
    // New typing must never inherit the object identity.
    cursor.setCharFormat(QTextCharFormat());
}

QString InlineEditor::CitationDisplayText(const QStringList& keys) const {
    if (!citation_numbers_) {
        // No numbering snapshot yet (fresh widget): show the keys so the pill
        // is still honest about what it stands for.
        return QStringLiteral("[") + keys.join(QStringLiteral(", ")) +
               QStringLiteral("]");
    }
    std::vector<std::string> std_keys;
    std_keys.reserve(keys.size());
    for (const QString& key : keys) std_keys.push_back(key.toStdString());
    return QString::fromStdString(citation_numbers_->FormatPill(std_keys));
}

QString InlineEditor::CrossReferenceDisplayText(const QString& target) const {
    const auto it = xref_labels_.find(target);
    return it != xref_labels_.end() && !it->second.isEmpty()
               ? it->second
               : QStringLiteral("[ref]");
}

void InlineEditor::SetCitationNumbers(
    std::shared_ptr<const CitationNumberResolver> numbers) {
    if (citation_numbers_ == numbers) return;
    citation_numbers_ = std::move(numbers);
    RefreshObjectDisplays();
}

void InlineEditor::SetCrossReferenceLabels(std::map<QString, QString> labels) {
    if (xref_labels_ == labels) return;
    xref_labels_ = std::move(labels);
    RefreshObjectDisplays();
}

void InlineEditor::RefreshObjectDisplays() {
    if (!document() || !document()->documentLayout()) return;
    // Repainting the pills rewrites *format* only. The base class still
    // reports that as a content change, so the row must not mistake it for
    // user input: refreshing the displays never dirties the editor.
    refreshing_displays_ = true;
    QTextCursor cursor(document());
    for (QTextBlock block = document()->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.length() == 0) continue;
            const QTextCharFormat format = fragment.charFormat();
            const QVariant kind = format.property(kTokenKindProperty);
            if (!kind.isValid()) continue;
            const TokenKind token_kind = static_cast<TokenKind>(kind.toInt());
            if (token_kind == TokenKind::Math) continue;
            const QString payload =
                format.property(kTokenPayloadProperty).toString();
            const QString display =
                token_kind == TokenKind::Citation
                    ? CitationDisplayText(payload.split(',', Qt::SkipEmptyParts))
                    : CrossReferenceDisplayText(payload);
            if (format.property(citation_format::kDisplayTextProperty)
                    .toString() == display) {
                continue;
            }
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(),
                               QTextCursor::KeepAnchor);
            QTextCharFormat update;
            update.setProperty(citation_format::kDisplayTextProperty, display);
            cursor.mergeCharFormat(update);
        }
    }
    refreshing_displays_ = false;
    if (viewport()) viewport()->update();
}

void InlineEditor::InsertMathObject(QTextCursor& cursor, const QString& latex) {
    ensurePolished();
    const QFont text_font = document()->defaultFont();
    const QFontMetricsF text_metrics(text_font);

    MathRenderStyle style;
    style.font_px = qMax(4, qRound(text_metrics.height()));
    style.color = QColor(theme::kPrimaryText);
    style.font_family = text_font.family();
    style.template_id = property("template_id").toString();
    MathRenderResult rendered = RenderMathPreview(latex, style);
    if (rendered.pixmap.isNull()) return;

    // Fit both sides of the TeX baseline into the current text line. The
    // object renderer paints the descent below the baseline, so this does not
    // enlarge the QTextLayout line box or the surrounding TextBlock.
    const qreal math_ascent = qMax<qreal>(1.0, rendered.baseline);
    const qreal math_descent =
        qMax<qreal>(1.0, rendered.height - rendered.baseline);
    qreal scale = qMin(text_metrics.ascent() / math_ascent,
                       text_metrics.descent() / math_descent);
    const qreal maximum_width =
        qMax<qreal>(48.0, viewport()->width() * kMaximumWidthFraction);
    if (rendered.width > 0) {
        scale = qMin(scale, maximum_width / rendered.width);
    }
    scale = qBound<qreal>(0.01, scale, 1.0);

    QTextCharFormat format;
    format.setObjectType(inline_math_format::kObjectType);
    format.setFont(text_font);
    // AlignNormal gives the object zero layout descent; intrinsicSize()
    // reports only the TeX ascent. AlignBaseline subtracts a font descent.
    format.setVerticalAlignment(QTextCharFormat::AlignNormal);
    format.setProperty(inline_math_format::kPixmapProperty,
                       QVariant::fromValue(rendered.pixmap));
    format.setProperty(inline_math_format::kWidthProperty,
                       rendered.width * scale);
    format.setProperty(inline_math_format::kHeightProperty,
                       rendered.height * scale);
    format.setProperty(inline_math_format::kBaselineProperty,
                       rendered.baseline * scale);
    format.setProperty(kTokenKindProperty, static_cast<int>(TokenKind::Math));
    format.setProperty(kTokenPayloadProperty, latex);
    cursor.insertText(QString(kObjectChar), format);
    // New typing must never inherit the semantic object properties.
    cursor.setCharFormat(QTextCharFormat());
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
            const bool has_kind =
                fragment.charFormat().property(kTokenKindProperty).isValid();
            if (has_token_char || has_object_char) {
                if (has_kind) continue;
                // Orphan token character: strip it and re-run the scan.
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
            if (has_kind) {
                // The object identity leaked onto ordinary characters (e.g.
                // typing through a raw cursor parked behind a pill). Strip
                // the semantics - never let plain text masquerade as a token.
                // setCharFormat replaces the whole format, so keep the two
                // legitimate visual properties (bold / italic).
                QTextCursor cursor(document());
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(),
                                   QTextCursor::KeepAnchor);
                QTextCharFormat clean;
                clean.setFontWeight(fragment.charFormat().fontWeight());
                clean.setFontItalic(fragment.charFormat().fontItalic());
                cursor.setCharFormat(clean);
                return;  // textChanged re-runs the scan
            }
        }
    }
}

void InlineEditor::InsertCitationObject(const QStringList& keys) {
    QStringList cleaned;
    for (const QString& key : keys) {
        const QString trimmed = key.trimmed();
        if (!trimmed.isEmpty()) cleaned << trimmed;
    }
    if (cleaned.isEmpty()) return;
    QTextCursor cursor = textCursor();
    InsertPillObject(cursor, TokenKind::Citation,
                     cleaned.join(QLatin1Char(',')),
                     CitationDisplayText(cleaned));
    setTextCursor(cursor);
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::InsertCrossReferenceObject(const QString& target_node) {
    if (target_node.isEmpty()) return;
    QTextCursor cursor = textCursor();
    InsertPillObject(cursor, TokenKind::CrossReference, target_node,
                     CrossReferenceDisplayText(target_node));
    setTextCursor(cursor);
    dirty_ = true;
    ResizeToContent();
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
    OpenMathEditor(std::nullopt);
}

void InlineEditor::EditMathAt(int position) {
    const auto hit = TokenAt(position);
    if (!hit || hit->kind != TokenKind::Math) return;
    OpenMathEditor(hit);
}

void InlineEditor::OpenMathEditor(const std::optional<TokenHit>& hit) {
    if (math_editor_open_) return;
    math_editor_open_ = true;
    QTextCursor target = textCursor();
    if (hit) {
        target.setPosition(hit->position);
        target.setPosition(hit->position + 1, QTextCursor::KeepAnchor);
    }
    // Heap ownership plus open() avoids a nested event loop and prevents a
    // row rebuild from deleting a stack-allocated child dialog.
    auto* dialog = new MathEditorDialog(hit ? hit->payload : QString(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this,
            [this, dialog, target](int result) mutable {
        const QString latex = dialog->latex();
        if (result == QDialog::Accepted && !latex.trimmed().isEmpty()) {
            target.beginEditBlock();
            InsertMathObject(target, latex);
            target.endEditBlock();
            setTextCursor(target);
            dirty_ = true;
            ResizeToContent();
        }
        math_editor_open_ = false;
        // Finish the dialog's signal delivery before committing can rebuild
        // its parent row. Context binding cancels this if the row is removed.
        QTimer::singleShot(0, this, [this]() {
            setFocus(Qt::OtherFocusReason);
            emit Committed();
        });
    });
    dialog->open();
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
    document()->setTextWidth(wrap_width);
    const qreal measured = document()->documentLayout()->documentSize().height();
    const int one_line = fontMetrics().height() + 12;
    const int target = qMax(static_cast<int>(qCeil(measured)) + 6, one_line);
    if (height() != target) setFixedHeight(target);
    updateGeometry();
}

void InlineEditor::focusOutEvent(QFocusEvent* event) {
    QTextEdit::focusOutEvent(event);
    // A focus loss caused by the citation/reference picker (or the math
    // editor) opening is *not* "the user finished editing" (citation plan
    // §4): the picker will insert an object and commit itself.
    if (!math_editor_open_ && !IsProtectedInsertOpen()) emit Committed();
}

}  // namespace pf::gui
