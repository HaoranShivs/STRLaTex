#include "app/BlockEditor.h"

#include <QFontMetrics>
#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTimer>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QToolButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QPainter>
#include <QCursor>
#include <functional>
#include <initializer_list>

#include "app/MathPreviewRenderer.h"
#include "app/Theme.h"
#include "document/InlineText.h"
#include "math/MathValidator.h"

namespace pf::gui {

namespace {
using theme::kAccent;

QString ToQ(const std::string& s) { return QString::fromStdString(s); }
std::string ToStd(const QString& s) { return s.toStdString(); }

// Reordering uses a private mime type so the editor ignores drags from
// outside (files, text) and other apps ignore ours.
constexpr const char* kBlockMime = "application/x-paperforge-block";

// Block kinds offered by the insert affordances, in menu order.
// Superseded by InsertOptionsFor(), which filters by position and template
// capability; kept only for the block menu's generic listing.
struct InsertEntry {
    const char* label;
    const char* kind;
};

std::vector<InsertEntry> InsertEntries(bool body_empty) {
    if (body_empty) {
        // A text block needs a heading to live in, and a section needs no
        // anchor, so an empty body can only start with a section.
        return {{"Section Title", "section"}};
    }
    return {{"Text", "text"}, {"Section Title", "section"},
            {"Subsection Title", "subsection"}, {"Equation", "equation"},
            {"Figure", "figure"}, {"Import Table…", "table"}};
}

// Rows whose content is a run of prose: they re-flow to the block width, so a
// hard-wrapped paste has to be softened (see ReflowHardWrappedText).
bool IsProseRole(const QString& role) {
    return role == QLatin1String("abstract") ||
           role == QLatin1String("paragraph") ||
           role == QLatin1String("caption");
}

// What may be inserted relative to `container_kind`, using the template's
// heading capability (plan §3, §9). The names are EditorItemKind machine
// names; MainWindow maps them onto edit commands.
struct InsertOption {
    const char* label;
    const char* kind;     // EditorItemKindName
    const char* group;    // menu group: Structure / Content
};

std::vector<InsertOption> InsertOptionsFor(bool body_empty,
                                           NodeKind container_kind,
                                           int max_heading_depth) {
    const bool in_section = container_kind == NodeKind::Section;
    const bool in_subsection = container_kind == NodeKind::Subsection;
    const bool in_subsubsection = container_kind == NodeKind::Subsubsection;

    if (body_empty) {
        // A text block needs a heading to live in, and a section needs no
        // anchor, so an empty body can only start with a section.
        return {{"Section Title", "section", "Structure"}};
    }

    std::vector<InsertOption> options;
    // A heading may only be inserted at a level the container can hold, and
    // only if the template supports that depth.
    const int container_depth = HeadingDepth(container_kind);
    if (!in_subsubsection && 1 <= max_heading_depth) {
        options.push_back({"Section Title", "section", "Structure"});
    }
    if (!in_subsubsection && 2 <= max_heading_depth) {
        options.push_back({"Subsection Title", "subsection", "Structure"});
    }
    if ((in_section || in_subsection) && 3 <= max_heading_depth) {
        options.push_back({"Subsubsection Title", "subsubsection", "Structure"});
    }
    (void)container_depth;
    options.push_back({"Text", "text", "Content"});
    options.push_back({"Equation", "equation", "Content"});
    options.push_back({"Figure", "figure", "Content"});
    options.push_back({"Import Table…", "table", "Content"});
    return options;
}

// Which structural container an anchor node sits in, so the insert menu can
// offer only what the model can express there. Blocks inherit the container
// they live in; a heading is its own container.
NodeKind ContainerKindFor(const Document& doc, const QString& anchor) {
    if (anchor.isEmpty()) return NodeKind::Section;
    auto address = LocateNode(doc, NodeId(anchor.toStdString()));
    if (!address) return NodeKind::Section;
    switch (address->kind) {
        case NodeKind::Section:
        case NodeKind::Paragraph:
        case NodeKind::Figure:
        case NodeKind::Table:
        case NodeKind::Equation:
            // A block inside a section (or the section heading itself) means
            // the next row is still owned by the section.
            return NodeKind::Section;
        case NodeKind::Subsection:
            return NodeKind::Subsection;
        case NodeKind::Subsubsection:
            return NodeKind::Subsubsection;
    }
    return NodeKind::Section;
}

// Editor that sizes itself to its content and exposes key events for / and @.
class BlockEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    void setReflowOnPaste(bool enabled) { reflow_on_paste_ = enabled; }

    explicit BlockEdit(bool single_line, QWidget* parent = nullptr)
        : QPlainTextEdit(parent), single_line_(single_line) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWordWrapMode(QTextOption::WordWrap);
        setFrameShape(QFrame::NoFrame);
        document()->setDocumentMargin(6);
        // NOTE: deliberately no idle/"typing" timer. An auto-commit a few
        // hundred milliseconds after the last keystroke made every pause in
        // typing rewrite the document, which rebuilt every row and destroyed
        // the text still being entered. Edits are committed on focus-out,
        // Enter and Ctrl+Enter only (design: no implicit content refresh).
        connect(this, &QPlainTextEdit::textChanged, this, [this]() {
            Resize();
            if (!loading_) dirty_ = true;
        });
        // Re-fit when the layout changes for any other reason: a width change
        // that re-wraps the text, or a font/zoom change.
        connect(document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged, this,
                [this](const QSizeF&) { Resize(); });
        Resize();
    }

    // Programmatic load from the document: never marks the row dirty and
    // never triggers a commit.
    void SetInitialText(const QString& text) {
        loading_ = true;
        setPlainText(text);
        loading_ = false;
        dirty_ = false;
    }

    bool IsDirty() const { return dirty_; }
    void MarkClean() { dirty_ = false; }
    void MarkDirty() { dirty_ = true; }
    // Wrap programmatic formatting (alignment, fonts, hints) so it cannot be
    // mistaken for user input.
    void BeginProgrammaticEdit() { loading_ = true; }
    void EndProgrammaticEdit() {
        loading_ = false;
        dirty_ = false;
    }

    // Adopt model text while keeping the caret roughly in place.
    void SetTextFromModel(const QString& text) {
        loading_ = true;
        const int position = textCursor().position();
        setPlainText(text);
        QTextCursor cursor = textCursor();
        cursor.setPosition(qMin(position, text.length()));
        setTextCursor(cursor);
        loading_ = false;
        dirty_ = false;
    }

    void SetMinimumContentHeight(int px) {
        min_height_ = px;
        Resize();
    }

    // Grow/shrink to fit the wrapped text exactly. Nothing here scrolls: the
    // single vertical scrollbar lives on the editor pane around all blocks.
    // Height of the wrapped text in pixels.
    //
    // Two Qt traps live here. QPlainTextDocumentLayout's documentSize() and
    // QTextDocument::size() report the *line count* (1, 2, 3 ...), not a
    // height; and per-block bounding rects are only meaningful for blocks the
    // layout has actually processed, so reading the last block's bottom gave
    // one line for a pasted multi-line abstract. Measuring the plain text with
    // the widget's own font is reliable; the sum of laid-out block heights is
    // a second opinion for what font metrics cannot see (tab stops, per-block
    // formats).
    qreal ContentHeight() const {
        QTextDocument* doc = document();
        const QAbstractTextDocumentLayout* layout = doc->documentLayout();
        qreal per_block = 0.0;
        for (QTextBlock block = doc->begin(); block.isValid();
             block = block.next()) {
            per_block += layout->blockBoundingRect(block).height();
        }
        const int wrap_width = qMax(1, viewport()->width());
        const qreal measured =
            fontMetrics()
                .boundingRect(QRect(0, 0, wrap_width, 0), Qt::TextWordWrap,
                              toPlainText())
                .height();
        return qMax(measured, per_block) + 2.0 * doc->documentMargin();
    }

