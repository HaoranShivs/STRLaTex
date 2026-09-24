#include "app/InlineEditor.h"

#include <atomic>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTimer>

#include "app/InlineMathObjectRenderer.h"
#include "app/MathEditorDialog.h"
#include "app/MathPreviewRenderer.h"
#include "app/Theme.h"
#include "app/math/MathRenderService.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
constexpr qreal kMaximumWidthFraction = 0.85;
} // namespace

const char* InlineEditor::InlineMimeType() {
    return "application/x-strlatex-inline";
}

InlineEditor::InlineEditor(QWidget* parent)
    : QTextEdit(parent), math_object_renderer_(std::make_unique<InlineMathObjectRenderer>()),
      citation_object_renderer_(std::make_unique<CitationObjectRenderer>()) {
    // P0-07：连接到共享的数学渲染服务。每一行都会获得唯一 id；
    // 服务通过 queued 信号回复，因此针对已销毁行的回复
    // 会被服务内部的 QPointer 守卫丢弃。
    static std::atomic<std::uint64_t> next_editor_id{0};
    const QString editor_id = QStringLiteral("inline-editor-%1").arg(next_editor_id.fetch_add(1));
    setObjectName(editor_id);
    MathRenderService* service = MathRenderService::Shared();
    service->RegisterClient(editor_id, this);
    connect(service, &MathRenderService::mathRendered, this, [this](const MathRenderResponse& response) {
        if (response.editor_id != objectName())
            return;
        ApplyMathRender(response.formula_id, response.latex, response.result.image, response.result.width,
                        response.result.height, response.result.baseline, response.result.device_pixel_ratio,
                        response.render_font_px);
    });
    setAcceptRichText(false);
    setWordWrapMode(QTextOption::WordWrap);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(theme::spacing::kEditorDocMargin);
    document()->documentLayout()->registerHandler(inline_math_format::kObjectType, math_object_renderer_.get());
    document()->documentLayout()->registerHandler(citation_format::kObjectType, citation_object_renderer_.get());
    connect(this, &QTextEdit::textChanged, this, [this]() {
        SanitizeTokens();
        ResizeToContent();
        if (!loading_ && !refreshing_displays_)
            dirty_ = true;
    });
    ResizeToContent();
}

InlineEditor::~InlineEditor() {
    if (document() && document()->documentLayout()) {
        document()->documentLayout()->unregisterHandler(inline_math_format::kObjectType, math_object_renderer_.get());
        document()->documentLayout()->unregisterHandler(citation_format::kObjectType, citation_object_renderer_.get());
    }
}

void InlineEditor::ResizeToContent() {
    ResizeToWidth(width());
}

void InlineEditor::SetReferenceItems(std::vector<ReferenceItem> items) {
    reference_items_ = std::move(items);
}

// ---------------- 模型 -> 编辑器 ----------------

void InlineEditor::SetBodyTypography(const QFont& font, int line_height_percent) {
    line_height_percent_ = line_height_percent;
    // 程序化设置样式：绝不算作用户编辑，绝不置为 dirty。
    loading_ = true;
    setFont(font);
    theme::ApplyDocumentTypography(document(), font, line_height_percent);
    RefreshMathGeometry(viewport()->width(), true);
    loading_ = false;
    ResizeToContent();
}

void InlineEditor::ApplyLineHeight() {
    if (line_height_percent_ <= 0)
        return;
    theme::ApplyDocumentTypography(document(), document()->defaultFont(), line_height_percent_);
}

