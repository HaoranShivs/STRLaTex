#include "app/BlockEditor.h"

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/Theme.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
using theme::kAccent;

QString ToQ(const std::string& s) { return QString::fromStdString(s); }
std::string ToStd(const QString& s) { return s.toStdString(); }

// Editor that sizes itself to its content and exposes key events for / and @.
class BlockEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit BlockEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWordWrapMode(QTextOption::WordWrap);
        setFrameShape(QFrame::NoFrame);
        document()->setDocumentMargin(6);
        connect(this, &QPlainTextEdit::textChanged, this, &BlockEdit::Resize);
        Resize();
    }

    void Resize() {
        qreal doc_height =
            document()->documentLayout()->documentSize().height();
        setFixedHeight(qMax(qCeil(doc_height) + 10,
                            fontMetrics().height() + 12));
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
        if (event->text() == QStringLiteral("/")) {
            if (toPlainText().trimmed().isEmpty()) {
                emit TriggerSlash();
                return;
            }
        }
        if (event->text() == QStringLiteral("@")) {
            emit TriggerAt();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            emit NewBlockAfter();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
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
    auto* handle = new QLabel("⋮⋮", header);
    handle->setFixedWidth(18);
    handle->setStyleSheet(QString("color: %1; font-weight: 700;").arg(theme::kDisabledText));
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
    connect(more, &QToolButton::clicked, this, [this, more, node_id]() {
        QMenu menu(this);
        QAction* up = menu.addAction("Move Up");
        QAction* down = menu.addAction("Move Down");
        menu.addSeparator();
        QAction* del = menu.addAction("Delete");
        QAction* chosen_action = menu.exec(more->mapToGlobal(QPoint(0, 18)));
        if (chosen_action == nullptr) return;
        if (chosen_action == up) {
            emit MoveBlockRequested(node_id, -1);
        } else if (chosen_action == down) {
            emit MoveBlockRequested(node_id, +1);
        } else if (chosen_action == del) {
            emit DeleteBlockRequested(node_id);
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
                                       bool mono, int min_lines) {
    auto* edit = new BlockEdit(card);
    edit->setPlainText(text);
    if (mono) {
        edit->setFont(theme::MonoFont(10));
    } else {
        edit->setFont(theme::UiFont(11));
    }
    // Minimum height for placeholders.
    int min_h = edit->fontMetrics().height() * min_lines + 12;
    edit->setMinimumHeight(min_h);
    edit->installEventFilter(this);
    AddEditorToCard(card, edit);
    return edit;
}

void BlockEditor::CommitBlock(Block& block) {
    if (rebuilding_ || !block.editor) return;
    QString text = block.editor->toPlainText();
    const QString& role = block.commit_role;
    if (role == "title") emit TitleEdited(text);
    else if (role == "authors") emit AuthorsEdited(text);
    else if (role == "affiliations") emit AffiliationsEdited(text);
    else if (role == "abstract") emit AbstractEdited(text);
    else if (role == "keywords") emit KeywordsEdited(text);
    else if (role == "paragraph") emit ParagraphEdited(block.node_id, text);
    else if (role == "equation") emit EquationEdited(block.node_id, text);
    else if (role == "section") emit SectionRenamed(block.node_id, text);
    else if (role == "subsection") emit SubsectionRenamed(block.node_id, text);
    else if (role == "caption") emit CaptionEdited(block.node_id, text);
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
                    if (auto* label = qobject_cast<QLabel*>(child)) {
                        label->setVisible(hover || label->text() != QStringLiteral("⋮⋮"));
                        if (label->text() == QStringLiteral("⋮⋮")) {
                            label->setVisible(hover);
                        }
                    }
                    if (auto* button = qobject_cast<QToolButton*>(child)) {
                        button->setVisible(hover);
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

void BlockEditor::OpenSlashMenu(QPlainTextEdit* origin) {
    static const std::vector<std::pair<const char*, const char*>> commands = {
        {"Title", "title"},       {"Authors", "authors"},
        {"Institution", "affiliations"}, {"Abstract", "abstract"},
        {"Keywords", "keywords"}, {"Section", "section"},
        {"Subsection", "subsection"}, {"Paragraph", "paragraph"},
        {"Equation", "equation"}, {"Figure", "figure"},
        {"Table", "table"},
    };
    std::vector<PopupList::Item> items;
    for (const auto& pair : commands) {
        PopupList::Item item;
        item.label = pair.first;
        item.detail = "Block";
        item.group = (QString(pair.second) == "equation" ||
                      QString(pair.second) == "figure" ||
                      QString(pair.second) == "table")
                         ? "Academic"
                         : "Basic";
        item.payload = pair.second;
        item.search = QString(pair.first).toLower();
        items.push_back(std::move(item));
    }

    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this, [this, origin](QString payload) {
        QString after;
        // Find the block owning this editor; empty means insert at end.
        for (const auto& block : blocks_) {
            if (block.editor == origin) {
                after = block.node_id;
                break;
            }
        }
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
    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this, [this, origin](QString payload) {
        // payload format: "cite:<key>" or "xref:<node>"
        QString after;
        for (const auto& block : blocks_) {
            if (block.editor == origin) {
                after = block.node_id;
                break;
            }
        }
        if (payload.startsWith("cite:")) {
            emit InsertCitationRequested(after, payload.mid(5));
        } else if (payload.startsWith("xref:")) {
            emit InsertCrossRefRequested(after, payload.mid(5));
        }
    });
    QPoint anchor = origin->mapToGlobal(QPoint(60, origin->height() + 4));
    popup->popup(anchor, reference_items_);
}

void BlockEditor::RebuildFromDocument(const Document& doc) {
    // Save focus.
    focus_node_.clear();
    focus_pos_ = 0;
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        focus_node_ = edit->property("row_node").toString();
        focus_pos_ = edit->textCursor().position();
    }

    rebuilding_ = true;
    // Clear.
    for (const auto& block : blocks_) {
        if (block.card) block.card->deleteLater();
    }
    blocks_.clear();

    auto add = [&](const QString& node_id, const QString& kind,
                   const QString& commit_role, const QString& text,
                   bool mono = false, bool header_inline = true,
                   int min_lines = 1) {
        QWidget* card = MakeCard(node_id, kind, commit_role, header_inline);
        QPlainTextEdit* edit = NewEditor(card, text, mono, min_lines);
        edit->setProperty("row_node", node_id);
        Block block;
        block.node_id = node_id;
        block.kind = kind;
        block.card = card;
        block.editor = edit;
        block.commit_role = commit_role;
        // Commit on Enter (commit signal) - the BlockEdit emits
        // CommitRequested on Enter AND focusOut; we want focus-out commit,
        // Enter just moves on. Wire it:
        BlockEdit* block_edit = qobject_cast<BlockEdit*>(edit);
        connect(block_edit, &BlockEdit::CommitRequested, this, [this, node_id]() {
            for (auto& b : blocks_) {
                if (b.node_id == node_id && b.editor) {
                    CommitBlock(b);
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
        qobject_cast<QVBoxLayout*>(host_->layout())
            ->insertWidget(host_->layout()->count() - 1, card);
        blocks_.push_back(std::move(block));
    };

    // Front matter.
    const auto& fm = doc.front_matter();
    add("", "Title", "title", ToQ(pf::InlineToPlainText(fm.title)));
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
    add("", "Authors", "authors", authors);
    QString affiliations;
    for (size_t i = 0; i < fm.affiliations.size(); ++i) {
        if (i) affiliations += "; ";
        const char* supers[] = {"¹ ", "² ", "³ ", "⁴ ", "⁵ "};
        affiliations += supers[i < 5 ? i : 4] + ToQ(fm.affiliations[i].name);
    }
    add("", "Institution", "affiliations", affiliations);
    add("", "Abstract", "abstract",
        fm.abstract_text ? ToQ(pf::InlineToPlainText(*fm.abstract_text))
                         : QString(),
        false, true, 3);
    QString keywords;
    for (size_t i = 0; i < fm.keywords.size(); ++i) {
        if (i) keywords += ", ";
        keywords += ToQ(fm.keywords[i]);
    }
    add("", "Keywords", "keywords", keywords);

    // Body.
    for (const auto& section : doc.body().sections) {
        add(ToQ(section.id.value()), "Section", "section",
            ToQ(pf::InlineToPlainText(section.title)));
        for (const auto& block : section.blocks) {
            if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                add(ToQ(para->id.value()), "Paragraph", "paragraph",
                    ToQ(pf::InlineToPlainText(para->content)), false, true, 2);
            } else if (const auto* eq = std::get_if<pf::DisplayEquation>(&block)) {
                add(ToQ(eq->id.value()), "Equation", "equation",
                    ToQ(eq->math_source), true, true, 2);
            }
        }
        for (const auto& sub : section.subsections) {
            add(ToQ(sub.id.value()), "Subsection", "subsection",
                ToQ(pf::InlineToPlainText(sub.title)));
            for (const auto& block : sub.blocks) {
                if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                    add(ToQ(para->id.value()), "Paragraph", "paragraph",
                        ToQ(pf::InlineToPlainText(para->content)), false, true, 2);
                } else if (const auto* eq = std::get_if<pf::DisplayEquation>(&block)) {
                    add(ToQ(eq->id.value()), "Equation", "equation",
                        ToQ(eq->math_source), true, true, 2);
                }
            }
        }
    }

    ApplyHints();

    // Restore focus.
    if (!focus_node_.isEmpty()) {
        for (const auto& block : blocks_) {
            if (block.node_id == focus_node_ && block.editor) {
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

std::optional<QString> BlockEditor::FocusedNodeId() const {
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        QString node = edit->property("row_node").toString();
        if (!node.isEmpty()) return node;
    }
    return std::nullopt;
}

void BlockEditor::RevealNode(const QString& node_id) {
    for (const auto& block : blocks_) {
        if (block.node_id == node_id && block.card) {
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
