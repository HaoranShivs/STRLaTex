#include "app/ProblemsPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include "app/Theme.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string& s) { return QString::fromStdString(s); }
}  // namespace

ProblemsPanel::ProblemsPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("background: %1;").arg(theme::kSidePanel));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(12, 6, 12, 0);
    auto* title = new QLabel("PROBLEMS", header);
    title->setObjectName("panelTitle");
    header_layout->addWidget(title);
    count_label_ = new QLabel(header);
    count_label_->setStyleSheet(QString("color: %1; font-size: 8pt;")
                                    .arg(theme::kSecondaryText));
    header_layout->addWidget(count_label_);
    header_layout->addStretch(1);
    // Severity filter chips.
    for (const char* f : {"All", "Errors", "Warnings"}) {
        auto* chip = new QPushButton(f, header);
        chip->setCheckable(true);
        chip->setFixedHeight(20);
        chip->setStyleSheet(QString(
            "QPushButton { border: none; color: %1; font-size: 8pt; padding: 1px 8px;"
            "              border-radius: 9px; background: transparent; }"
            "QPushButton:checked { background: %2; color: %3; }")
            .arg(theme::kSecondaryText, theme::kAccentSoft, theme::kPrimaryText));
        connect(chip, &QPushButton::toggled, this, [this, f](bool on) {
            if (on) {
                filter_ = QString(f).toLower();
                if (filter_ == "errors") filter_ = "error";
                if (filter_ == "warnings") filter_ = "warning";
                Refilter();
            }
        });
        if (QString(f) == "All") {
            chip->blockSignals(true);
            chip->setChecked(true);
            chip->blockSignals(false);
        }
        header_layout->addWidget(chip);
    }
    layout->addWidget(header);

    tabs_ = new QTabBar(this);
    tabs_->addTab("Problems");
    tabs_->addTab("Build Log");
    tabs_->setStyleSheet(QString(
        "QTabBar::tab { background: transparent; color: %1; padding: 4px 12px;"
        "               border: none; border-bottom: 2px solid transparent; font-size: 8pt; }"
        "QTabBar::tab:selected { color: %2; border-bottom: 2px solid %3; }")
        .arg(theme::kSecondaryText, theme::kPrimaryText, theme::kAccent));
    layout->addWidget(tabs_);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(BuildProblemsTab());
    stack_->addWidget(BuildLogTab());
    layout->addWidget(stack_, 1);

    connect(tabs_, &QTabBar::currentChanged, stack_,
            &QStackedWidget::setCurrentIndex);
}

QWidget* ProblemsPanel::BuildProblemsTab() {
    list_ = new QListWidget(this);
    list_->setStyleSheet(QString(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { padding: 6px 10px; border-radius: 4px; }"
        "QListWidget::item:hover { background: %1; }"
        "QListWidget::item:selected { background: %1; }")
        .arg(theme::kAccentSoft));
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                int index = item->data(Qt::UserRole).toInt();
                if (index >= 0 &&
                    index < static_cast<int>(diagnostics_.size())) {
                    emit DiagnosticActivated(diagnostics_[index]);
                }
            });
    return list_;
}

QWidget* ProblemsPanel::BuildLogTab() {
    log_view_ = new QPlainTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setFont(theme::MonoFont(8));
    return log_view_;
}

void ProblemsPanel::SetDiagnostics(
    const std::vector<Diagnostic>& diagnostics) {
    diagnostics_ = diagnostics;
    int errors = 0;
    int warnings = 0;
    for (const auto& d : diagnostics) {
        if (d.severity == DiagnosticSeverity::Error) ++errors;
        if (d.severity == DiagnosticSeverity::Warning) ++warnings;
    }
    count_label_->setText(QString("%1 errors · %2 warnings")
                              .arg(errors)
                              .arg(warnings));
    Refilter();
}

void ProblemsPanel::Refilter() {
    list_->clear();
    for (size_t i = 0; i < diagnostics_.size(); ++i) {
        const auto& d = diagnostics_[i];
        QString severity = d.severity == DiagnosticSeverity::Error
                               ? QStringLiteral("error")
                               : QStringLiteral("warning");
        if (filter_ != "all" && filter_ != severity) continue;
        QString icon = severity == "error" ? "✕" : "⚠";
        QString text = QString("%1  %2\n%3")
                           .arg(icon, ToQ(d.message),
                                severity == "error" ? "ERROR" : "WARNING");
        auto* item = new QListWidgetItem(list_);
        item->setText(text);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setForeground(QColor(severity == "error" ? theme::kError
                                                       : theme::kWarning));
    }
}

void ProblemsPanel::SetBuildLog(const QString& log) {
    if (log_view_) log_view_->setPlainText(log);
}

void ProblemsPanel::SetStale(bool stale, int behind_by) {
    if (stale) {
        count_label_->setText(QString("preview %1 revision(s) behind")
                                  .arg(behind_by));
    }
}

}  // namespace pf::gui
