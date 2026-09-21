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

// 严重级别排序（方案 §21）：Error 在前，其次是 Warning，最后是 Info；同级
// 保持到达顺序——对校验器输出而言即文档顺序，对编译器消息而言即行顺序。
int SeverityRank(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error: return 0;
        case DiagnosticSeverity::Warning: return 1;
        case DiagnosticSeverity::Info: return 2;
    }
    return 3;
}

// 图标承载含义；颜色仅作辅助（方案 §23）。
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
    // 严重级别筛选 chip（方案 §41：v1 只提供 Errors/Warnings）。
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
    // 以「从未构建」的空状态启动（方案 §42），这样面板在首次 build
    // 之前不会是一块空白。
    Refilter();
}

QWidget* ProblemsPanel::BuildProblemsTab() {
    list_ = new QListWidget(this);
    // 对象名是控件测试读取的稳定接缝。
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
                if (index < 0) return;  // 空状态行，并非问题项
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

    // 日志操作（方案 §8）。
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
        // 只清空显示内容；模型与 BuildSession 保持不变（方案 §8）。
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
    // 内存上限（方案 §43）：当编译器倾泻出数 MB 输出时，很旧的行会被挤出，
    // 而不是让控件无限增长。
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
    // 默认按严重级别排序，且使用稳定排序，使相同严重级别保持原有的
    // 文档/行顺序（方案 §21）。
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
    // 带实时计数和 Outdated 标记的标签页标题（方案 §24/§49）。
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
    // 「Figure · main.tex:41」这样的风格（方案 §22/§28）：已知时给出块
    // 类型，映射后仍保留时给出生成文件中的位置。
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

    // 空状态（方案 §42）。
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
    // 仅状态文本（方案 §49）：Diagnostic 本身不受影响；在下一次 build
    // 整体替换之前，标签页标题会一直带着 Outdated 标记。
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
    // 新的 build 从第一个事件起就接管日志（方案 §30/§35）：上一个 build
    // 的显示被清空，只保留当前 Build。
    if (event.build_id != log_build_id_) {
        log_view_->clear();
        events_.clear();
        last_stream_.clear();
        log_needs_newline_ = false;
        log_build_id_ = event.build_id;
    }
    if (event.type == BuildEventType::BuildStarted) {
        // 重试的 snapshot 不复用任何 id，但仍要再次清空，使同一个 build
        // 发出两次 start 事件时日志也从空开始（§8）。
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
        // 编译器输出保留其原始字节（方案 §7）；流头部让两个通道无需时间戳
        // 也能区分。
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
        // 在生命周期事件之前结束未换行的原始行，使时间戳总是从新的一行
        // 开始。
        if (log_needs_newline_) text += "\n";
        text += QString("%1 %2")
                    .arg(ToQ(FormatBuildTimestamp(event.timestamp_ms).c_str()),
                         ToQ(event.message));
    }
    events_.push_back(event);

    const bool was_at_bottom = !log_view_->verticalScrollBar() ||
        log_view_->verticalScrollBar()->value() >=
            log_view_->verticalScrollBar()->maximum();
    // 在文档末尾增量追加（无需用 toPlainText() 重新扫描）：原始编译器数据块
    // 可能把一行从中间截断，因此换行状态记在 log_needs_newline_ 中，而不是
    // 依据控件文本推断。
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