void InlineEditor::InsertContent(const InlineContent& content) {
    QTextCursor cursor = textCursor();
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            QTextCharFormat format;
            format.setFontWeight(HasMark(run->marks, TextMark::Strong) ? QFont::Bold : QFont::Normal);
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
            InsertPillObject(cursor, TokenKind::Citation, keys.join(','), CitationDisplayText(keys));
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            const QString target = QString::fromStdString(ref->target.value());
            InsertPillObject(cursor, TokenKind::CrossReference, target, CrossReferenceDisplayText(target));
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
    // clear() 丢弃了块格式：恢复比例行高，
    // 使该行在每次重新加载后仍按 150% 显示（UI 方案 §5）。
    ApplyLineHeight();
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
    // 从文档自身的结构中读取 marks，而不是在每个光标位置调用
    // charFormat()：charFormat() 报告的是该位置 *之前* 那个字符的格式，
    // 因此逐字符扫描会让每个 run 整体偏移一个字符——粗体只是因为长 run
    // 才侥幸「看起来正常」，而斜体则丢掉了第一个字符，这正是斜体始终
    // 无法进入 PDF 的原因。QTextFragment 是同一格式的最大连续段，
    // 因此遍历 fragment 能精确还原 run 的边界。
    InlineContent content;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.length() == 0)
                continue;
            const int fragment_start = fragment.position();
            const int fragment_end = fragment_start + fragment.length();
            if (fragment_end <= begin || fragment_start >= end)
                continue;

            const QTextCharFormat format = fragment.charFormat();
            const QString text = fragment.text();

            // 语义 token。fragment 自身的格式才是权威的 token 描述；
            // TokenAt() 会报告 *前一个* 字符的格式，从而误判每一个 token。
            const QVariant kind_variant = format.property(kTokenKindProperty);
            if (kind_variant.isValid()) {
                const TokenKind kind = static_cast<TokenKind>(kind_variant.toInt());
                const QString payload = format.property(kTokenPayloadProperty).toString();
                // 通常每个 fragment 只有一个 token；合并后的 fragment
                // 可能包含多个相同的 token。
                const int token_count = text.count(kTokenChar) + text.count(kObjectChar);
                if (token_count > 0) {
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
                // token 格式扩散到并非 token 标记的字符上属于格式泄漏
                // （例如外部光标在输入时继承了对象格式）。绝不能据此
                // 臆造语义节点——直接落到下面的分支，保留文本。
            }

            // 普通文本：取与 [begin, end) 重叠的部分，
            // 并丢弃任何已丢失格式的 token 字符。
            const int from = qMax(fragment_start, begin);
            const int to = qMin(fragment_end, end);
            if (to <= from)
                continue;
            QString run = text.mid(from - fragment_start, to - from);
            run.remove(kTokenChar);
            run.remove(kObjectChar);
            if (run.isEmpty())
                continue;

            std::uint8_t marks = 0;
            SetMark(marks, TextMark::Strong, format.fontWeight() >= QFont::Bold);
            SetMark(marks, TextMark::Emphasis, format.fontItalic());

            if (!content.empty()) {
                if (auto* previous = std::get_if<TextRun>(&content.back()); previous && previous->marks == marks) {
                    previous->text += run.toStdString();
                    continue;
                }
            }
            content.push_back(TextRun{run.toStdString(), marks});
        }
        // 单个段落块内部不存在段落分隔：换行会变成空格，
        // 这正是文档模型所期望的。
        if (block.next().isValid() && !content.empty() && block.next().position() < end) {
            if (auto* run = std::get_if<TextRun>(&content.back())) {
                run->text += " ";
            }
        }
    }
    return content;
}

QString InlineEditor::PlainText() const {
    return toPlainText();
}

// ---------------- 标记 ----------------