    void Resize() {
        const qreal doc_height = ContentHeight();
        const int target =
            qMax(qMax(static_cast<int>(qCeil(doc_height)) + 2 * frameWidth() +
                          6,
                      fontMetrics().height() + 12),
                 min_height_);
        if (height() != target) setFixedHeight(target);
        updateGeometry();
    }

signals:
    void TriggerSlash();
    void TriggerAt();
    void CommitRequested();
    void NewBlockAfter();

protected:
    void keyPressEvent(QKeyEvent* event) override {
        // "/" at line start with empty-ish context opens the block menu.
        if (property("commands_enabled").toBool() &&
            event->text() == QStringLiteral("/")) {
            if (toPlainText().trimmed().isEmpty()) {
                emit TriggerSlash();
                return;
            }
        }
        if (property("references_enabled").toBool() &&
            event->text() == QStringLiteral("@")) {
            emit TriggerAt();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            emit CommitRequested();
            emit NewBlockAfter();
            return;
        }
        if (single_line_ &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            emit CommitRequested();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
        if (document()->blockCount() >= 1) Resize();
    }

    void focusOutEvent(QFocusEvent* event) override {
        QPlainTextEdit::focusOutEvent(event);
        emit CommitRequested();
    }

    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        Resize();
    }

    // Undo the hard wrapping a PDF copy brings along, so the paragraph can
    // re-flow to the block width instead of staying a fixed column wide.
    void insertFromMimeData(const QMimeData* source) override {
        if (reflow_on_paste_ && source && source->hasText()) {
            const QString pasted = source->text();
            const QString reflowed =
                ToQ(pf::ReflowHardWrappedText(ToStd(pasted)));
            if (reflowed != pasted) {
                QMimeData adjusted;
                adjusted.setText(reflowed);
                QPlainTextEdit::insertFromMimeData(&adjusted);
                return;
            }
        }
        QPlainTextEdit::insertFromMimeData(source);
    }

private:
    bool single_line_ = false;
    bool loading_ = false;
    bool dirty_ = false;  // user typed something not yet in the document
    bool reflow_on_paste_ = false;  // long-text rows re-flow pasted text
    int min_height_ = 0;
};

class InlineTokenHighlighter final : public QSyntaxHighlighter {
public:
    explicit InlineTokenHighlighter(QTextDocument* document)
        : QSyntaxHighlighter(document) {}

protected:
    void highlightBlock(const QString& text) override {
        static const QRegularExpression token_pattern(
            QStringLiteral(R"(\[(?:cite|ref):[^\]]+\])"));
        QTextCharFormat format;
        format.setForeground(QColor(theme::kAccent));
        format.setBackground(QColor(theme::kAccentSoft));
        format.setFontWeight(QFont::DemiBold);
        auto matches = token_pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};

// The "\u22ee\u22ee" grip in a card header. Dragging it starts a reorder; the gaps
// between blocks accept the drop.
class DragHandle : public QWidget {
    Q_OBJECT

public:
    DragHandle(const QString& node_id, QWidget* parent)
        : QWidget(parent), node_id_(node_id) {
        setFixedWidth(18);
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("Drag to move this block"));
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(theme::kDisabledText), 1.4));
        // The two columns of a grip.
        const int cx = width() / 2;
        for (int dx : {-2, 2}) {
            painter.drawLine(cx + dx, 4, cx + dx, height() - 4);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            press_pos_ = event->pos();
            pressed_ = true;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!pressed_ || node_id_.isEmpty()) return;
        if ((event->pos() - press_pos_).manhattanLength() < 6) return;
        pressed_ = false;

        auto* mime = new QMimeData();
        mime->setData(kBlockMime, node_id_.toUtf8());
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        // A small pixmap so the cursor shows what is being carried.
        QPixmap preview(120, 18);
        preview.fill(QColor(theme::kAccentSoft));
        QPainter painter(&preview);
        painter.setPen(QColor(theme::kAccent));
        painter.drawRect(0, 0, preview.width() - 1, preview.height() - 1);
        painter.drawText(6, 13, QStringLiteral("moving block"));
        drag->setPixmap(preview);
        drag->exec(Qt::MoveAction);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        pressed_ = false;
        QWidget::mouseReleaseEvent(event);
    }

private:
    QString node_id_;
    QPoint press_pos_;
    bool pressed_ = false;
};

// The strip between two blocks. It always reserves its height (so nothing
// jumps when the pointer arrives) and reveals an insert button on hover, which
// is how a block is added without hunting for a toolbar. The last block gets
// one too, and an empty body gets one after the front matter.
class BlockGap : public QWidget {
    Q_OBJECT

public:
    BlockGap(std::function<void(QWidget*)> on_activate,
             std::function<void(const QString&)> on_drop, QWidget* parent)
        : QWidget(parent),
          on_activate_(std::move(on_activate)),
          on_drop_(std::move(on_drop)) {
        setAcceptDrops(true);
        setFixedHeight(16);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Insert a block here"));
        setAttribute(Qt::WA_Hover, true);
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        QWidget::enterEvent(event);
        hovered_ = true;
        update();
    }

    void leaveEvent(QEvent* event) override {
        QWidget::leaveEvent(event);
        hovered_ = false;
        update();
    }

    void paintEvent(QPaintEvent*) override {
        if (!hovered_ && !drop_active_) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const int mid = height() / 2;
        // A hairline across the content width, with the button in the middle.
        const int inset = 12;
        QColor line(theme::kAccent);
        line.setAlpha(drop_active_ ? 255 : 70);
        painter.setPen(QPen(line, drop_active_ ? 2 : 1));
        painter.drawLine(inset, mid, width() - inset, mid);
        if (drop_active_) return;  // the line alone marks the drop target

        const QPoint centre(width() / 2, mid);
        const int radius = 7;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(theme::kAccent));
        painter.drawEllipse(centre, radius, radius);
        painter.setPen(QPen(Qt::white, 1.6));
        painter.drawLine(centre.x() - 3, centre.y(), centre.x() + 3, centre.y());
        painter.drawLine(centre.x(), centre.y() - 3, centre.x(), centre.y() + 3);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (on_activate_) on_activate_(this);
        event->accept();
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(kBlockMime)) {
            drop_active_ = true;
            update();
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat(kBlockMime)) {
            event->acceptProposedAction();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        drop_active_ = false;
        update();
        QWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        drop_active_ = false;
        update();
        if (!event->mimeData()->hasFormat(kBlockMime)) return;
        event->acceptProposedAction();
        applyDrop(QString::fromUtf8(event->mimeData()->data(kBlockMime)));
    }

public slots:
    // What a completed drop does. Split out so the reorder can be driven
    // without Qt's drag manager, which needs a real pointer device.
    void applyDrop(const QString& node_id) {
        if (on_drop_) on_drop_(node_id);
    }

private:
    std::function<void(QWidget*)> on_activate_;
    std::function<void(const QString&)> on_drop_;
    bool hovered_ = false;
    bool drop_active_ = false;
};

}  // namespace

#include "app/BlockEditor.moc"

// ---------------- BlockEditor ----------------

BlockEditor::BlockEditor(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    host_ = new QWidget(scroll_);
    host_->setStyleSheet(QString("background: %1;").arg(theme::kEditorBackground));
    auto* host_layout = new QVBoxLayout(host_);
    host_layout->setContentsMargins(24, 20, 24, 20);
    host_layout->setSpacing(6);
    host_layout->addStretch(1);
    scroll_->setWidget(host_);
    outer->addWidget(scroll_);
}

