#include "app/ProblemsPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStringList>
#include <QTabBar>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>

#include "app/Theme.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string& s) { return QString::fromStdString(s); }

// Severity ranks (plan §21): Error first, then Warning, then Info; ties keep
// arrival order, which is document order for validator output and line order
// for compiler messages.
int SeverityRank(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error: return 0;
        case DiagnosticSeverity::Warning: return 1;
        case DiagnosticSeverity::Info: return 2;
    }
    return 3;
}

// Icons carry the meaning; colour is only an aid (plan §23).
const char* SeverityIcon(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error: return "\xE2\x9C\x95";  // ✕
        case DiagnosticSeverity::Warning: return "\xE2\x9A\xA0";  // ⚠
        case DiagnosticSeverity::Info: return "\xE2\x93\xB9";   // ⓘ
    }
    return "?";
}

const char* SeverityWord(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error: return "ERROR";
        case DiagnosticSeverity::Warning: return "WARNING";
        case DiagnosticSeverity::Info: return "INFO";
    }
    return "";
}

QString EventIcon(DiagnosticSeverity severity) {
    return QString::fromUtf8(SeverityIcon(severity));
}
}  // namespace

ProblemsPanel::ProblemsPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("background: %1;").arg(theme::kSidePanel));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(12, 6, 12, 0);
    auto* title = new QLabel("DIAGNOSTICS", header);
    title->setObjectName("panelTitle");
    header_layout->addWidget(title);
    count_label_ = new QLabel(header);
    count_label_->setObjectName("problemCountLabel");
    count_label_->setStyleSheet(QString("color: %1; font-size: 8pt;")
                                    .arg(theme::kSecondaryText));
    header_layout->addWidget(count_label_);
    header_layout->addStretch(1);
    // Severity filter chips (plan §41: only Errors/Warnings in v1).
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
    tabs_->setObjectName("problemTabs");
    tabs_->addTab("Problems");
    tabs_->addTab("Build Log");
    tabs_->setExpanding(false);
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
    // Start in the "never built" empty state (plan §42) so the panel is
    // never a blank hole before the first build.
    Refilter();
}

QWidget* ProblemsPanel::BuildProblemsTab() {
    list_ = new QListWidget(this);
    // Object names are the stable seam the widget tests read through.
    list_->setObjectName("problemsList");
    list_->setStyleSheet(QString(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { padding: 6px 10px; border-radius: 4px; }"
        "QListWidget::item:hover { background: %1; }"
        "QListWidget::item:selected { background: %1; }")
        .arg(theme::kAccentSoft));
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                int index = item->data(Qt::UserRole).toInt();
                if (index < 0) return;  // empty-state row, not a problem
                if (index >= 0 &&
                    index < static_cast<int>(diagnostics_.size())) {
                    emit DiagnosticActivated(diagnostics_[index]);
                }
            });
    return list_;
}

QWidget* ProblemsPanel::BuildLogTab() {
    auto* page = new QWidget(this);
    auto* page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(8, 4, 8, 8);
    page_layout->setSpacing(4);

    // Log actions (plan §8).
    auto* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->addStretch(1);
    auto add_tool_button = [&](const QString& text, auto slot) {
        auto* button = new QPushButton(text, page);
        button->setFixedHeight(20);
        button->setStyleSheet(QString(
            "QPushButton { border: none; color: %1; font-size: 8pt; padding: 1px 8px; }"
            "QPushButton:hover { color: %2; }")
            .arg(theme::kSecondaryText, theme::kPrimaryText));
        toolbar->addWidget(button);
        connect(button, &QPushButton::clicked, page, slot);
        return button;
    };
    add_tool_button("Copy", [this]() {
        QApplication::clipboard()->setText(log_view_->toPlainText());
    });
    add_tool_button("Clear View", [this]() {
        // Clears the display only; the model and the BuildSession stay
        // untouched (plan §8).
        log_view_->clear();
    });
    auto_scroll_button_ = add_tool_button("Auto Scroll: On", [this]() {
        const bool on = !auto_scroll_button_->isChecked();
        auto_scroll_button_->setChecked(on);
        auto_scroll_button_->setText(on ? "Auto Scroll: On"
                                        : "Auto Scroll: Off");
    });
    auto_scroll_button_->setCheckable(true);
    auto_scroll_button_->setChecked(true);
    page_layout->addLayout(toolbar);

    log_view_ = new QPlainTextEdit(page);
    log_view_->setObjectName("buildLogView");
    log_view_->setReadOnly(true);
    log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_view_->setFont(theme::MonoFont(8));
    // Memory cap (plan §43): very old lines fall out instead of growing the
    // widget without bound when a compiler dumps megabytes of output.
    log_view_->setMaximumBlockCount(20000);
    page_layout->addWidget(log_view_, 1);
    return page;
}

