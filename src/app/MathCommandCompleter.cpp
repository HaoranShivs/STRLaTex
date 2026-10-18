#include "app/MathCommandCompleter.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QStringListModel>
#include <QTextCursor>

#include <algorithm>
#include <cctype>

#include "app/MathPaletteWidget.h"
#include "math/MathCommandCatalog.h"

namespace pf::gui {

MathCommandCompleter::MathCommandCompleter(QObject* parent) : QObject(parent) {
    model_ = new QStringListModel(this);
    completer_ = new QCompleter(model_, this);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setFilterMode(Qt::MatchStartsWith);
    completer_->setMaxVisibleItems(12);
}

MathCommandCompleter::~MathCommandCompleter() {
    if (target_)
        target_->removeEventFilter(this);
}

void MathCommandCompleter::Attach(QPlainTextEdit* target) {
    if (target_) {
        target_->removeEventFilter(this);
        disconnect(target_, nullptr, this, nullptr);
    }
    target_ = target;
    if (!target_)
        return;
    completer_->setWidget(target_);
    target_->installEventFilter(this);
    connect(target_, &QPlainTextEdit::textChanged, this, [this]() { RefreshCompletions(); });
    connect(target_, &QPlainTextEdit::cursorPositionChanged, this, [this]() { RefreshCompletions(); });
    connect(completer_, QOverload<const QString&>::of(&QCompleter::activated), this,
            [this](const QString& completion) { AcceptCompletion(completion); });
    RefreshCompletions();
}

void MathCommandCompleter::SetAvailablePackages(std::vector<std::string> packages) {
    packages_ = std::move(packages);
    RefreshCompletions();
}

bool MathCommandCompleter::IsCompletionVisibleForTest() const {
    return completer_ && completer_->popup() && completer_->popup()->isVisible();
}

QWidget* MathCommandCompleter::PopupForTest() const {
    return completer_ ? completer_->popup() : nullptr;
}

QString MathCommandCompleter::CurrentCompletionForTest() const {
    return completer_ ? completer_->currentCompletion() : QString();
}

bool MathCommandCompleter::AcceptCurrentForTest() {
    if (!IsCompletionVisibleForTest())
        return false;
    const QString current = CurrentCompletionForTest();
    if (current.isEmpty())
        return false;
    AcceptCompletion(current);
    return true;
}

QString MathCommandCompleter::CommandFromCompletion(const QString& completion) const {
    const QString trimmed = completion.trimmed();
    if (!trimmed.startsWith(QLatin1Char('\\')))
        return {};
    int end = 1;
    while (end < trimmed.size() && trimmed.at(end).isLetter())
        ++end;
    return trimmed.mid(1, end - 1);
}

void MathCommandCompleter::RefreshCompletions() {
    if (!target_ || !target_->hasFocus() || !completer_) {
        if (completer_ && completer_->popup())
            completer_->popup()->hide();
        return;
    }
    const QTextCursor cursor = target_->textCursor();
    if (cursor.hasSelection()) {
        if (completer_->popup())
            completer_->popup()->hide();
        return;
    }
    const QString text = target_->toPlainText();
    const int position = cursor.position();
    int start = position - 1;
    while (start >= 0 && text.at(start).isLetter())
        --start;
    if (start < 0 || text.at(start) != QLatin1Char('\\')) {
        if (completer_->popup())
            completer_->popup()->hide();
        return;
    }
    const QString prefix = text.mid(start, position - start);
    if (prefix.size() > 24) {
        if (completer_->popup())
            completer_->popup()->hide();
        return;
    }

    const std::string command_prefix = prefix.mid(1).toStdString();
    QStringList displays;
    const auto matches = pf::MathCommandCatalog::Instance().Search(command_prefix);
    for (const auto& command : matches) {
        if (!command.completable)
            continue;
        const std::string lowered = [&] {
            std::string value = command.command;
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }();
        if (lowered.rfind(command_prefix, 0) != 0)
            continue; // 仅按命令前缀补全，不用说明文本冒充前缀匹配
        QString display = QStringLiteral("\\") + QString::fromStdString(command.command);
        if (!command.description.empty())
            display += QStringLiteral("  ·  ") + QString::fromStdString(command.description);
        if (!packages_.empty()) {
            for (const auto& package : command.packages) {
                if (std::find(packages_.begin(), packages_.end(), package) == packages_.end()) {
                    display += QStringLiteral("  ⚠ needs ") + QString::fromStdString(package);
                    break;
                }
            }
        }
        displays << display;
    }
    if (displays.isEmpty()) {
        if (completer_->popup())
            completer_->popup()->hide();
        return;
    }
    model_->setStringList(displays);
    completer_->setCompletionPrefix(prefix);
    // QCompleter::complete(rect) 会直接把 rect 的宽度当作 popup 宽度；若传入
    // 光标矩形（宽 1px），补全弹层就会退化成光标下方的一条 1px 长线。
    QRect anchor = target_->cursorRect();
    anchor.setWidth(qMax(340, target_->viewport()->width() / 2));
    completer_->complete(anchor);
}

void MathCommandCompleter::InsertCommandAtCursor(const std::string& command) {
    const pf::MathCommand* entry = pf::MathCommandCatalog::Instance().Find(command);
    if (!target_ || !entry)
        return;
    QTextCursor cursor = target_->textCursor();
    const int end = cursor.position();
    const QString text = target_->toPlainText();
    int start = end - 1;
    while (start >= 0 && text.at(start).isLetter())
        --start;
    if (start >= 0 && text.at(start) == QLatin1Char('\\')) {
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    const MathInsertionResult built = BuildMathInsertion(entry->snippet, QString());
    cursor.insertText(built.text);
    QTextCursor placed(target_->document());
    placed.setPosition(
        qBound(0, cursor.position() - (built.text.size() - built.cursor), target_->document()->characterCount() - 1));
    target_->setTextCursor(placed);
    target_->setFocus(Qt::OtherFocusReason);
    emit CompletionAccepted(QString::fromStdString(command));
}

void MathCommandCompleter::AcceptCompletion(const QString& completion) {
    if (!target_)
        return;
    const QString command = CommandFromCompletion(completion);
    if (command.isEmpty())
        return;
    if (completer_->popup())
        completer_->popup()->hide();
    InsertCommandAtCursor(command.toStdString());
}

bool MathCommandCompleter::eventFilter(QObject* watched, QEvent* event) {
    if (watched != target_.data() || event->type() != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);
    auto* key = static_cast<QKeyEvent*>(event);
    const bool visible = completer_ && completer_->popup() && completer_->popup()->isVisible();
    if (visible) {
        switch (key->key()) {
        case Qt::Key_Escape:
            completer_->popup()->hide();
            return true;
        case Qt::Key_Up:
        case Qt::Key_Down:
            QApplication::sendEvent(completer_->popup(), event);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            AcceptCompletion(completer_->currentCompletion());
            return true;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace pf::gui