QWidget* BlockEditor::BuildFormatToolbar(InlineEditor* editor) {
    auto* bar = new QWidget(this);
    bar->setProperty("formatBar", true);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(4);

    const QString style = QString(
        "QToolButton { background: transparent; color: %1; border: none;"
        " border-radius: 4px; padding: 2px 7px; font-weight: 600; }"
        "QToolButton:hover { background: %2; color: %3; }")
        .arg(theme::kSecondaryText, theme::kAccentSoft, theme::kAccent);

    auto* bold = new QToolButton(bar);
    bold->setText(QStringLiteral("B"));
    bold->setToolTip(QStringLiteral("Bold (Ctrl+B)"));
    bold->setAutoRaise(true);
    bold->setStyleSheet(style);
    connect(bold, &QToolButton::clicked, editor,
            [editor]() { editor->ToggleBold(); });

    auto* italic = new QToolButton(bar);
    italic->setText(QStringLiteral("I"));
    italic->setToolTip(QStringLiteral("Italic (Ctrl+I)"));
    italic->setAutoRaise(true);
    italic->setStyleSheet(QString(style) + "QToolButton { font-style: italic; }");
    connect(italic, &QToolButton::clicked, editor,
            [editor]() { editor->ToggleItalic(); });

    auto* math = new QToolButton(bar);
    math->setText(QStringLiteral("Inline Math"));
    math->setToolTip(QStringLiteral("Insert an inline equation"));
    math->setAutoRaise(true);
    math->setStyleSheet(style);
    connect(math, &QToolButton::clicked, editor,
            [editor]() { editor->BeginInlineMath(); });

    auto* citation = new QToolButton(bar);
    citation->setText(QStringLiteral("Citation"));
    citation->setToolTip(QStringLiteral("Insert a citation"));
    citation->setAutoRaise(true);
    citation->setStyleSheet(style);
    connect(citation, &QToolButton::clicked, this,
            [this, editor]() { ShowCitationPicker(editor); });

    auto* reference = new QToolButton(bar);
    reference->setText(QStringLiteral("Reference"));
    reference->setToolTip(QStringLiteral("Insert a cross reference"));
    reference->setAutoRaise(true);
    reference->setStyleSheet(style);
    connect(reference, &QToolButton::clicked, this,
            [this, editor]() { ShowReferencePicker(editor); });

    // The buttons must never take focus. If one did, clicking it would end the
    // row's edit, commit the pre-format content, and rebuild every card before
    // the click handler ran - so the mark landed on a destroyed editor and the
    // row was re-laid out mid-edit (the "big blank area" report).
    bold->setFocusPolicy(Qt::NoFocus);
    italic->setFocusPolicy(Qt::NoFocus);
    math->setFocusPolicy(Qt::NoFocus);
    citation->setFocusPolicy(Qt::NoFocus);
    reference->setFocusPolicy(Qt::NoFocus);

    layout->addWidget(bold);
    layout->addWidget(italic);
    layout->addSpacing(6);
    layout->addWidget(math);
    layout->addWidget(citation);
    layout->addWidget(reference);
    layout->addStretch(1);
    bar->setStyleSheet(QString("background: %1;").arg(theme::kEditorBackground));
    return bar;
}

void BlockEditor::ShowCitationPicker(InlineEditor* editor) {
    std::vector<PopupList::Item> items;
    for (const auto& item : reference_items_) {
        if (!item.payload.startsWith(QStringLiteral("cite:"))) continue;
        PopupList::Item entry;
        entry.label = item.label;
        entry.detail = item.detail;
        entry.group = QStringLiteral("Citations");
        entry.payload = item.payload.mid(5);
        entry.search = item.label.toLower();
        items.push_back(std::move(entry));
    }
    if (items.empty()) {
        PopupList::Item empty;
        empty.label = QStringLiteral("No references imported");
        empty.detail = QStringLiteral("Import a .bib file first");
        items.push_back(empty);
    }
    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this, [editor](QString payload) {
        editor->InsertCitationToken({payload});
    });
    popup->popup(editor->mapToGlobal(QPoint(24, editor->height() + 4)), items);
}

void BlockEditor::ShowReferencePicker(InlineEditor* editor) {
    std::vector<PopupList::Item> items;
    // Cross references point at document nodes, so the items come from the
    // last rebuild's reference list where they were tagged as node:….
    for (const auto& item : reference_items_) {
        if (!item.payload.startsWith(QStringLiteral("xref:"))) continue;
        PopupList::Item entry;
        entry.label = item.label;
        entry.detail = item.detail;
        entry.group = QStringLiteral("Cross References");
        entry.payload = item.payload.mid(5);
        entry.search = item.label.toLower();
        items.push_back(std::move(entry));
    }
    if (items.empty()) {
        PopupList::Item empty;
        empty.label = QStringLiteral("Nothing to reference yet");
        empty.detail = QStringLiteral("Add a section, figure or equation first");
        items.push_back(empty);
    }
    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this, [editor](QString payload) {
        editor->InsertCrossReferenceToken(payload);
    });
    popup->popup(editor->mapToGlobal(QPoint(24, editor->height() + 4)), items);
}

QWidget* BlockEditor::MakeTextCard(const QString& node_id,
                                   const InlineContent& content) {
    auto* card = MakeCard(node_id, QStringLiteral("Text"),
                          QStringLiteral("paragraph"), true);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());

    auto* editor = new InlineEditor(card);
    editor->SetContent(content);
    editor->setProperty("row_node", node_id);
    editor->setProperty("row_focus_key", node_id);
    editor->setPlaceholderText(QStringLiteral("Write text…  @ inserts a reference, Ctrl+Enter adds a block"));

    connect(editor, &InlineEditor::Committed, this, [this, editor, node_id]() {
        for (auto& block : blocks_) {
            if (block.inline_editor != editor) continue;
            const InlineContent content = editor->Content();
            if (content == block.committed_content) {
                editor->MarkClean();
                emit RowCommitted();
                return;
            }
            block.committed_content = content;
            editor->MarkClean();
            emit ParagraphContentEdited(node_id, content);
            emit RowCommitted();
            return;
        }
    });
    connect(editor, &InlineEditor::NewBlockAfter, this, [this, node_id]() {
        emit InsertBlockRequested(QStringLiteral("text"), node_id);
    });

    card_layout->addWidget(editor);
    card_layout->addWidget(BuildFormatToolbar(editor));

    Block block;
    block.node_id = node_id;
    block.kind = QStringLiteral("Text");
    block.card = card;
    block.inline_editor = editor;
    block.commit_role = QStringLiteral("paragraph");
    block.committed_text = ToQ(pf::InlineToPlainText(content));
    block.committed_content = content;
    blocks_.push_back(std::move(block));
    return card;
}

