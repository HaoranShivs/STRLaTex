#include "app/InlineContentEditorDialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/InlineEditor.h"
#include "app/Theme.h"

namespace pf::gui {

InlineContentEditorDialog::InlineContentEditorDialog(
    const InlineContent& initial, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("inlineContentEditorDialog"));
    setWindowTitle(QStringLiteral("Edit content"));
    setModal(true);
    resize(600, 300);
    auto* layout = new QVBoxLayout(this);
    toolbar_ = new QToolBar(this);
    toolbar_->setMovable(false);
    layout->addWidget(toolbar_);

    auto* area = new QScrollArea(this);
    area->setWidgetResizable(true);
    auto* host = new QWidget(area);
    auto* content_layout = new QVBoxLayout(host);
    content_layout->setContentsMargins(8, 8, 8, 8);
    editor_ = new InlineEditor(host);
    editor_->SetBodyTypography(theme::EditorFont(theme::BlockVisualRole::Body),
                               theme::typography::kBodyLineHeight);
    editor_->SetContent(initial);
    content_layout->addWidget(editor_);
    content_layout->addStretch(1);
    area->setWidget(host);
    area->setFocusPolicy(Qt::ClickFocus);
    area->setFocusProxy(editor_);
    layout->addWidget(area, 1);

    auto* bold = new QAction(QStringLiteral("Bold"), editor_);
    bold->setIconText(QStringLiteral("B"));
    auto* italic = new QAction(QStringLiteral("Italic"), editor_);
    italic->setIconText(QStringLiteral("I"));
    auto* math = new QAction(QStringLiteral("Inline Math"), editor_);
    connect(bold, &QAction::triggered, editor_, &InlineEditor::ToggleBold);
    connect(italic, &QAction::triggered, editor_, &InlineEditor::ToggleItalic);
    connect(math, &QAction::triggered, editor_, &InlineEditor::BeginInlineMath);
    SetAllowedActions({bold, italic, math});

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    editor_->setFocus(Qt::OtherFocusReason);
}

void InlineContentEditorDialog::SetCitationResolver(
    std::shared_ptr<const pf::CitationNumberResolver> resolver) {
    editor_->SetCitationNumbers(std::move(resolver));
}

void InlineContentEditorDialog::SetCrossReferenceLabels(
    std::map<QString, QString> labels) {
    editor_->SetCrossReferenceLabels(std::move(labels));
}

void InlineContentEditorDialog::SetAllowedActions(
    const QList<QAction*>& actions) {
    toolbar_->clear();
    toolbar_->addActions(actions);
    toolbar_->setFocusProxy(nullptr);
    toolbar_->setFocusPolicy(Qt::ClickFocus);
    toolbar_->setFocusProxy(editor_);
    for (auto* button : toolbar_->findChildren<QToolButton*>())
        button->setFocusPolicy(Qt::NoFocus);
    editor_->SetFormattingActions(actions);
}

void InlineContentEditorDialog::SetPlaceholder(const QString& text) {
    editor_->setPlaceholderText(text);
}

InlineContent InlineContentEditorDialog::content() const {
    return editor_->Content();
}

} // namespace pf::gui
