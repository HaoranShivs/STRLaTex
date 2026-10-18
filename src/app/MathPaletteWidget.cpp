#include "app/MathPaletteWidget.h"

#include <QGridLayout>
#include <QLayout>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/Theme.h"

namespace pf::gui {

namespace {

struct BuiltInsertion {
    QString text;
    int cursor = -1;
};

BuiltInsertion BuildInsertion(const QString& snippet, const QString& selection) {
    BuiltInsertion built;
    bool selection_pending = !selection.isEmpty();
    for (const QChar c : snippet) {
        if (c != QLatin1Char('$')) {
            built.text += c;
            continue;
        }
        if (selection_pending) {
            built.text += selection;
            selection_pending = false;
        } else if (built.cursor < 0) {
            // 记录第一个未被选区占用的空参数（位置 0 也合法）。
            built.cursor = built.text.size();
        }
    }
    if (built.cursor < 0)
        built.cursor = built.text.size();
    built.cursor = qBound(0, built.cursor, built.text.size());
    return built;
}

std::vector<pf::MathCategory> Categories() {
    return {pf::MathCategory::Structure, pf::MathCategory::Font,     pf::MathCategory::Greek,
            pf::MathCategory::Function,  pf::MathCategory::Operator, pf::MathCategory::Relation,
            pf::MathCategory::Arrow,     pf::MathCategory::SetLogic, pf::MathCategory::Misc};
}

} // namespace

MathInsertionResult BuildMathInsertion(const std::string& snippet, const QString& selection) {
    const BuiltInsertion built = BuildInsertion(QString::fromStdString(snippet), selection);
    return {built.text, built.cursor};
}

QString MakeMathInsertion(const std::string& snippet, const QString& selection) {
    return BuildMathInsertion(snippet, selection).text;
}

int InsertMathCommand(QPlainTextEdit* target, const pf::MathCommand& command) {
    if (!target || command.snippet.empty())
        return -1;
    QTextCursor cursor = target->textCursor();
    QString selection;
    if (cursor.hasSelection()) {
        selection = cursor.selectedText();
        selection.replace(QChar(0x2029), QLatin1Char(' '));
    }
    const int start = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
    const MathInsertionResult built = BuildMathInsertion(command.snippet, selection);
    if (cursor.hasSelection())
        cursor.removeSelectedText();
    cursor.insertText(built.text);
    QTextCursor placed(target->document());
    placed.setPosition(qBound(0, start + built.cursor, target->document()->characterCount() - 1));
    target->setTextCursor(placed);
    target->setFocus(Qt::OtherFocusReason);
    return start;
}

MathPaletteWidget::MathPaletteWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mathPalette"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    setFocusPolicy(Qt::NoFocus);
    tabs_ = new QTabBar(this);
    tabs_->setFocusPolicy(Qt::NoFocus);
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->setStyleSheet(QString("QTabBar::tab { color: %1; padding: 4px 8px; border: none; }"
                                 "QTabBar::tab:selected { color: %2; font-weight: 600; }")
                             .arg(theme::kSecondaryText, theme::kAccent));
    const auto categories = Categories();
    for (const auto category : categories)
        tabs_->addTab(QString::fromUtf8(pf::ToString(category)));
    outer->addWidget(tabs_);
    connect(tabs_, &QTabBar::currentChanged, this, [this, categories](int index) {
        if (rebuilding_ || index < 0 || static_cast<size_t>(index) >= categories.size())
            return;
        SetCategory(categories[static_cast<size_t>(index)]);
    });

    auto* grid_host = new QWidget(this);
    grid_ = new QGridLayout(grid_host);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(3);
    outer->addWidget(grid_host);
    RebuildButtons();
}

void MathPaletteWidget::SetTarget(QPlainTextEdit* target) {
    target_ = target;
}

void MathPaletteWidget::SetAvailablePackages(std::vector<std::string> packages) {
    packages_ = std::move(packages);
    RebuildButtons();
}

void MathPaletteWidget::SetCategory(pf::MathCategory category) {
    category_ = category;
    RebuildButtons();
}

bool MathPaletteWidget::CanInsert(const pf::MathCommand& command) const {
    if (packages_.empty())
        return true;
    for (const auto& package : command.packages) {
        if (std::find(packages_.begin(), packages_.end(), package) == packages_.end())
            return false;
    }
    return true;
}

void MathPaletteWidget::RebuildButtons() {
    if (!grid_)
        return;
    rebuilding_ = true;
    const auto categories = Categories();
    for (size_t i = 0; i < categories.size(); ++i) {
        if (categories[i] == category_ && tabs_->currentIndex() != static_cast<int>(i)) {
            const QSignalBlocker blocker(tabs_);
            tabs_->setCurrentIndex(static_cast<int>(i));
        }
    }
    while (QLayoutItem* item = grid_->takeAt(0)) {
        // 立即删除：deleteLater 会让 findChild(objectName) 在重建后仍可能
        // 命中旧按钮，且同一分类反复切换时积累不可见的控件。
        if (QWidget* widget = item->widget())
            delete widget;
        delete item;
    }
    const auto commands = pf::MathCommandCatalog::Instance().InCategory(category_);
    constexpr int kColumns = 7;
    for (size_t i = 0; i < commands.size(); ++i) {
        const pf::MathCommand& command = commands[i];
        auto* button = new QToolButton(this);
        button->setObjectName(QStringLiteral("math-command-") + QString::fromStdString(command.command));
        button->setText(QString::fromStdString(command.label));
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(24);
        QString tooltip =
            QStringLiteral("\\%1 — %2")
                .arg(QString::fromStdString(command.command), QString::fromStdString(command.description));
        const bool enabled = CanInsert(command);
        for (const auto& package : command.packages) {
            if (std::find(packages_.begin(), packages_.end(), package) == packages_.end())
                tooltip += QStringLiteral("\nRequires \\usepackage{%1}").arg(QString::fromStdString(package));
        }
        button->setToolTip(tooltip);
        button->setEnabled(enabled);
        button->setStyleSheet(QString("QToolButton { color: %1; border: 1px solid %2; border-radius: 4px;"
                                      " padding: 1px 5px; background: %3; }"
                                      "QToolButton:hover { background: %4; border-color: %5; }"
                                      "QToolButton:disabled { color: %6; background: transparent; }")
                                  .arg(theme::kPrimaryText, theme::kDivider, theme::kEditorBackground,
                                       theme::kAccentSoft, theme::kAccent, theme::kDisabledText));
        connect(button, &QToolButton::clicked, this, [this, command]() {
            if (target_)
                InsertMathCommand(target_.data(), command);
            emit InsertRequested(QString::fromStdString(command.command));
        });
        grid_->addWidget(button, static_cast<int>(i / kColumns), static_cast<int>(i % kColumns));
    }
    grid_->setColumnStretch(kColumns, 1);
    rebuilding_ = false;
}

} // namespace pf::gui