QWidget* BlockEditor::MakeEquationCard(const QString& node_id,
                                       const pf::EquationBlock& equation) {
    // Design §4: the equation row owns LaTeX Source, Preview, Numbered and
    // Label. The user only ever edits the math body.
    auto* card = MakeCard(node_id, QStringLiteral("Equation"),
                          QStringLiteral("equation"), true);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());

    auto* source = NewEditor(card, ToQ(equation.expression.latex), true, 2);
    source->setPlaceholderText(
        QStringLiteral("LaTeX body, e.g. \\frac{\\partial u}{\\partial t}"));
    source->setProperty("row_node", node_id);
    source->setProperty("row_focus_key", node_id);
    source->setProperty("commands_enabled", true);
    if (auto* block_edit = qobject_cast<BlockEdit*>(source)) {
        block_edit->setReflowOnPaste(false);
        block_edit->MarkClean();
    }

    auto* preview = new QLabel(card);
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(56);
    preview->setStyleSheet(
        QString("background: %1; border: 1px solid %2; border-radius: 6px;"
                " color: %3;")
            .arg(theme::kEditorBackground, theme::kDivider,
                 theme::kSecondaryText));
    card_layout->addWidget(preview);

    auto* status = new QLabel(card);
    status->setWordWrap(true);
    card_layout->addWidget(status);

    auto* controls = new QWidget(card);
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(6);
    auto* numbered = new QCheckBox(QStringLiteral("Numbered"), controls);
    numbered->setChecked(equation.numbered);
    numbered->setProperty("row_node", node_id);
    numbered->setToolTip(QStringLiteral(
        "Numbered equations get an equation number and a label"));
    auto* label_caption = new QLabel(QStringLiteral("Label"), controls);
    label_caption->setStyleSheet(
        QString("color: %1;").arg(theme::kSecondaryText));
    auto* label_edit = new QLineEdit(controls);
    label_edit->setPlaceholderText(QStringLiteral("eq:energy"));
    label_edit->setText(ToQ(equation.label));
    label_edit->setProperty("row_node", node_id);
    label_edit->setToolTip(
        QStringLiteral("LaTeX label used by cross references, e.g. eq:energy"));
    controls_layout->addWidget(numbered);
    controls_layout->addWidget(label_caption);
    controls_layout->addWidget(label_edit, 1);
    card_layout->addWidget(controls);

    // Source changed -> validation -> render -> preview (design §7).
    auto refresh = [preview, status](const QString& latex) {
        const pf::MathValidation validation =
            pf::ValidateMath(latex.toStdString(), pf::MathFlavor::Display);
        if (validation.invalid()) {
            status->setText(QStringLiteral("Invalid — %1")
                                .arg(QString::fromStdString(validation.error)));
            status->setStyleSheet(QString("color: %1;").arg(theme::kError));
        } else if (validation.pending()) {
            status->setText(QStringLiteral("Empty equation"));
            status->setStyleSheet(
                QString("color: %1;").arg(theme::kSecondaryText));
        } else {
            status->setText(QString());
            status->setStyleSheet(QString());
        }
        MathRenderStyle style;
        style.font_px = 22;
        const MathRenderResult rendered = RenderMathPreview(latex, style);
        if (rendered.pixmap.isNull()) {
            preview->setPixmap(QPixmap());
            preview->setText(QStringLiteral("—"));
        } else {
            preview->setText(QString());
            preview->setPixmap(rendered.pixmap);
        }
    };
    refresh(ToQ(equation.expression.latex));

    auto* timer = new QTimer(card);
    timer->setSingleShot(true);
    timer->setInterval(160);
    connect(source, &QPlainTextEdit::textChanged, timer,
            [timer]() { timer->start(); });
    connect(timer, &QTimer::timeout, card,
            [source, refresh]() { refresh(source->toPlainText()); });

    Block block;
    block.node_id = node_id;
    block.kind = QStringLiteral("Equation");
    block.card = card;
    block.editor = source;
    block.commit_role = QStringLiteral("equation");
    block.committed_text = ToQ(equation.expression.latex);
    block.equation_numbered = equation.numbered;
    block.equation_label = ToQ(equation.label);
    blocks_.push_back(std::move(block));

    BlockEdit* block_edit = qobject_cast<BlockEdit*>(source);
    connect(block_edit, &BlockEdit::CommitRequested, this, [this, source]() {
        for (auto& candidate : blocks_) {
            if (candidate.editor == source) {
                CommitBlock(candidate);
                break;
            }
        }
    });

    // Numbered / label changes are separate attributes; they commit at once.
    // The block is looked up by its editor because `blocks_` can reallocate
    // while the rest of the document is still being rebuilt.
    connect(numbered, &QCheckBox::toggled, this,
            [this, source, label_edit](bool on) {
                for (auto& candidate : blocks_) {
                    if (candidate.editor != source) continue;
                    candidate.equation_numbered = on;
                    emit EquationEdited(candidate.node_id,
                                        source->toPlainText(), on,
                                        label_edit->text());
                    break;
                }
            });
    connect(label_edit, &QLineEdit::editingFinished, this,
            [this, source, numbered]() {
                const QLineEdit* edit = qobject_cast<QLineEdit*>(sender());
                const QString label = edit ? edit->text() : QString();
                for (auto& candidate : blocks_) {
                    if (candidate.editor != source) continue;
                    candidate.equation_label = label;
                    emit EquationEdited(candidate.node_id,
                                        source->toPlainText(),
                                        numbered->isChecked(), label);
                    break;
                }
            });

    return card;
}
QWidget* BlockEditor::MakeCard(const QString& node_id, const QString& kind,
                               const QString& commit_role, bool header_inline) {
    auto* card = new QFrame(host_);
    card->setObjectName("blockCard");
    card->setProperty("row_node", node_id);
    card->setProperty("row_kind", kind);
    card->setProperty("commit_role", commit_role);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(10, 4, 10, 4);
    card_layout->setSpacing(2);

    // Hover header (space always reserved to avoid layout jumps, design #61).
    auto* header = new QWidget(card);
    header->setFixedHeight(18);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(6);
    auto* handle = new DragHandle(node_id, header);
    auto* type_label = new QLabel(kind, header);
    type_label->setStyleSheet(QString("color: %1; font-size: 8pt; font-weight: 700;")
                                  .arg(theme::kSecondaryText));
    header_layout->addWidget(handle);
    header_layout->addWidget(type_label);
    header_layout->addStretch(1);

    // More ("⋯") button with a context menu.
    auto* more = new QToolButton(header);
    more->setText("⋯");
    more->setAutoRaise(true);
    more->setFixedSize(22, 18);
    more->setStyleSheet(QString("QToolButton { color: %1; border: none; }"
                                "QToolButton:hover { background: %2; border-radius: 4px; }")
                            .arg(theme::kSecondaryText, theme::kAccentSoft));
    const bool can_restructure = !node_id.isEmpty();
    const bool can_reflow = IsProseRole(commit_role);
    more->setVisible(false);
    connect(more, &QToolButton::clicked, this,
            [this, more, card, node_id, can_restructure, can_reflow]() {
                if (!can_restructure && !can_reflow) return;
                QMenu menu(this);
                QMenu* insert_menu = nullptr;
                QAction* up = nullptr;
                QAction* down = nullptr;
                QAction* del = nullptr;
                QAction* reflow = nullptr;
                if (can_restructure) {
                    insert_menu = menu.addMenu("Insert Block Below");
                    for (const auto& entry : InsertEntries(false)) {
                        QAction* action =
                            insert_menu->addAction(QString::fromUtf8(entry.label));
                        action->setData(QString::fromUtf8(entry.kind));
                    }
                }
                if (can_reflow) {
                    reflow = menu.addAction("Reflow Text");
                    reflow->setToolTip(
                        "Undo the hard line breaks of a pasted paragraph so it "
                        "wraps to the block width");
                }
                if (can_restructure) {
                    menu.addSeparator();
                    up = menu.addAction("Move Up");
                    down = menu.addAction("Move Down");
                    menu.addSeparator();
                    del = menu.addAction("Delete");
                }
                QAction* chosen_action =
                    menu.exec(more->mapToGlobal(QPoint(0, 18)));
                if (chosen_action == nullptr) return;
                if (insert_menu && chosen_action->parent() == insert_menu) {
                    emit InsertBlockRequested(chosen_action->data().toString(),
                                              node_id);
                } else if (chosen_action == up) {
                    emit MoveBlockRequested(node_id, -1);
                } else if (chosen_action == down) {
                    emit MoveBlockRequested(node_id, +1);
                } else if (chosen_action == del) {
                    emit DeleteBlockRequested(node_id);
                } else if (chosen_action == reflow) {
                    ReflowRow(card, node_id);
                }
            });
    header_layout->addWidget(more);

    card_layout->addWidget(header);
    card->setProperty("header", QVariant::fromValue(static_cast<QWidget*>(header)));

    // Default chrome: header transparent (visible on hover via stylesheet).
    card->setStyleSheet(QString(
        "QFrame#blockCard { border-left: 3px solid transparent;"
        "                    border-radius: 4px; }"));
    header->setVisible(true);
    handle->setVisible(false);
    type_label->setVisible(header_inline);
    more->setVisible(false);

    // Hover: show chrome (design #4).
    card->installEventFilter(this);
    return card;
}

void BlockEditor::AddEditorToCard(QWidget* card, QPlainTextEdit* edit) {
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    card_layout->addWidget(edit);
}

QPlainTextEdit* BlockEditor::NewEditor(QWidget* card, const QString& text,
                                       bool mono, int min_lines,
                                       bool single_line) {
    auto* edit = new BlockEdit(single_line, card);
    edit->SetInitialText(text);
    if (mono) {
        edit->setFont(theme::MonoFont(10));
    } else {
        edit->setFont(theme::UiFont(11));
    }
    edit->setStyleSheet(QString(
        "QPlainTextEdit { background: transparent; border: none; color: %1;"
        " padding: 0; selection-background-color: %2; }")
                            .arg(theme::kPrimaryText, theme::kAccentSoft));
    // Minimum height for placeholders (the abstract asks for three lines).
    const int min_h = edit->fontMetrics().height() * min_lines + 12;
    if (auto* block_edit = qobject_cast<BlockEdit*>(edit)) {
        block_edit->SetMinimumContentHeight(min_h);
    }
    edit->installEventFilter(this);
    AddEditorToCard(card, edit);
    return edit;
}

