#include "app/OutlinePanel.h"

#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/Theme.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string& s) { return QString::fromStdString(s); }
}  // namespace

OutlinePanel::OutlinePanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("background: %1;").arg(theme::kSidePanel));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tabs_ = new QTabBar(this);
    tabs_->setShape(QTabBar::TriangularNorth);
    tabs_->addTab("Document");
    tabs_->addTab("References");
    tabs_->setStyleSheet(QString(
        "QTabBar::tab { background: transparent; color: %1; padding: 8px 16px;"
        "               border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %2; border-bottom: 2px solid %3; }")
        .arg(theme::kSecondaryText, theme::kPrimaryText, theme::kAccent));
    layout->addWidget(tabs_);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(BuildDocumentTab());
    stack_->addWidget(BuildReferencesTab());
    layout->addWidget(stack_, 1);

    connect(tabs_, &QTabBar::currentChanged, this,
            &OutlinePanel::OnTabChanged);
}

QWidget* OutlinePanel::BuildDocumentTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 4, 8);

    auto* title = new QLabel("DOCUMENT", page);
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    outline_ = new QTreeWidget(page);
    outline_->setHeaderHidden(true);
    outline_->setRootIsDecorated(false);
    outline_->setIndentation(16);
    outline_->setStyleSheet(QString(
        "QTreeWidget { background: transparent; border: none; }"
        "QTreeWidget::item { padding: 3px 4px; border-radius: 4px; }"
        "QTreeWidget::item:hover { background: %1; }"
        "QTreeWidget::item:selected { background: %1; color: %2; }")
        .arg(theme::kAccentSoft, theme::kPrimaryText));
    connect(outline_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        QString node = item->data(0, Qt::UserRole).toString();
        if (!node.isEmpty()) emit NodeActivated(node);
    });
    layout->addWidget(outline_, 1);
    return page;
}

QWidget* OutlinePanel::BuildReferencesTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    ref_search_ = new QLineEdit(page);
    ref_search_->setPlaceholderText("Search references…");
    layout->addWidget(ref_search_);

    ref_count_ = new QLabel("0 references", page);
    ref_count_->setStyleSheet(QString("color: %1; font-size: 8pt;")
                                  .arg(theme::kSecondaryText));
    layout->addWidget(ref_count_);

    ref_list_ = new QListWidget(page);
    ref_list_->setStyleSheet(QString(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { padding: 6px; border-radius: 4px; }"
        "QListWidget::item:hover { background: %1; }"
        "QListWidget::item:selected { background: %1; }")
        .arg(theme::kAccentSoft));
    connect(ref_list_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        QString key = item->data(Qt::UserRole).toString();
        if (!key.isEmpty()) emit CitationChosen(key);
    });
    layout->addWidget(ref_list_, 1);

    connect(ref_search_, &QLineEdit::textChanged, this,
            [this](const QString&) { ApplyReferenceFilter(); });
    return page;
}

void OutlinePanel::ApplyReferenceFilter() {
    if (!ref_search_ || !ref_list_) return;
    const QString needle = ref_search_->text().toLower();
    for (int i = 0; i < ref_list_->count(); ++i) {
        auto* item = ref_list_->item(i);
        item->setHidden(!needle.isEmpty() &&
                        !item->text().toLower().contains(needle));
    }
}

void OutlinePanel::RebuildFromDocument(
    const Document& doc, const std::vector<BibEntry>& references) {
    // Outline: front matter that readers navigate to, then the structure
    // (design #17).
    outline_->clear();
    const auto& front = doc.front_matter();
    if (front.abstract_text && !pf::InlineIsBlank(*front.abstract_text)) {
        auto* item = new QTreeWidgetItem(outline_);
        item->setText(0, QStringLiteral("Abstract"));
        // Front matter has no block node, so it is addressed by the editor
        // row key, which RevealNode understands.
        item->setData(0, Qt::UserRole, QStringLiteral("front:abstract"));
        QFont font = item->font(0);
        font.setItalic(true);
        item->setFont(0, font);
        item->setForeground(0, QColor(theme::kSecondaryText));
        item->setToolTip(0, ToQ(pf::InlineToPlainText(*front.abstract_text)));
    }
    for (const auto& section : doc.body().sections) {
        QString title = ToQ(pf::InlineToPlainText(section.title));
        if (title.isEmpty()) title = QStringLiteral("Untitled Section");
        auto* item = new QTreeWidgetItem(outline_);
        item->setText(0, title);
        item->setData(0, Qt::UserRole, ToQ(section.id.value()));
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        for (const auto& sub : section.subsections) {
            QString sub_title = ToQ(pf::InlineToPlainText(sub.title));
            if (sub_title.isEmpty()) sub_title = QStringLiteral("Untitled");
            auto* sub_item = new QTreeWidgetItem(item);
            sub_item->setText(0, sub_title);
            sub_item->setData(0, Qt::UserRole, ToQ(sub.id.value()));
        }
    }

    // References.
    references_ = references;
    ref_list_->clear();
    for (const auto& entry : references) {
        QString authors = entry.authors.empty()
                              ? QString()
                              : ToQ(entry.authors.front()) +
                                    (entry.authors.size() > 1
                                         ? QStringLiteral(" et al.")
                                         : QString());
        QString line = authors.isEmpty()
                           ? ToQ(entry.title)
                           : authors + QStringLiteral(" · ") + ToQ(entry.year);
        auto* item = new QListWidgetItem(ref_list_);
        item->setText(line);
        item->setData(Qt::UserRole, ToQ(entry.key));
        item->setToolTip(ToQ(entry.title));
    }
    ref_count_->setText(QString("%1 references").arg(references.size()));
    // Keep the user's search term in force over the freshly built list.
    ApplyReferenceFilter();
}

void OutlinePanel::OnTabChanged(int index) { stack_->setCurrentIndex(index); }

}  // namespace pf::gui