void InlineEditor::ToggleBold() {
    QTextCursor cursor = textCursor();
    const bool on = !IsBoldActive();
    QTextCharFormat format;
    format.setFontWeight(on ? QFont::Bold : QFont::Normal);
    if (cursor.hasSelection())
        cursor.mergeCharFormat(format);
    mergeCurrentCharFormat(format);
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::ToggleItalic() {
    QTextCursor cursor = textCursor();
    const bool on = !IsItalicActive();
    QTextCharFormat format;
    format.setFontItalic(on);
    if (cursor.hasSelection())
        cursor.mergeCharFormat(format);
    mergeCurrentCharFormat(format);
    dirty_ = true;
    ResizeToContent();
}

bool InlineEditor::IsBoldActive() const {
    return const_cast<InlineEditor*>(this)->textCursor().charFormat().fontWeight() >= QFont::Bold;
}

bool InlineEditor::IsItalicActive() const {
    return const_cast<InlineEditor*>(this)->textCursor().charFormat().fontItalic();
}

// ---------------- 语义行内对象 ----------------

// 引用 / 交叉引用 pill。一个对象替换字符，由 CitationObjectRenderer 绘制
// （引用方案 §1）；payload（key 或目标节点）存放在 char format 中，
// 显示文本从文档级编号映射解析，绝不取决于该字符自身显示的内容。
void InlineEditor::InsertPillObject(QTextCursor& cursor, TokenKind kind, const QString& payload,
                                    const QString& display) {
    QTextCharFormat format;
    format.setObjectType(citation_format::kObjectType);
    format.setFont(document()->defaultFont());
    // pill 不得撑高行高：AlignNormal 恰好按渲染器报告的高度预留空间。
    format.setVerticalAlignment(QTextCharFormat::AlignNormal);
    format.setProperty(kTokenKindProperty, static_cast<int>(kind));
    format.setProperty(kTokenPayloadProperty, payload);
    format.setProperty(citation_format::kDisplayTextProperty, display);
    cursor.insertText(QString(kObjectChar), format);
    // 后续输入绝不能继承对象的身份标识。
    cursor.setCharFormat(QTextCharFormat());
}

QString InlineEditor::CitationDisplayText(const QStringList& keys) const {
    if (!citation_numbers_) {
        // 尚无编号 snapshot（新建的 widget）：显示 key，
        // 让 pill 仍能如实表明自己所代表的含义。
        return QStringLiteral("[") + keys.join(QStringLiteral(", ")) + QStringLiteral("]");
    }
    std::vector<std::string> std_keys;
    std_keys.reserve(keys.size());
    for (const QString& key : keys)
        std_keys.push_back(key.toStdString());
    return QString::fromStdString(citation_numbers_->FormatPill(std_keys));
}

QString InlineEditor::CrossReferenceDisplayText(const QString& target) const {
    const auto it = xref_labels_.find(target);
    return it != xref_labels_.end() && !it->second.isEmpty() ? it->second : QStringLiteral("[ref]");
}

void InlineEditor::SetCitationNumbers(std::shared_ptr<const CitationNumberResolver> numbers) {
    if (citation_numbers_ == numbers)
        return;
    citation_numbers_ = std::move(numbers);
    RefreshObjectDisplays();
}

void InlineEditor::SetCrossReferenceLabels(std::map<QString, QString> labels) {
    if (xref_labels_ == labels)
        return;
    xref_labels_ = std::move(labels);
    RefreshObjectDisplays();
}

void InlineEditor::RefreshObjectDisplays() {
    if (!document() || !document()->documentLayout())
        return;
    // 重绘 pill 只改写 *格式*。基类仍会将其报告为内容变更，
    // 因此该行不能把它误认为用户输入：刷新显示绝不会把编辑器置为 dirty。
    refreshing_displays_ = true;
    QTextCursor cursor(document());
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.length() == 0)
                continue;
            const QTextCharFormat format = fragment.charFormat();
            const QVariant kind = format.property(kTokenKindProperty);
            if (!kind.isValid())
                continue;
            const TokenKind token_kind = static_cast<TokenKind>(kind.toInt());
            if (token_kind == TokenKind::Math)
                continue;
            const QString payload = format.property(kTokenPayloadProperty).toString();
            const QString display = token_kind == TokenKind::Citation
                                        ? CitationDisplayText(payload.split(',', Qt::SkipEmptyParts))
                                        : CrossReferenceDisplayText(payload);
            if (format.property(citation_format::kDisplayTextProperty).toString() == display) {
                continue;
            }
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            QTextCharFormat update;
            update.setProperty(citation_format::kDisplayTextProperty, display);
            cursor.mergeCharFormat(update);
        }
    }
    refreshing_displays_ = false;
    if (viewport())
        viewport()->update();
}