void BlockEditor::ReflowRow(QWidget* card, const QString& node_id) {
    if (!card) return;
    QPlainTextEdit* editor = card->findChild<QPlainTextEdit*>();
    if (!editor) return;
    Block* target = nullptr;
    for (auto& block : blocks_) {
        if (block.editor == editor) {
            target = &block;
            break;
        }
    }
    if (!target) return;
    const QString before = editor->toPlainText();
    const QString after = ToQ(pf::ReflowHardWrappedText(ToStd(before)));
    if (after == before) return;
    const int caret = editor->textCursor().position();
    auto* block_edit = qobject_cast<BlockEdit*>(editor);
    if (block_edit) block_edit->BeginProgrammaticEdit();
    editor->setPlainText(after);
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(qMin(caret, static_cast<int>(after.size())));
    editor->setTextCursor(cursor);
    if (block_edit) block_edit->EndProgrammaticEdit();
    // Commit at once: the point of the action is to persist the re-flow, not
    // to leave it waiting for a focus change.
    CommitBlock(*target);
    RevealNode(node_id);
}

void BlockEditor::CommitBlock(Block& block) {
    if (rebuilding_) return;
    // A Text row commits through its InlineEditor (rich content); every other
    // row is a plain BlockEdit.
    if (block.inline_editor) {
        const InlineContent content = block.inline_editor->Content();
        if (content == block.committed_content) {
            block.inline_editor->MarkClean();
            return;
        }
        block.committed_content = content;
        block.committed_text = ToQ(pf::InlineToPlainText(content));
        block.inline_editor->MarkClean();
        emit ParagraphContentEdited(block.node_id, content);
        emit RowCommitted();
        return;
    }
    if (!block.editor) return;
    auto* edit = qobject_cast<BlockEdit*>(block.editor);
    QString text = block.editor->toPlainText();
    // Prose may still carry the hard line breaks of a paste; softening them is
    // output-neutral (LaTeX treats a single break as a space) and lets the
    // paragraph re-flow to the block width.
    if (IsProseRole(block.commit_role)) {
        text = ToQ(pf::ReflowHardWrappedText(ToStd(text)));
    }
    // Nothing changed since the document last saw this row: no edit, so no
    // documentChanged -> no rebuild. This is what stops restyling or simply
    // focusing a row from cycling into a full editor rebuild.
    if (text == block.committed_text) {
        if (edit) edit->MarkClean();
        emit RowCommitted();
        return;
    }
    block.committed_text = text;
    if (edit) edit->MarkClean();
    const QString& role = block.commit_role;
    if (role == "title") emit TitleEdited(text);
    else if (role == "authors") emit AuthorsEdited(text);
    else if (role == "affiliations") emit AffiliationsEdited(text);
    else if (role == "abstract") emit AbstractEdited(text);
    else if (role == "keywords") emit KeywordsEdited(text);
    else if (role == "paragraph") emit ParagraphEdited(block.node_id, text);
    else if (role == "equation") {
        emit EquationEdited(block.node_id, text, block.equation_numbered,
                            block.equation_label);
    }
    else if (role == "section") emit SectionRenamed(block.node_id, text);
    else if (role == "subsection") emit SubsectionRenamed(block.node_id, text);
    else if (role == "subsubsection") emit SubsubsectionRenamed(block.node_id, text);
    else if (role == "caption") emit CaptionEdited(block.node_id, text);
    emit RowCommitted();
}

bool BlockEditor::eventFilter(QObject* watched, QEvent* event) {
    // Hover chrome for block cards.
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        auto* card = qobject_cast<QFrame*>(watched);
        if (card && card->objectName() == "blockCard") {
            bool hover = event->type() == QEvent::Enter;
            auto* header = card->property("header").value<QWidget*>();
            if (header) {
                for (QObject* child : header->children()) {
                    if (auto* grip = qobject_cast<DragHandle*>(child)) {
                        grip->setVisible(hover);
                    }
                    if (auto* button = qobject_cast<QToolButton*>(child)) {
                        button->setVisible(
                            hover &&
                            !card->property("row_node").toString().isEmpty());
                    }
                }
            }
        }
        return false;
    }

    // Key/hover wiring from editors.
    auto* edit = qobject_cast<BlockEdit*>(watched);
    if (edit && event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_F2) {
            OpenSlashMenu(edit);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BlockEditor::BuildAuthorBindingPanel(QWidget* card,
                                          const FrontMatter& front) {
    if (card == nullptr) return;
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    if (card_layout == nullptr) return;

    auto* panel = new QWidget(card);
    panel->setObjectName("authorBindingPanel");
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(2, 0, 2, 2);
    panel_layout->setSpacing(1);

    auto* caption = new QLabel("Institution links", panel);
    caption->setStyleSheet(QString("color: %1; font-size: 8pt;"
                                   " font-weight: 700; letter-spacing: 0.5px;")
                               .arg(theme::kDisabledText));
    panel_layout->addWidget(caption);

    if (front.authors.empty()) {
        auto* hint = new QLabel("Add an author above to link institutions.",
                                panel);
        hint->setStyleSheet(
            QString("color: %1; font-size: 9pt;").arg(theme::kDisabledText));
        panel_layout->addWidget(hint);
    } else if (front.affiliations.empty()) {
        auto* hint = new QLabel(
            "Add an institution above, then link each author to it here.",
            panel);
        hint->setStyleSheet(
            QString("color: %1; font-size: 9pt;").arg(theme::kDisabledText));
        panel_layout->addWidget(hint);
    } else {
        for (size_t i = 0; i < front.authors.size(); ++i) {
            const auto& author = front.authors[i];
            auto* row = new QWidget(panel);
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            row_layout->setSpacing(6);

            auto* name = new QLabel(ToQ(author.name), row);
            name->setMinimumWidth(120);
            name->setStyleSheet(QString("color: %1; font-size: 9pt;")
                                    .arg(theme::kPrimaryText));
            row_layout->addWidget(name);

            // The button shows the current numbers, so the binding is visible
            // without opening anything.
            QStringList current;
            for (size_t a = 0; a < front.affiliations.size(); ++a) {
                for (const auto& link : author.affiliations) {
                    if (link == front.affiliations[a].id) {
                        static const char* kSupers[] = {"\u00b9", "\u00b2",
                                                        "\u00b3", "\u2074",
                                                        "\u2075", "\u2076",
                                                        "\u2077", "\u2078",
                                                        "\u2079"};
                        current << (a < 9 ? QString::fromUtf8(kSupers[a])
                                          : QString::number(a + 1));
                    }
                }
            }
            auto* pick = new QToolButton(row);
            pick->setObjectName("authorAffiliationPicker");
            pick->setText(current.isEmpty() ? QStringLiteral("link…")
                                            : current.join(QLatin1Char(' ')));
            pick->setToolTip("Which institutions does this author belong to?");
            pick->setCursor(Qt::PointingHandCursor);
            pick->setFocusPolicy(Qt::NoFocus);
            pick->setStyleSheet(
                QString("QToolButton { color: %1; background: %2;"
                        " border: 1px solid %3; border-radius: 9px;"
                        " padding: 1px 8px; font-size: 9pt; }"
                        "QToolButton:hover { border-color: %4; }")
                    .arg(theme::kSecondaryText, theme::kSidePanel,
                         theme::kDivider, theme::kAccent));
            row_layout->addWidget(pick);
            row_layout->addStretch(1);
            panel_layout->addWidget(row);

            const int author_index = static_cast<int>(i);
            connect(pick, &QToolButton::clicked, this,
                    [this, pick, front, author_index]() {
                        QMenu menu(this);
                        for (size_t a = 0; a < front.affiliations.size(); ++a) {
                            const auto& affiliation = front.affiliations[a];
                            const bool linked = std::any_of(
                                front.authors[author_index].affiliations.begin(),
                                front.authors[author_index].affiliations.end(),
                                [&](const AffiliationId& id) {
                                    return id == affiliation.id;
                                });
                            QAction* action = menu.addAction(
                                QStringLiteral("%1  %2")
                                    .arg(a + 1)
                                    .arg(ToQ(affiliation.name)));
                            action->setCheckable(true);
                            action->setChecked(linked);
                            const QString id = ToQ(affiliation.id.value());
                            connect(action, &QAction::triggered, this,
                                    [this, author_index, id](bool checked) {
                                        emit AuthorAffiliationToggled(
                                            author_index, id, checked);
                                    });
                        }
                        menu.exec(pick->mapToGlobal(QPoint(0, pick->height())));
                    });
        }
    }
    card_layout->addWidget(panel);
}

QWidget* BlockEditor::MakeGap(const QString& anchor) {
    auto* gap = new BlockGap(
        [this, anchor](QWidget* source) { ShowInsertMenu(anchor, source); },
        [this, anchor](const QString& node) {
            if (anchor.isEmpty()) {
                // The body is empty, so there is nothing to reorder into.
                return;
            }
            if (node == anchor) return;
            emit MoveBlockToRequested(node, anchor);
        },
        host_);
    gap->setProperty("gap_anchor", anchor);
    gap->setProperty("body_gap", anchor.isEmpty());
    return gap;
}

void BlockEditor::ShowInsertMenu(const QString& anchor, QWidget* source) {
    const bool body_empty = anchor.isEmpty();
    QMenu menu(this);
    if (body_empty) {
        QAction* hint =
            menu.addAction("The body is empty — start with a section");
        hint->setEnabled(false);
        menu.addSeparator();
    }
    const NodeKind container =
        ContainerKindFor(*container_document_, anchor);
    const int max_depth = max_heading_depth_ > 0 ? max_heading_depth_ : 3;
    const char* current_group = nullptr;
    for (const auto& entry : InsertOptionsFor(body_empty, container, max_depth)) {
        if (current_group == nullptr ||
            std::strcmp(current_group, entry.group) != 0) {
            if (current_group != nullptr) menu.addSeparator();
            QAction* group = menu.addAction(QString::fromUtf8(entry.group));
            group->setEnabled(false);
            current_group = entry.group;
        }
        QAction* action = menu.addAction(QString::fromUtf8(entry.label));
        action->setData(QString::fromUtf8(entry.kind));
    }
    QAction* chosen = menu.exec(
        source->mapToGlobal(QPoint(source->width() / 2, source->height())));
    if (chosen == nullptr || !chosen->data().isValid()) return;
    emit InsertBlockRequested(chosen->data().toString(), anchor);
}

void BlockEditor::OpenSlashMenu(QPlainTextEdit* origin) {
    // Same rules as the gap menu: filtered by the container the row belongs
    // to and by the template's heading depth.
    QString row_node;
    for (const auto& block : blocks_) {
        if (block.editor == origin) {
            row_node = block.node_id;
            break;
        }
    }
    const NodeKind container = ContainerKindFor(*container_document_, row_node);
    const int max_depth = max_heading_depth_ > 0 ? max_heading_depth_ : 3;

    std::vector<PopupList::Item> items;
    const char* current_group = nullptr;
    for (const auto& entry : InsertOptionsFor(row_node.isEmpty(), container,
                                              max_depth)) {
        PopupList::Item item;
        item.label = QString::fromUtf8(entry.label);
        item.detail = "Insert";
        item.group = QString::fromUtf8(entry.group);
        item.payload = QString::fromUtf8(entry.kind);
        item.search = item.label.toLower();
        if (current_group == nullptr ||
            std::strcmp(current_group, entry.group) != 0) {
            current_group = entry.group;
        }
        items.push_back(std::move(item));
    }

    QString after;
    for (const auto& block : blocks_) {
        if (block.editor == origin) {
            after = block.node_id;
            break;
        }
    }
    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this, [this, after](QString payload) {
        emit InsertBlockRequested(payload, after);
    });
    QPoint anchor = origin->mapToGlobal(QPoint(60, origin->height() + 4));
    popup->popup(anchor, items);
}

