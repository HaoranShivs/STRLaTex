#include "app/PopupList.h"

#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QScreen>

#include "app/Theme.h"

namespace pf::gui {

namespace {
constexpr int kPopupWidth = 340;
constexpr int kPopupMaxHeight = 320;
} // namespace

PopupList::PopupList(QWidget* parent) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::Box);
    setStyleSheet(QString("QFrame { background: white; border: 1px solid %1; border-radius: 8px; }"
                          "QLineEdit { border: none; border-bottom: 1px solid %1; border-radius: 0;"
                          "            padding: 8px; font-size: 10pt; background: transparent; }"
                          "QTreeWidget { border: none; background: transparent; outline: 0; }"
                          "QTreeWidget::item { padding: 4px 6px; border-radius: 4px; }"
                          "QTreeWidget::item:selected { background: %2; color: %3; }")
                      .arg(theme::kDivider, theme::kAccentSoft, theme::kPrimaryText));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    search_box_ = new QLineEdit(this);
    search_box_->setPlaceholderText("Type to filter…");
    search_box_->installEventFilter(this);
    layout->addWidget(search_box_);

    list_ = new QTreeWidget(this);
    list_->setColumnCount(2);
    list_->setHeaderHidden(true);
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setIndentation(8);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->installEventFilter(this);
    layout->addWidget(list_);

    connect(search_box_, &QLineEdit::textChanged, this, [this](const QString& text) { SetFilter(text); });
    connect(list_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem*) { ConfirmCurrent(); });
}

void PopupList::popup(const QPoint& global_pos, const std::vector<Item>& items, const QString& filter) {
    all_items_ = items;
    search_box_->blockSignals(true);
    search_box_->clear();
    search_box_->blockSignals(false);
    Refilter();
    adjustSize();
    resize(kPopupWidth, qMin(kPopupMaxHeight, sizeHint().height()));
    // 光标临近屏幕边缘时，把列表保持在当前屏幕可用区域内。
    QPoint position = global_pos;
    if (QScreen* screen = QGuiApplication::screenAt(global_pos)) {
        const QRect available = screen->availableGeometry();
        position.setX(qBound(available.left(), position.x(), qMax(available.left(), available.right() - width() + 1)));
        position.setY(qBound(available.top(), position.y(), qMax(available.top(), available.bottom() - height() + 1)));
    }
    move(position);
    show();
    raise();
    // search_box_->setFocus(Qt::PopupFocusReason);
    if (!filter.isEmpty()) {
        search_box_->setText(filter);
    }
}

void PopupList::Refilter() {
    // 用搜索框当前的内容重新执行过滤。
    SetFilter(search_box_->text());
}

void PopupList::SetFilter(const QString& text) {
    QString needle = text.trimmed().toLower();
    if (!needle.startsWith('/') && !needle.startsWith('@')) {
        // 调用方已经去掉了触发字符；此处保持原样
    } else {
        needle.remove(0, 1);
    }
    list_->clear();
    QString last_group;
    int visible = 0;
    for (const auto& item : all_items_) {
        QString hay = item.search.isEmpty() ? item.label.toLower() : item.search.toLower();
        if (!needle.isEmpty() && !hay.contains(needle))
            continue;
        if (!item.group.isEmpty() && item.group != last_group) {
            auto* header = new QTreeWidgetItem(list_);
            header->setText(0, item.group.toUpper());
            header->setFlags(Qt::NoItemFlags);
            QFont font = header->font(0);
            font.setPointSize(8);
            font.setBold(true);
            header->setFont(0, font);
            header->setForeground(0, QColor(theme::kSecondaryText));
            last_group = item.group;
        }
        auto* row = new QTreeWidgetItem(list_);
        row->setText(0, item.label);
        row->setText(1, item.detail);
        row->setData(0, Qt::UserRole, item.payload);
        row->setForeground(1, QColor(theme::kSecondaryText));
        ++visible;
    }
    if (visible == 0) {
        auto* empty = new QTreeWidgetItem(list_);
        empty->setText(0, "No matches");
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(0, QColor(theme::kDisabledText));
    } else {
        // 选中第一个可被选中的行。
        for (int i = 0; i < list_->topLevelItemCount(); ++i) {
            if (list_->topLevelItem(i)->flags() & Qt::ItemIsSelectable) {
                list_->setCurrentItem(list_->topLevelItem(i));
                break;
            }
        }
    }
    adjustSize();
    resize(kPopupWidth, qMin(kPopupMaxHeight, sizeHint().height()));
}

bool PopupList::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        switch (key_event->key()) {
        case Qt::Key_Down: {
            auto* current = list_->currentItem();
            int next = current ? list_->indexOfTopLevelItem(current) + 1 : 0;
            for (int i = next; i < list_->topLevelItemCount(); ++i) {
                if (list_->topLevelItem(i)->flags() & Qt::ItemIsSelectable) {
                    list_->setCurrentItem(list_->topLevelItem(i));
                    break;
                }
            }
            return true;
        }
        case Qt::Key_Up: {
            auto* current = list_->currentItem();
            int prev = current ? list_->indexOfTopLevelItem(current) - 1 : list_->topLevelItemCount() - 1;
            for (int i = prev; i >= 0; --i) {
                if (list_->topLevelItem(i)->flags() & Qt::ItemIsSelectable) {
                    list_->setCurrentItem(list_->topLevelItem(i));
                    break;
                }
            }
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
            ConfirmCurrent();
            return true;
        case Qt::Key_Escape:
            emit dismissed();
            close();
            return true;
        default:
            break;
        }
    }
    if (event->type() == QEvent::FocusOut) {
        auto* focus_event = static_cast<QFocusEvent*>(event);
        if (focus_event->reason() != Qt::PopupFocusReason) {
            close();
            emit dismissed();
        }
    }
    return QFrame::eventFilter(watched, event);
}

void PopupList::ConfirmCurrent() {
    auto* current = list_->currentItem();
    if (!current)
        return;
    QString payload = current->data(0, Qt::UserRole).toString();
    if (payload.isEmpty())
        return;
    close();
    emit chosen(payload);
}

} // namespace pf::gui