void InlineEditor::InsertMathObject(QTextCursor& cursor, const QString& latex) {
    ensurePolished();
    const QFont text_font = document()->defaultFont();

    // P0-07：插入轻量占位符并将渲染入队。GUI 线程绝不运行 TeX：
    // 20 秒以上的编译不再冻结窗口，结果通过服务的 queued 信号返回。
    const QString formula_id = QStringLiteral("f%1").arg(++next_formula_id_);

    QTextCharFormat format;
    format.setObjectType(inline_math_format::kObjectType);
    format.setFont(text_font);
    format.setVerticalAlignment(QTextCharFormat::AlignNormal);
    // 渲染完成之前也显示一个参与排版的胶囊。
    UpdateMathGeometry(&format, viewport()->width());
    format.setProperty(kTokenKindProperty, static_cast<int>(TokenKind::Math));
    // payload 仍然是 LaTeX 主体；formula id 通过 kind 字节的伴随属性传递，
    // 以便回复能定位到自己的对象。
    format.setProperty(kTokenPayloadProperty, latex);
    format.setProperty(kMathFormulaIdProperty, formula_id);
    cursor.insertText(QString(kObjectChar), format);
    // 后续输入绝不能继承语义对象的属性。
    cursor.setCharFormat(QTextCharFormat());

    RequestMathRender(formula_id, latex);
}

void InlineEditor::RequestMathRender(const QString& formula_id, const QString& latex) {
    const QFont text_font = document()->defaultFont();
    MathRenderStyle style;
    style.font_px = qMax(4, qRound(QFontMetricsF(text_font).height()));
    style.color = QColor(theme::kPrimaryText);
    style.font_family = text_font.family();
    style.template_id = property("template_id").toString();
    ++math_generation_;
    MathRenderService::Shared()->Request(objectName(), formula_id, latex, style);
}

void InlineEditor::UpdateMathGeometry(QTextCharFormat* format, int available_width) const {
    const QFontMetricsF metrics(document()->defaultFont());
    // AlignNormal 把对象高度计入行的 ascent。严格低于正文 ascent，
    // 因而任何公式都不能把这一行撑高。
    const qreal pill_height = qMax<qreal>(1.0, metrics.ascent() - 1.0);
    const qreal pad_x = qMax<qreal>(4.0, metrics.height() * 0.20);
    const qreal pad_y = qMin<qreal>(1.0, pill_height * 0.10);
    const qreal image_limit_height = qMax<qreal>(1.0, pill_height - 2.0 * pad_y);
    const QPixmap pixmap = format->property(inline_math_format::kPixmapProperty).value<QPixmap>();

    qreal image_width = metrics.horizontalAdvance(QStringLiteral("…"));
    qreal image_height = image_limit_height;
    qreal baseline = pill_height - pad_y;
    if (!pixmap.isNull()) {
        const qreal source_width =
            qMax<qreal>(1.0, format->property(inline_math_format::kSourceWidthProperty).toDouble());
        const qreal source_height =
            qMax<qreal>(1.0, format->property(inline_math_format::kSourceHeightProperty).toDouble());
        // 透明边缘已经在 worker 中去掉。以完整位图为单位，按胶囊的
        // 高和可用宽度等比放到最大，绝不单独压缩上高或下深。
        const qreal max_width = qMax<qreal>(1.0, available_width * kMaximumWidthFraction - 2.0 * pad_x);
        const qreal scale = qMin(image_limit_height / source_height, max_width / source_width);
        image_width = source_width * scale;
        image_height = source_height * scale;
        baseline = pad_y + format->property(inline_math_format::kSourceBaselineProperty).toDouble() * scale;
    }
    format->setVerticalAlignment(QTextCharFormat::AlignNormal);
    format->setProperty(inline_math_format::kImageWidthProperty, image_width);
    format->setProperty(inline_math_format::kImageHeightProperty, image_height);
    format->setProperty(inline_math_format::kWidthProperty, image_width + 2.0 * pad_x);
    format->setProperty(inline_math_format::kHeightProperty, pill_height);
    format->setProperty(inline_math_format::kBaselineProperty, qBound<qreal>(0.0, baseline, pill_height));
}

