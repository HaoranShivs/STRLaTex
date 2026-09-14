#include "app/WelcomePage.h"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "app/Theme.h"

namespace pf::gui {

WelcomePage::WelcomePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(60, 80, 60, 40);
    layout->setSpacing(10);

    auto* title = new QLabel("PaperForge", this);
    title->setStyleSheet(QString(
        "font-size: 26pt; font-weight: 700; color: %1; background: transparent;")
        .arg(theme::kPrimaryText));
    layout->addWidget(title);

    auto* tagline = new QLabel(
        "Create academic papers without writing LaTeX.", this);
    tagline->setStyleSheet(QString(
        "font-size: 12pt; color: %1; background: transparent; margin-bottom: 24px;")
        .arg(theme::kSecondaryText));
    layout->addWidget(tagline);

    auto* new_btn = new QPushButton("New Project", this);
    new_btn->setObjectName("primary");
    new_btn->setFixedWidth(220);
    connect(new_btn, &QPushButton::clicked, this,
            &WelcomePage::NewProjectRequested);
    layout->addWidget(new_btn);

    auto* open_btn = new QPushButton("Open Project…", this);
    open_btn->setFixedWidth(220);
    connect(open_btn, &QPushButton::clicked, this,
            &WelcomePage::OpenProjectRequested);
    layout->addWidget(open_btn);

    layout->addSpacing(30);
    auto* recent_title = new QLabel("RECENT", this);
    recent_title->setObjectName("panelTitle");
    layout->addWidget(recent_title);

    recent_list_ = new QListWidget(this);
    recent_list_->setFixedWidth(360);
    recent_list_->setStyleSheet(QString(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { padding: 8px; border-radius: 6px; }"
        "QListWidget::item:hover { background: %1; }")
        .arg(theme::kAccentSoft));
    connect(recent_list_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
                emit RecentActivated(item->data(Qt::UserRole).toString());
            });
    layout->addWidget(recent_list_);
    layout->addStretch(1);
}

void WelcomePage::AddRecent(const QString& name, const QString& path) {
    auto* item = new QListWidgetItem(recent_list_);
    item->setText(name);
    item->setData(Qt::UserRole, path);
    item->setToolTip(path);
}

}  // namespace pf::gui