void BlockEditor::OpenAtMenu(QPlainTextEdit* origin) {
    if (reference_items_.empty()) {
        reference_items_.push_back(
            {QStringLiteral("No references loaded — import a .bib file"),
             {}, {}, {}, {}});
    }
    QString paragraph;
    for (const auto& block : blocks_) {
        if (block.editor == origin) {
            paragraph = block.node_id;
            break;
        }
    }
    const QString editor_text = origin->toPlainText();
    const int cursor_position = origin->textCursor().position();
    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this,
            [this, paragraph, editor_text,
             cursor_position](QString payload) {
        // payload format: "cite:<key>" or "xref:<node>"
        QString token;
        if (payload.startsWith("cite:")) {
            token = "[cite:" + payload.mid(5) + "]";
        } else if (payload.startsWith("xref:")) {
            token = "[ref:" + payload.mid(5) + "]";
        }
        if (!token.isEmpty()) {
            QString updated = editor_text;
            updated.insert(qBound(0, cursor_position, updated.size()), token);
            emit ParagraphEdited(paragraph, updated);
        }
    });
    QPoint anchor = origin->mapToGlobal(QPoint(60, origin->height() + 4));
    popup->popup(anchor, reference_items_);
}

void BlockEditor::RebuildFromDocument(const Document& doc) {
    // The insert menus resolve anchors against this document until the next
    // rebuild; MainWindow keeps the session alive for the editor's lifetime,
    // and rebuilds always pass the current document.
    container_document_ = &doc;
    // Save focus, and the live text of a row the user is still editing. That
    // text wins over the document: a rebuild triggered from anywhere else
    // must never discard what is currently being typed.
    focus_node_.clear();
    focus_pos_ = 0;
    QString pending_key;
    QString pending_text;
    bool has_pending = false;
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        focus_node_ = edit->property("row_focus_key").toString();
        focus_pos_ = edit->textCursor().position();
        if (edit->IsDirty()) {
            pending_key = focus_node_;
            pending_text = edit->toPlainText();
            has_pending = true;
        }
    }

    rebuilding_ = true;
    auto* host_layout = qobject_cast<QVBoxLayout*>(host_->layout());
    // Clear every widget from the previous pass, keeping only the trailing
    // stretch. Rows and gaps are both created per rebuild, and a survivor
    // would keep stealing hover and clicks from the rows drawn over it.
    for (int i = host_layout->count() - 1; i >= 0; --i) {
        QLayoutItem* item = host_layout->itemAt(i);
        if (item == nullptr || item->widget() == nullptr) continue;
        host_layout->takeAt(i);
        QWidget* widget = item->widget();
        widget->hide();
        widget->deleteLater();
        delete item;
    }
    blocks_.clear();
    // A gap follows every body block (including the last), so the pointer
    // never has to travel to a toolbar to add the next one.
    auto append_gap = [&](const QString& anchor) {
        host_layout->insertWidget(host_layout->count() - 1, MakeGap(anchor));
    };
    auto add = [&](const QString& node_id, const QString& kind,
                   const QString& commit_role, const QString& text,
                   bool mono = false, bool header_inline = true,
                   int min_lines = 1, bool single_line = false) -> QWidget* {
        QWidget* card = MakeCard(node_id, kind, commit_role, header_inline);
        const QString focus_key =
            node_id.isEmpty() ? QStringLiteral("front:") + commit_role
                              : node_id;
        const bool restore_pending = has_pending && focus_key == pending_key;
        QString row_text = restore_pending ? pending_text : text;
        if (IsProseRole(commit_role) && !restore_pending) {
            // Never re-flow while the user is typing in the row.
            row_text = ToQ(pf::ReflowHardWrappedText(ToStd(row_text)));
        }
        QPlainTextEdit* edit =
            NewEditor(card, row_text, mono, min_lines, single_line);
        if (auto* block_edit = qobject_cast<BlockEdit*>(edit)) {
            if (restore_pending) {
                // The model text is the baseline so the user's text is still
                // committed on focus-out.
                block_edit->BeginProgrammaticEdit();
                block_edit->EndProgrammaticEdit();
                block_edit->MarkClean();
            }
        }
        if (auto* block_edit = qobject_cast<BlockEdit*>(edit)) {
            block_edit->setReflowOnPaste(IsProseRole(commit_role));
            block_edit->MarkClean();
        }
        edit->setProperty("row_node", node_id);
        edit->setProperty("row_focus_key", focus_key);
        edit->setProperty("commands_enabled", !node_id.isEmpty());
        edit->setProperty("references_enabled", commit_role == "paragraph");
        auto center_text = [edit]() {
            auto* block_edit = qobject_cast<BlockEdit*>(edit);
            if (block_edit) block_edit->BeginProgrammaticEdit();
            QTextCursor cursor = edit->textCursor();
            cursor.select(QTextCursor::Document);
            QTextBlockFormat format;
            format.setAlignment(Qt::AlignHCenter);
            cursor.mergeBlockFormat(format);
            cursor.clearSelection();
            edit->setTextCursor(cursor);
            if (block_edit) block_edit->EndProgrammaticEdit();
        };
        if (kind == "Title") {
            edit->setFont(theme::UiFont(22, true));
            edit->setPlaceholderText("Untitled paper");
            center_text();
        } else if (kind == "Authors") {
            edit->setPlaceholderText("Author names — separate with commas");
            center_text();
        } else if (kind == "Institution") {
            edit->setPlaceholderText("Affiliations — separate with semicolons");
            center_text();
        } else if (kind == "Abstract") {
            edit->setPlaceholderText("Write the abstract…");
        } else if (kind == "Keywords") {
            edit->setPlaceholderText("Keywords — separate with commas");
        } else if (kind == "Section Title") {
            edit->setFont(theme::UiFont(17, true));
            edit->setPlaceholderText("Section title");
        } else if (kind == "Subsection Title") {
            edit->setFont(theme::UiFont(14, true));
            edit->setPlaceholderText("Subsection title");
        } else if (kind == "Subsubsection Title") {
            edit->setFont(theme::UiFont(12, true));
            edit->setPlaceholderText("Subsubsection title");
        } else if (kind == "Text") {
            edit->setPlaceholderText(
                "Write text…  @ inserts a reference, Ctrl+Enter adds a block");
            new InlineTokenHighlighter(edit->document());
        } else if (kind == "Equation") {
            edit->setPlaceholderText("LaTeX equation");
        }
        qobject_cast<BlockEdit*>(edit)->Resize();
        Block block;
        block.node_id = node_id;
        block.kind = kind;
        block.card = card;
        block.editor = edit;
        block.commit_role = commit_role;
        block.committed_text = text;  // what the document holds for this row
        if (restore_pending) {
            // Keep it dirty: the user's uncommitted text is still pending.
            if (auto* restored = qobject_cast<BlockEdit*>(edit)) {
                restored->MarkDirty();
            }
        }
        // Commit on Enter (commit signal) - the BlockEdit emits
        // CommitRequested on Enter AND focusOut; we want focus-out commit,
        // Enter just moves on. Wire it:
        BlockEdit* block_edit = qobject_cast<BlockEdit*>(edit);
        connect(block_edit, &BlockEdit::CommitRequested, this, [this, edit]() {
            for (auto& b : blocks_) {
                if (b.editor == edit) {
                    CommitBlock(b);
                    break;
                }
            }
        });
        connect(block_edit, &BlockEdit::TriggerSlash, this,
                [this, edit]() { OpenSlashMenu(edit); });
        connect(block_edit, &BlockEdit::TriggerAt, this,
                [this, edit]() { OpenAtMenu(edit); });
        connect(block_edit, &BlockEdit::NewBlockAfter, this, [this, node_id]() {
            emit InsertBlockRequested("paragraph", node_id);
        });
        host_layout->insertWidget(host_layout->count() - 1, card);
        if (!node_id.isEmpty()) append_gap(node_id);
        blocks_.push_back(std::move(block));
        return card;
    };

    // Front matter.
    const auto& fm = doc.front_matter();
    add("", "Title", "title", ToQ(pf::InlineToPlainText(fm.title)), false,
        true, 1, true);
    QString authors;
    for (size_t i = 0; i < fm.authors.size(); ++i) {
        if (i) authors += " · ";
        authors += ToQ(fm.authors[i].name);
        // affiliation superscripts
        for (const auto& aff_id : fm.authors[i].affiliations) {
            for (size_t a = 0; a < fm.affiliations.size(); ++a) {
                if (fm.affiliations[a].id == aff_id) {
                    const char* supers[] = {"¹", "²", "³", "⁴", "⁵"};
                    authors += supers[a < 5 ? a : 4];
                    break;
                }
            }
        }
    }
    {
        QWidget* authors_card =
            add("", "Authors", "authors", authors, false, true, 1, true);
        BuildAuthorBindingPanel(authors_card, fm);
    }
    QString affiliations;
    for (size_t i = 0; i < fm.affiliations.size(); ++i) {
        if (i) affiliations += "; ";
        const char* supers[] = {"¹ ", "² ", "³ ", "⁴ ", "⁵ "};
        affiliations += supers[i < 5 ? i : 4] + ToQ(fm.affiliations[i].name);
    }
    add("", "Institution", "affiliations", affiliations, false, true, 1,
        true);
    add("", "Abstract", "abstract",
        fm.abstract_text ? ToQ(pf::InlineToPlainText(*fm.abstract_text))
                         : QString(),
        false, true, 3);
    QString keywords;
    for (size_t i = 0; i < fm.keywords.size(); ++i) {
        if (i) keywords += ", ";
        keywords += ToQ(fm.keywords[i]);
    }
    add("", "Keywords", "keywords", keywords, false, true, 1, true);

    // Body.
    if (doc.body().sections.empty()) {
        // Nothing to hover between yet: offer to start the body.
        append_gap(QString());
    }
    for (const auto& section : doc.body().sections) {
        add(ToQ(section.id.value()), "Section Title", "section",
            ToQ(pf::InlineToPlainText(section.title)), false, true, 1, true);
        for (const auto& block : section.blocks) {
            if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                QWidget* text_card = MakeTextCard(ToQ(para->id.value()),
                                                  para->content);
                host_layout->insertWidget(host_layout->count() - 1, text_card);
                append_gap(ToQ(para->id.value()));
            } else if (const auto* eq = std::get_if<pf::EquationBlock>(&block)) {
                QWidget* equation_card =
                    MakeEquationCard(ToQ(eq->id.value()), *eq);
                host_layout->insertWidget(host_layout->count() - 1,
                                          equation_card);
                append_gap(ToQ(eq->id.value()));
            } else if (const auto* figure = std::get_if<pf::Figure>(&block)) {
                QWidget* card = MakeCard(ToQ(figure->id.value()), "Figure",
                                         "caption", true);
                auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
                auto* image = new QLabel(card);
                image->setAlignment(Qt::AlignCenter);
                image->setMinimumHeight(140);
                image->setStyleSheet(QString(
                    "background: %1; border: 1px solid %2; border-radius: 6px;"
                    "color: %3;")
                    .arg(theme::kSidePanel, theme::kDivider,
                         theme::kSecondaryText));
                QString path = asset_path_resolver_
                                   ? asset_path_resolver_(figure->asset_id)
                                   : QString();
                QPixmap pixmap(path);
                if (!pixmap.isNull()) {
                    image->setPixmap(pixmap.scaled(
                        680, 260, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
                } else {
                    image->setText("Image preview unavailable");
                }
                card_layout->addWidget(image);
                auto* caption = NewEditor(
                    card,
                    ToQ(pf::ReflowHardWrappedText(
                        pf::InlineToPlainText(figure->caption))),
                    false, 1);
                if (auto* caption_edit = qobject_cast<BlockEdit*>(caption)) {
                    caption_edit->setReflowOnPaste(true);
                }
                caption->setPlaceholderText("Figure caption");
                caption->setProperty("row_node", ToQ(figure->id.value()));
                caption->setProperty("row_focus_key", ToQ(figure->id.value()));
                caption->setProperty("commands_enabled", true);
                Block gui_block;
                gui_block.node_id = ToQ(figure->id.value());
                gui_block.kind = QStringLiteral("Figure");
                gui_block.card = card;
                gui_block.editor = caption;
                gui_block.commit_role = QStringLiteral("caption");
                gui_block.committed_text =
                    ToQ(pf::InlineToPlainText(figure->caption));
                auto* block_edit = qobject_cast<BlockEdit*>(caption);
                connect(block_edit, &BlockEdit::CommitRequested, this,
                        [this, caption]() {
                            for (auto& candidate : blocks_) {
                                if (candidate.editor == caption) {
                                    CommitBlock(candidate);
                                    break;
                                }
                            }
                        });
                host_layout->insertWidget(host_layout->count() - 1, card);
                append_gap(ToQ(figure->id.value()));
                blocks_.push_back(std::move(gui_block));
            } else if (const auto* table = std::get_if<pf::Table>(&block)) {
                QWidget* card = MakeCard(ToQ(table->id.value()), "Table",
                                         "caption", true);
                auto* grid = new QTableWidget(
                    static_cast<int>(table->RowCount()),
                    static_cast<int>(table->ColumnCount()), card);
                grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
                grid->horizontalHeader()->setSectionResizeMode(
                    QHeaderView::Stretch);
                grid->verticalHeader()->setVisible(false);
                grid->setMaximumHeight(qMin(260, 34 + 32 * grid->rowCount()));
                for (int row = 0; row < grid->rowCount(); ++row) {
                    for (int column = 0; column < grid->columnCount(); ++column) {
                        grid->setItem(row, column, new QTableWidgetItem(ToQ(
                            pf::InlineToPlainText(table->cells[row][column].content))));
                    }
                }
                qobject_cast<QVBoxLayout*>(card->layout())->addWidget(grid);
                auto* caption = NewEditor(
                    card,
                    ToQ(pf::ReflowHardWrappedText(
                        pf::InlineToPlainText(table->caption))),
                    false, 1);
                if (auto* caption_edit = qobject_cast<BlockEdit*>(caption)) {
                    caption_edit->setReflowOnPaste(true);
                }
                caption->setPlaceholderText("Table caption");
                caption->setProperty("row_node", ToQ(table->id.value()));
                caption->setProperty("row_focus_key", ToQ(table->id.value()));
                caption->setProperty("commands_enabled", true);
                Block gui_block;
                gui_block.node_id = ToQ(table->id.value());
                gui_block.kind = QStringLiteral("Table");
                gui_block.card = card;
                gui_block.editor = caption;
                gui_block.commit_role = QStringLiteral("caption");
                gui_block.committed_text =
                    ToQ(pf::InlineToPlainText(table->caption));
                auto* block_edit = qobject_cast<BlockEdit*>(caption);
                connect(block_edit, &BlockEdit::CommitRequested, this,
                        [this, caption]() {
                            for (auto& candidate : blocks_) {
                                if (candidate.editor == caption) {
                                    CommitBlock(candidate);
                                    break;
                                }
                            }
                        });
                host_layout->insertWidget(host_layout->count() - 1, card);
                append_gap(ToQ(table->id.value()));
                blocks_.push_back(std::move(gui_block));
            }
        }
        for (const auto& sub : section.subsections) {
            add(ToQ(sub.id.value()), "Subsection Title", "subsection",
                ToQ(pf::InlineToPlainText(sub.title)), false, true, 1, true);
            for (const auto& block : sub.blocks) {
                if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                    QWidget* text_card = MakeTextCard(ToQ(para->id.value()),
                                                      para->content);
                    host_layout->insertWidget(host_layout->count() - 1,
                                              text_card);
                    append_gap(ToQ(para->id.value()));
                } else if (const auto* eq = std::get_if<pf::EquationBlock>(&block)) {
                    QWidget* equation_card =
                        MakeEquationCard(ToQ(eq->id.value()), *eq);
                    host_layout->insertWidget(host_layout->count() - 1,
                                              equation_card);
                    append_gap(ToQ(eq->id.value()));
                }
            }
            for (const auto& subsub : sub.subsubsections) {
                add(ToQ(subsub.id.value()), "Subsubsection Title", "subsubsection",
                    ToQ(pf::InlineToPlainText(subsub.title)), false, true, 1,
                    true);
                for (const auto& block : subsub.blocks) {
                    if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                        QWidget* text_card =
                            MakeTextCard(ToQ(para->id.value()), para->content);
                        host_layout->insertWidget(host_layout->count() - 1,
                                                  text_card);
                        append_gap(ToQ(para->id.value()));
                    } else if (const auto* eq =
                                   std::get_if<pf::EquationBlock>(&block)) {
                        QWidget* equation_card =
                            MakeEquationCard(ToQ(eq->id.value()), *eq);
                        host_layout->insertWidget(host_layout->count() - 1,
                                                  equation_card);
                        append_gap(ToQ(eq->id.value()));
                    }
                }
            }
        }
    }

    ApplyHints();

    // Restore focus.
    if (!focus_node_.isEmpty()) {
        for (const auto& block : blocks_) {
            if (block.editor &&
                block.editor->property("row_focus_key").toString() ==
                    focus_node_) {
                auto cursor = block.editor->textCursor();
                cursor.setPosition(qMin(focus_pos_,
                                        block.editor->toPlainText().length()));
                block.editor->setTextCursor(cursor);
                block.editor->setFocus();
                break;
            }
        }
    }
    rebuilding_ = false;
}