void InlineEditor::RefreshMathGeometry(int available_width, bool rerender) {
    if (updating_math_geometry_ || !document())
        return;
    QScopedValueRollback<bool> geometry_guard(updating_math_geometry_, true);
    QScopedValueRollback<bool> clean_guard(refreshing_displays_, true);
    std::vector<int> positions;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().objectType() == inline_math_format::kObjectType)
                positions.push_back(fragment.position());
        }
    }
    for (int position : positions) {
        QTextCursor token(document());
        token.setPosition(position);
        token.setPosition(position + 1, QTextCursor::KeepAnchor);
        QTextCharFormat format = token.charFormat();
        const QTextCharFormat old = format;
        format.setFont(document()->defaultFont());
        UpdateMathGeometry(&format, available_width);
        if (format != old)
            token.setCharFormat(format);
        if (rerender) {
            RequestMathRender(format.property(kMathFormulaIdProperty).toString(),
                              format.property(kTokenPayloadProperty).toString());
        }
    }
}

void InlineEditor::ApplyMathRender(const QString& formula_id, const QString& latex, const QImage& image, int width,
                                   int height, int baseline, qreal device_pixel_ratio, int render_font_px) {
    if (image.isNull() || width <= 0 || height <= 0)
        return;
    ensurePolished();

    // 按 id 定位公式对象。渲染在途期间，文档可能已重新加载、已撤销，
    // 或对象已被删除；无论哪种情况，都直接丢弃该回复（绝不崩溃）。
    QTextCursor found;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QTextCharFormat fmt = fragment.charFormat();
            if (fmt.objectType() != inline_math_format::kObjectType)
                continue;
            if (fmt.property(kMathFormulaIdProperty).toString() != formula_id)
                continue;
            if (fmt.property(kTokenPayloadProperty).toString() != latex)
                continue;
            found = QTextCursor(document());
            found.setPosition(fragment.position());
            found.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            break;
        }
        if (!found.isNull())
            break;
    }
    if (found.isNull())
        return;

    const QPixmap pixmap = PixmapFromMathResult([&] {
        MathRenderResult partial;
        partial.image = image;
        partial.device_pixel_ratio = device_pixel_ratio;
        return partial;
    }());

    QTextCharFormat update = found.charFormat();
    update.setProperty(inline_math_format::kPixmapProperty, QVariant::fromValue(pixmap));
    const qreal dpr = device_pixel_ratio > 0 ? device_pixel_ratio : 1.0;
    // 用真实像素尺寸保留宽高比，避免结果的整数逻辑尺寸各自取整造成形变。
    update.setProperty(inline_math_format::kSourceWidthProperty, image.width() / dpr);
    update.setProperty(inline_math_format::kSourceHeightProperty, image.height() / dpr);
    update.setProperty(inline_math_format::kSourceBaselineProperty, baseline);
    update.setProperty(inline_math_format::kRenderFontPxProperty, render_font_px);
    UpdateMathGeometry(&update, viewport()->width());
    // 这里只改变对象的格式——QTextDocument 绝不会重建或重新创建，
    // 因此光标和周围文本保持原样。
    QScopedValueRollback<bool> clean_guard(refreshing_displays_, true);
    found.setCharFormat(update);
    ResizeToContent();
    if (viewport())
        viewport()->update();
}