// ---------------- Problems ----------------

void ProblemsPanel::SetDiagnostics(
    const std::vector<Diagnostic>& diagnostics) {
    state_ = ProblemsState::Done;
    stale_ = false;
    diagnostics_ = diagnostics;
    // Default sort by severity, stable so equal severities keep their
    // document/line order (plan §21).
    std::stable_sort(diagnostics_.begin(), diagnostics_.end(),
                     [](const Diagnostic& a, const Diagnostic& b) {
                         return SeverityRank(a.severity) <
                                SeverityRank(b.severity);
                     });
    int errors = 0;
    int warnings = 0;
    for (const auto& d : diagnostics_) {
        if (d.severity == DiagnosticSeverity::Error) ++errors;
        else if (d.severity == DiagnosticSeverity::Warning) ++warnings;
    }
    QString counts;
    if (!diagnostics_.empty()) {
        counts = QString("%1 Errors \u00b7 %2 Warnings").arg(errors).arg(warnings);
        if (warnings == 0 && errors > 0) {
            counts = QString("%1 Errors").arg(errors);
        } else if (errors == 0) {
            counts = QString("%1 Warnings").arg(warnings);
        }
    }
    count_label_->setText(counts);
    UpdateHeader();
    Refilter();
}

void ProblemsPanel::UpdateHeader() {
    // Tab title with live count and Outdated marker (plan §24/§49).
    QString tab = "Problems";
    const int total = static_cast<int>(diagnostics_.size());
    if (state_ == ProblemsState::Done && total > 0) {
        tab += QString(" (%1)").arg(total);
        if (stale_) tab += " \u2014 Outdated";
    } else if (state_ == ProblemsState::Done && total == 0 && stale_) {
        tab += " \u2014 Outdated";
    }
    tabs_->setTabText(0, tab);
}

QString ProblemsPanel::LocationText(const pf::Diagnostic& d) const {
    // "Figure \u00b7 main.tex:41" style (plan §22/§28): the block kind when
    // known, plus the generated-file position when it survived mapping.
    QStringList parts;
    if (d.location.has_block_location()) {
        if (!d.location.label.empty()) parts << ToQ(d.location.label);
    }
    if (d.location.has_file_location()) {
        parts << QString("%1:%2").arg(ToQ(d.location.file),
                                      QString::number(*d.location.line));
    }
    if (d.location.kind == DiagnosticLocationKind::CitationKey &&
        !d.location.citation_key.empty()) {
        parts << QString("[%1]").arg(ToQ(d.location.citation_key));
    }
    if (parts.isEmpty()) return ToQ(ToString(d.source));
    return parts.join(" \u00b7 ");
}