void BlockEditor::SetRequiredHints(const RequiredHints& hints) {
    hints_ = hints;
    ApplyHints();
}

void BlockEditor::ApplyHints() {
    for (auto& block : blocks_) {
        bool missing = false;
        if (block.kind == "Title") {
            missing = hints_.title && block.editor &&
                      block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Authors") {
            missing = hints_.authors && block.editor &&
                      block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Institution") {
            missing = hints_.affiliations && block.editor &&
                      block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Abstract") {
            missing = hints_.abstract_text && block.editor &&
                      block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Keywords") {
            missing = hints_.keywords && block.editor &&
                      block.editor->toPlainText().trimmed().isEmpty();
        }
        if (!block.card) continue;
        if (missing) {
            block.card->setStyleSheet(QString(
                "QFrame#blockCard { border-left: 3px solid %1;"
                " background: %2; border-radius: 4px; }")
                .arg(theme::kError, theme::kErrorSoft));
        } else {
            block.card->setStyleSheet(QString(
                "QFrame#blockCard { border-left: 3px solid transparent;"
                " border-radius: 4px; }"
                "QFrame#blockCard:hover { background: #FAFBFC; }"));
        }
    }
}

void BlockEditor::SetReferenceItems(std::vector<PopupList::Item> items) {
    reference_items_ = std::move(items);
}