std::optional<InlineEditor::TokenHit> InlineEditor::TokenAt(int position) const {
    const int last = document()->characterCount() - 1;
    if (position < 0 || position >= last)
        return std::nullopt;
    QTextCursor cursor(document());
    cursor.setPosition(position);
    // 除非有内容被选中，否则 charFormat() 报告的是光标 *之前* 的字符，
    // 因此精确选中要检测的那个字符。
    cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
    const QTextCharFormat format = cursor.charFormat();
    const QVariant kind = format.property(kTokenKindProperty);
    if (!kind.isValid())
        return std::nullopt;
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
    // 丢失了 payload 的 token 字符（外部粘贴）会变成普通文本，
    // 而不是 Content() 会静默丢弃的 token。
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QString fragment_text = fragment.text();
            const bool has_token_char = fragment_text.contains(kTokenChar);
            const bool has_object_char = fragment_text.contains(kObjectChar);
            const bool has_kind = fragment.charFormat().property(kTokenKindProperty).isValid();
            if (has_token_char || has_object_char) {
                if (has_kind)
                    continue;
                // 孤立的 token 字符：将其剥离并重新扫描。
                QTextCursor cursor(document());
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                QString replacement = fragment_text;
                replacement.remove(kTokenChar);
                replacement.remove(kObjectChar);
                cursor.insertText(replacement);
                return; // textChanged 会重新扫描
            }
            if (has_kind) {
                // 对象身份标识泄漏到了普通字符上（例如通过停在
                // pill 后面的原始光标输入）。剥离语义——绝不能让
                // 普通文本伪装成 token。setCharFormat 会替换整个格式，
                // 因此保留两个合法的视觉属性（粗体 / 斜体）。
                QTextCursor cursor(document());
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                QTextCharFormat clean;
                clean.setFontWeight(fragment.charFormat().fontWeight());
                clean.setFontItalic(fragment.charFormat().fontItalic());
                cursor.setCharFormat(clean);
                return; // textChanged 会重新扫描
            }
        }
    }
}

void InlineEditor::InsertCitationObject(const QStringList& keys) {
    QStringList cleaned;
    for (const QString& key : keys) {
        const QString trimmed = key.trimmed();
        if (!trimmed.isEmpty())
            cleaned << trimmed;
    }
    if (cleaned.isEmpty())
        return;
    QTextCursor cursor = textCursor();
    InsertPillObject(cursor, TokenKind::Citation, cleaned.join(QLatin1Char(',')), CitationDisplayText(cleaned));
    setTextCursor(cursor);
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::InsertCrossReferenceObject(const QString& target_node) {
    if (target_node.isEmpty())
        return;
    QTextCursor cursor = textCursor();
    InsertPillObject(cursor, TokenKind::CrossReference, target_node, CrossReferenceDisplayText(target_node));
    setTextCursor(cursor);
    dirty_ = true;
    ResizeToContent();
}

void InlineEditor::InsertInlineMath(const QString& latex) {
    if (latex.trimmed().isEmpty())
        return;
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
    if (!hit || hit->kind != TokenKind::Math)
        return;
    OpenMathEditor(hit);
}

void InlineEditor::OpenMathEditor(const std::optional<TokenHit>& hit) {
    if (math_editor_open_)
        return;
    math_editor_open_ = true;
    QTextCursor target = textCursor();
    if (hit) {
        target.setPosition(hit->position);
        target.setPosition(hit->position + 1, QTextCursor::KeepAnchor);
    }
    // 堆所有权加 open() 可避免嵌套事件循环，
    // 并防止行重建时删除栈上分配的子对话框。
    auto* dialog = new MathEditorDialog(hit ? hit->payload : QString(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog, target](int result) mutable {
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
        // 在提交可能重建父行之前，先完成对话框的信号投递。
        // 若该行被移除，上下文绑定会取消这一操作。
        QTimer::singleShot(0, this, [this]() {
            setFocus(Qt::OtherFocusReason);
            emit Committed();
        });
    });
    dialog->open();
}

// ---------------- 事件 ----------------

void InlineEditor::keyPressEvent(QKeyEvent* event) {
    // Backspace/Delete 删除整个 token，绝不删除半个。
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        const QString text = toPlainText();
        const int position = textCursor().position();
        const int probe = event->key() == Qt::Key_Backspace ? position - 1 : position;
        if (!textCursor().hasSelection() && probe >= 0 && probe < text.length() &&
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
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_B) {
        ToggleBold();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_I) {
        ToggleItalic();
        return;
    }
    QTextEdit::keyPressEvent(event);
    ResizeToContent();
}

bool InlineEditor::canInsertFromMimeData(const QMimeData*) const {
    return true;
}

void InlineEditor::insertFromMimeData(const QMimeData* source) {
    if (!source)
        return;
    // 编辑器自身的富内容（marks、token、数学对象）通过私有剪贴板
    // flavour 在文档内复制/粘贴时得以保留。
    if (source->hasFormat(InlineMimeType())) {
        const std::string encoded = source->data(InlineMimeType()).toStdString();
        const InlineContent content = InlineFromRichText(encoded);
        InsertContent(content);
        dirty_ = true;
        ResizeToContent();
        return;
    }
    // Ctrl+Shift+V：仅纯文本（方案 §14）。普通 Ctrl+V 保留粗体/斜体，
    // 丢弃字体、字号、颜色和间距。
    const bool plain_only = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (plain_only || !source->hasHtml()) {
        const QString pasted = source->text();
        if (pasted.isEmpty())
            return;
        const QString reflowed = QString::fromStdString(ReflowHardWrappedText(pasted.toStdString()));
        textCursor().insertText(reflowed);
        dirty_ = true;
        ResizeToContent();
        return;
    }
    // 富文本粘贴：转换 HTML，仅保留字重/斜体。
    QTextDocument converted;
    converted.setHtml(source->html());
    QTextCursor cursor = textCursor();
    for (QTextBlock block = converted.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat allowed;
            allowed.setFontWeight(fragment.charFormat().fontWeight());
            allowed.setFontItalic(fragment.charFormat().fontItalic());
            cursor.insertText(fragment.text(), allowed);
        }
        if (block.next().isValid())
            cursor.insertText(QStringLiteral("\n"));
    }
    dirty_ = true;
    ResizeToContent();
}