void ProblemsPanel::Refilter() {
    list_->clear();

    // Empty states (plan §42).
    if (state_ != ProblemsState::Done) {
        const QString text = state_ == ProblemsState::Building
                                 ? QStringLiteral("Building\u2026")
                                 : QStringLiteral(
                                       "No build diagnostics available.");
        auto* item = new QListWidgetItem(list_);
        item->setText(text);
        item->setData(Qt::UserRole, -1);
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(QColor(theme::kSecondaryText));
        return;
    }
    if (diagnostics_.empty()) {
        auto* item = new QListWidgetItem(list_);
        item->setText(QString::fromUtf8("\xE2\x9C\x94") +
                      " No problems detected.");
        item->setData(Qt::UserRole, -1);
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(QColor(theme::kOk));
        return;
    }

    for (size_t i = 0; i < diagnostics_.size(); ++i) {
        const auto& d = diagnostics_[i];
        const char* severity = d.severity == DiagnosticSeverity::Error
                                   ? "error"
                                   : d.severity == DiagnosticSeverity::Warning
                                         ? "warning"
                                         : "info";
        if (filter_ != "all" && filter_ != severity) continue;
        QString text =
            QString("%1  %2\n    %3 \u00b7 %4")
                .arg(EventIcon(d.severity), ToQ(d.message),
                     ToQ(SeverityWord(d.severity)), LocationText(d));
        auto* item = new QListWidgetItem(list_);
        item->setText(text);
        item->setData(Qt::UserRole, static_cast<int>(i));
        const char* color = d.severity == DiagnosticSeverity::Error
                                ? theme::kError
                                : d.severity == DiagnosticSeverity::Warning
                                      ? theme::kWarning
                                      : theme::kSecondaryText;
        item->setForeground(QColor(color));
    }
}

void ProblemsPanel::SetStale(bool stale, int behind_by) {
    (void)behind_by;
    // Status text only (plan §49): the diagnostics themselves are untouched;
    // the tab title grows the Outdated marker until the next build replaces
    // the whole set.
    stale_ = stale;
    UpdateHeader();
}

void ProblemsPanel::ShowProblemsTab() {
    tabs_->setCurrentIndex(0);
    stack_->setCurrentIndex(0);
}

// ---------------- Build Log ----------------

void ProblemsPanel::ShowBuildLog() {
    tabs_->setCurrentIndex(1);
    stack_->setCurrentIndex(1);
}

void ProblemsPanel::AppendEvent(const pf::BuildEvent& event) {
    // A new build owns the log from its first event on (plan §30/§35): the
    // previous build's display is cleared; only the current Build is kept.
    if (event.build_id != log_build_id_) {
        log_view_->clear();
        events_.clear();
        last_stream_.clear();
        log_needs_newline_ = false;
        log_build_id_ = event.build_id;
    }
    if (event.type == BuildEventType::BuildStarted) {
        // A retried snapshot reuses no id, but re-clear so the log starts
        // empty even when the same build emits two start events (§8).
        log_view_->clear();
        events_.clear();
        last_stream_.clear();
        log_needs_newline_ = false;
        state_ = ProblemsState::Building;
        diagnostics_.clear();
        count_label_->setText(QString());
        UpdateHeader();
        Refilter();
    }

    QString text;
    if (event.type == BuildEventType::StdOut ||
        event.type == BuildEventType::StdErr) {
        // Compiler output keeps its raw bytes (plan §7); stream headers make
        // the two channels readable without timestamps.
        const bool is_err = event.type == BuildEventType::StdErr;
        const std::string stream = is_err ? "err" : "out";
        if (last_stream_ != stream) {
            if (log_needs_newline_) text += "\n";
            else if (!last_stream_.empty()) text += "\n";
            text += is_err ? "[stderr]" : "[stdout]";
            text += "\n";
            last_stream_ = stream;
        }
        text += ToQ(event.message);
    } else {
        // Close an unterminated raw line before a lifecycle event so the
        // timestamp always starts a fresh line.
        if (log_needs_newline_) text += "\n";
        text += QString("%1 %2")
                    .arg(ToQ(FormatBuildTimestamp(event.timestamp_ms).c_str()),
                         ToQ(event.message));
    }
    events_.push_back(event);

    const bool was_at_bottom = !log_view_->verticalScrollBar() ||
        log_view_->verticalScrollBar()->value() >=
            log_view_->verticalScrollBar()->maximum();
    // Incremental append at the document end (no toPlainText() rescans):
    // raw compiler chunks may split a line mid-way, so the newline bookkeeping
    // lives in log_needs_newline_ instead of the widget text.
    QTextCursor cursor(log_view_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    log_needs_newline_ = !text.endsWith("\n");
    if (auto_scroll_button_->isChecked() || was_at_bottom) {
        log_view_->verticalScrollBar()->setValue(
            log_view_->verticalScrollBar()->maximum());
    }
}

}  // namespace pf::gui