void BlockEditor::SetAssetPathResolver(
    std::function<QString(const AssetId&)> resolver) {
    asset_path_resolver_ = std::move(resolver);
}

bool BlockEditor::HasUncommittedFocus() const {
    // Text rows (InlineEditor) carry uncommitted input just like the plain
    // rows; a rebuild while either is dirty would destroy what is being
    // typed - and, for a rich row, the format state too.
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        return rich->IsDirty();
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        return edit->IsDirty();
    }
    return false;
}

void BlockEditor::RefreshHints() { ApplyHints(); }

void BlockEditor::CommitFocused() {
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        for (auto& block : blocks_) {
            if (block.inline_editor == rich) {
                CommitBlock(block);
                return;
            }
        }
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        for (auto& block : blocks_) {
            if (block.editor == edit) {
                CommitBlock(block);
                return;
            }
        }
    }
}

std::optional<QString> BlockEditor::FocusedNodeId() const {
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        QString node = rich->property("row_node").toString();
        if (!node.isEmpty()) return node;
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        QString node = edit->property("row_node").toString();
        if (!node.isEmpty()) return node;
    }
    return std::nullopt;
}

void BlockEditor::RevealNode(const QString& node_id) {
    for (const auto& block : blocks_) {
        // Front-matter rows (Abstract, Title) are addressed by their row key,
        // real blocks by their node id.
        const bool matches = block.node_id == node_id ||
                             (block.editor &&
                              block.editor->property("row_focus_key")
                                      .toString() == node_id);
        if (matches && block.card) {
            scroll_->ensureWidgetVisible(block.card, 0, 80);
            block.card->setStyleSheet(QString(
                "QFrame#blockCard { border-left: 3px solid %1; border-radius: 4px; }")
                .arg(theme::kAccent));
            QTimer::singleShot(1200, this, [this]() { ApplyHints(); });
            return;
        }
    }
}

}  // namespace pf::gui
