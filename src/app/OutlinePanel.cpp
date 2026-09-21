#include "app/OutlinePanel.h"

#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include "app/Theme.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string& s) { return QString::fromStdString(s); }

// 返回节点 key 匹配的树项，没有则返回空。key 存储在 UserRole 中。
QTreeWidgetItem* FindOutlineItem(QTreeWidget* tree, const QString& key) {
    if (key.isEmpty()) return nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == key) return *it;
    }
    return nullptr;
}
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
    // 大纲：先是读者可导航到的 front matter，然后是正文结构（设计 #17）。
    outline_->clear();
    const auto& front = doc.front_matter();
    if (front.abstract_text && !pf::InlineIsBlank(*front.abstract_text)) {
        auto* item = new QTreeWidgetItem(outline_);
        item->setText(0, QStringLiteral("Abstract"));
        // front matter 没有区块节点，因此用编辑器行 key 来寻址，
        // RevealNode 能识别这种 key。
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
            for (const auto& subsub : sub.subsubsections) {
                QString subsub_title = ToQ(pf::InlineToPlainText(subsub.title));
                if (subsub_title.isEmpty()) subsub_title = QStringLiteral("Untitled");
                auto* subsub_item = new QTreeWidgetItem(sub_item);
                subsub_item->setText(0, subsub_title);
                subsub_item->setData(0, Qt::UserRole, ToQ(subsub.id.value()));
                QFont subsub_font = subsub_item->font(0);
                subsub_font.setItalic(true);
                subsub_item->setFont(0, subsub_font);
            }
        }
    }

    // References 部分。
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
    // 在新构建出的列表上继续沿用用户的搜索词。
    ApplyReferenceFilter();

    // 重新高亮光标所在行：重建替换了所有项，因此在此恢复
    // 跟踪高亮（UI 方案 §10）。不滚动——用户可能在树的其他位置
    // 阅读，刷新时强行拉动视口还不如不刷新。
    if (!selected_key_.isEmpty()) {
        if (QTreeWidgetItem* keep = FindOutlineItem(outline_, selected_key_)) {
            outline_->setCurrentItem(keep);
        }
    }
}

void OutlinePanel::OnTabChanged(int index) { stack_->setCurrentIndex(index); }

void OutlinePanel::SelectNode(const QString& outline_key) {
    selected_key_ = outline_key;
    if (!outline_) return;
    if (outline_key.isEmpty()) {
        outline_->setCurrentItem(nullptr);
        return;
    }
    if (QTreeWidgetItem* item = FindOutlineItem(outline_, outline_key)) {
        outline_->setCurrentItem(item);
        outline_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    } else {
        // 节点已被删除：不保留任何高亮。
        outline_->setCurrentItem(nullptr);
    }
}

}  // namespace pf::gui