QMimeData* InlineEditor::createMimeDataFromSelection() const {
    // 自行构建 QMimeData，而不是装饰基类返回的那个：某些平台的
    // 剪贴板后端返回的是包装对象，其存储并非普通的 QMimeData map，
    // 写入其中的数据会被静默丢弃。重新创建该对象可保留标准的
    // plain/html flavour，并让私有 flavour 得以保留。
    QMimeData* base = QTextEdit::createMimeDataFromSelection();
    auto* data = new QMimeData();
    if (base) {
        const QStringList formats = base->formats();
        for (const QString& format : formats) {
            if (format == QStringLiteral("text/plain") || format == QStringLiteral("text/html")) {
                continue; // 通过下面的访问器恢复
            }
            data->setData(format, base->data(format));
        }
        if (base->hasHtml())
            data->setHtml(base->html());
        if (base->hasText())
            data->setText(base->text());
        delete base;
    }
    const QTextCursor cursor = const_cast<InlineEditor*>(this)->textCursor();
    if (cursor.hasSelection()) {
        const InlineContent content = ContentInRange(cursor.selectionStart(), cursor.selectionEnd());
        const std::string encoded = InlineToRichText(content);
        data->setData(InlineMimeType(), QByteArray::fromStdString(encoded));
    }
    return data;
}

void InlineEditor::mousePressEvent(QMouseEvent* event) {
    // 点击 token 会整体选中它（方案 §5）。
    const QString text = toPlainText();
    const int position = document()->documentLayout()->hitTest(event->pos(), Qt::FuzzyHit);
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
    // 双击行内数学对象会打开 LaTeX 源码编辑器。
    const QString text = toPlainText();
    const int position = document()->documentLayout()->hitTest(event->pos(), Qt::FuzzyHit);
    const int candidates[] = {position, position - 1};
    for (const int probe : candidates) {
        if (probe < 0 || probe >= text.length())
            continue;
        if (text.at(probe) != kObjectChar)
            continue;
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
    RefreshMathGeometry(event->size().width() - 2 * frameWidth(), false);
    // 使用传入的宽度进行测量：在 resize 事件投递期间，
    // width() 仍报告旧值。
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
    if (height() != target)
        setFixedHeight(target);
    updateGeometry();
}

void InlineEditor::focusOutEvent(QFocusEvent* event) {
    QTextEdit::focusOutEvent(event);
    // 由引用/交叉引用选择器（或数学编辑器）打开导致的焦点丢失
    // *不* 是「用户完成编辑」（引用方案 §4）：选择器会自行
    // 插入对象并提交。
    if (!math_editor_open_ && !IsProtectedInsertOpen())
        emit Committed();
}

} // namespace pf::gui
