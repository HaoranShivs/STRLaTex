#include "app/PopupList.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "app/Theme.h"

namespace pf::gui {

namespace {

constexpr int kItemIndexRole = Qt::UserRole + 1;
constexpr int kTitleRole = PopupList::TitleRole;
constexpr int kDetailRole = PopupList::DetailRole;
constexpr int kGroupHeaderRole = PopupList::GroupHeaderRole;
constexpr int kBadgeRole = PopupList::BadgeRole;
constexpr int kMinWidth = 260;
constexpr int kMaxWidth = 960;
constexpr int kMinHeight = 150;

// 两行条目绘制：主行 = label + 右对齐 detail + 固定 badge，次行 = title。分组标题
// 只画一行小号大写标题。整个绘制由 delegate 负责，因此 QTreeWidget
// 的样式表不需要再为每个 item 设置 padding。
class PopupRowDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize base = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(kGroupHeaderRole).toBool())
            return QSize(base.width(), 26);
        const bool has_title = !index.data(kTitleRole).toString().isEmpty();
        const int line = option.fontMetrics.height();
        const int height = (has_title ? line * 2 : line) + 12;
        return QSize(base.width(), qMax(height, 28));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        const QRect rect = option.rect;
        QStyle* style = option.widget ? option.widget->style() : QApplication::style();

        if (index.data(kGroupHeaderRole).toBool()) {
            QFont font = option.font;
            font.setPointSizeF(qMax(7.0, font.pointSizeF() - 1.5));
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(QColor(theme::kSecondaryText));
            const QString text = index.data(Qt::DisplayRole).toString().toUpper();
            painter->drawText(rect.adjusted(8, 2, -8, -2), Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(text, Qt::ElideRight, qMax(1, rect.width() - 16)));
            painter->restore();
            return;
        }

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(selected ? theme::kAccentSoft : theme::kBlockHover));
            painter->drawRoundedRect(rect.adjusted(2, 1, -2, -1), 4, 4);
        }

        QRect content = rect.adjusted(8, 4, -8, -4);
        const bool checkable = index.data(Qt::CheckStateRole).isValid();
        if (checkable) {
            QStyleOptionButton check;
            check.state = QStyle::State_Enabled;
            if (index.data(Qt::CheckStateRole).toInt() == Qt::Checked)
                check.state |= QStyle::State_On;
            check.rect = QRect(content.left(), content.center().y() - 8, 16, 16);
            style->drawControl(QStyle::CE_CheckBox, &check, painter, option.widget);
            content.setLeft(content.left() + 24);
        }

        const bool enabled = (index.flags() & Qt::ItemIsEnabled) != 0;
        const QColor primary(enabled ? theme::kPrimaryText : theme::kDisabledText);
        const QColor secondary(enabled ? theme::kSecondaryText : theme::kDisabledText);
        const QString label = index.data(Qt::DisplayRole).toString();
        const QString detail = index.data(kDetailRole).toString();
        const QString title = index.data(kTitleRole).toString();
        const QString badge = index.data(kBadgeRole).toString();

        QFont label_font = option.font;
        label_font.setWeight(QFont::DemiBold);
        QFont detail_font = option.font;
        detail_font.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1.5));
        QFont title_font = option.font;
        title_font.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1.0));
        QFont badge_font = option.font;
        badge_font.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1.0));
        badge_font.setWeight(QFont::DemiBold);

        const QFontMetrics label_metrics(label_font);
        const QFontMetrics detail_metrics(detail_font);
        const QFontMetrics badge_metrics(badge_font);
        const int line = qMax(label_metrics.height(), option.fontMetrics.height());
        const auto layout = PopupList::LayoutFirstLine(QRect(content.left(), content.top(), content.width(), line),
                                                       line, detail_metrics, badge_metrics, detail, badge);

        // 编号 badge 占用第一行最右侧的固定区域。detail（venue）只能使用它
        // 左侧的空间，因此 "[3]" 永远不会因为期刊名太长而被 elide 掉。
        if (!layout.badge.isEmpty()) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(enabled ? theme::kAccentSoft : theme::kBlockHover));
            painter->drawRoundedRect(layout.badge, layout.badge.height() / 2.0, layout.badge.height() / 2.0);
            painter->setFont(badge_font);
            painter->setPen(QColor(enabled ? theme::kAccent : theme::kDisabledText));
            painter->drawText(layout.badge, Qt::AlignCenter, badge);
        }

        if (!layout.detail.isEmpty()) {
            painter->setFont(detail_font);
            painter->setPen(secondary);
            painter->drawText(layout.detail, Qt::AlignRight | Qt::AlignVCenter,
                              detail_metrics.elidedText(detail, Qt::ElideRight, layout.detail.width()));
        }

        painter->setFont(label_font);
        painter->setPen(primary);
        painter->drawText(layout.label, Qt::AlignLeft | Qt::AlignVCenter,
                          label_metrics.elidedText(label, Qt::ElideRight, layout.label.width()));

        if (!title.isEmpty()) {
            painter->setFont(title_font);
            painter->setPen(secondary);
            const QRect second(content.left(), content.top() + line + 1, content.width(), line);
            painter->drawText(second, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(title_font).elidedText(title, Qt::ElideRight, qMax(1, second.width())));
        }
        painter->restore();
    }
};

} // namespace

PopupList::FirstLineLayout PopupList::LayoutFirstLine(const QRect& content, int line_height,
                                                       const QFontMetrics& detail_metrics,
                                                       const QFontMetrics& badge_metrics,
                                                       const QString& detail, const QString& badge) {
    constexpr int kGap = 8;
    constexpr int kBadgePadding = 6;
    FirstLineLayout layout;
    const QRect first(content.left(), content.top(), content.width(), line_height);

    int badge_width = 0;
    if (!badge.isEmpty()) {
        badge_width = qMin(first.width(), badge_metrics.horizontalAdvance(badge) + 2 * kBadgePadding);
        const int badge_height = qMax(14, qMin(first.height(), badge_metrics.height() + 4));
        layout.badge = QRect(first.right() - badge_width + 1,
                             first.top() + (first.height() - badge_height) / 2, badge_width, badge_height);
    }

    const int right_edge = first.right() - badge_width - (badge_width > 0 ? kGap : 0);
    int detail_width = 0;
    if (!detail.isEmpty() && right_edge > first.left()) {
        const int available = qMax(0, right_edge - first.left() + 1);
        detail_width = qMin(detail_metrics.horizontalAdvance(detail), qMax(0, available / 2));
        if (detail_width > 0) {
            layout.detail = QRect(right_edge - detail_width + 1, first.top(), detail_width, first.height());
        }
    }

    const int label_right = right_edge - (detail_width > 0 ? detail_width + kGap : 0);
    const int label_width = qMax(1, label_right - first.left() + 1);
    layout.label = QRect(first.left(), first.top(), label_width, first.height());
    return layout;
}

PopupList::PopupList(QWidget* parent) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("popupList"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::Box);
    setStyleSheet(QString("QFrame#popupList { background: white; border: 1px "
                          "solid %1; border-radius: 8px; }"
                          "QLineEdit#popupSearch { border: none; "
                          "border-bottom: 1px solid %1; border-radius: 0;"
                          "            padding: 8px; font-size: 10pt; "
                          "background: transparent; }"
                          "QCheckBox { color: %4; padding: 6px 8px; }"
                          "QLabel#popupSummary { color: %2; }"
                          "QPushButton#popupInsert { background: %3; color: "
                          "white; border: none;"
                          "            border-radius: 5px; padding: 5px 14px; }"
                          "QPushButton#popupInsert:disabled { background: %2; }"
                          "QTreeWidget { border: none; background: "
                          "transparent; outline: 0; }")
                      .arg(theme::kDivider, theme::kSecondaryText, theme::kAccent, theme::kPrimaryText));
    BuildUi();
}

void PopupList::BuildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    search_box_ = new QLineEdit(this);
    search_box_->setObjectName(QStringLiteral("popupSearch"));
    search_box_->setPlaceholderText(QStringLiteral("Type to filter…"));
    search_box_->installEventFilter(this);
    layout->addWidget(search_box_);

    toggle_ = new QCheckBox(this);
    toggle_->setFocusPolicy(Qt::StrongFocus);
    toggle_->installEventFilter(this);
    toggle_->hide();
    layout->addWidget(toggle_);

    list_ = new QTreeWidget(this);
    list_->setColumnCount(1);
    list_->setHeaderHidden(true);
    // 单列铺满弹层宽度：badge 位于 delegate rect 的右边缘，不能受列宽
    // 默认值影响。隐藏表头后 resize mode 仍然生效。
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setStretchLastSection(true);
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(false);
    list_->setIndentation(0);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->viewport()->setMouseTracking(true);
    list_->setItemDelegate(new PopupRowDelegate(list_));
    list_->installEventFilter(this);
    list_->viewport()->installEventFilter(this);
    layout->addWidget(list_, 1);

    auto* footer = new QWidget(this);
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(8, 4, 8, 6);
    footer_layout->setSpacing(6);
    summary_ = new QLabel(footer);
    summary_->setObjectName(QStringLiteral("popupSummary"));
    footer_layout->addWidget(summary_);
    footer_layout->addStretch(1);
    insert_button_ = new QPushButton(QStringLiteral("Insert"), footer);
    insert_button_->setObjectName(QStringLiteral("popupInsert"));
    insert_button_->setDefault(true);
    footer_layout->addWidget(insert_button_);
    summary_->hide();
    insert_button_->hide();
    footer->setObjectName(QStringLiteral("popupFooter"));
    layout->addWidget(footer);

    connect(search_box_, &QLineEdit::textChanged, this, [this](const QString& text) { SetFilter(text); });
    connect(toggle_, &QCheckBox::toggled, this, [this](bool) { UpdateFooter(); });
    connect(insert_button_, &QPushButton::clicked, this, [this]() { ConfirmCurrent(); });
    connect(list_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item || options_.multi_select)
            return;
        if ((item->flags() & Qt::ItemIsEnabled) == 0)
            return;
        ConfirmCurrent();
    });
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item || !options_.multi_select)
            return;
        if ((item->flags() & Qt::ItemIsEnabled) == 0)
            return;
        ConfirmCurrent();
    });
    connect(list_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (rebuilding_rows_ || !item)
            return;
        const QVariant role = item->data(0, kItemIndexRole);
        if (!role.isValid())
            return;
        const int index = role.toInt();
        if (index < 0 || static_cast<size_t>(index) >= all_items_.size())
            return;
        all_items_[static_cast<size_t>(index)].checked = item->checkState(0) == Qt::Checked;
        UpdateFooter();
    });
}

void PopupList::popup(const QPoint& global_pos, const std::vector<Item>& items, const QString& filter) {
    Show(global_pos, items, Options{}, filter);
}

void PopupList::popup(const QPoint& global_pos, const std::vector<Item>& items, const Options& options) {
    Show(global_pos, items, options, QString());
}

void PopupList::Show(const QPoint& global_pos, const std::vector<Item>& items, const Options& options,
                     const QString& filter) {
    // 旧重载与 v2 重载共用 UI：先设置 options，再重建。
    all_items_ = items;
    options_ = options;
    toggle_->setText(options.toggle_label);
    toggle_->setVisible(options.show_toggle);
    toggle_->setChecked(options.toggle_initial);
    summary_->setVisible(options.multi_select);
    insert_button_->setVisible(options.multi_select);

    search_box_->blockSignals(true);
    search_box_->clear();
    search_box_->setText(filter);
    search_box_->blockSignals(false);

    SetFilter(filter);

    adjustSize();
    const int width = qBound(kMinWidth, options.width_px > 0 ? options.width_px : 340, kMaxWidth);
    int height = qMin(options.max_height_px > 0 ? options.max_height_px : 420, sizeHint().height());
    height = qMax(height, kMinHeight);
    resize(width, height);

    QPoint position = global_pos;
    if (QScreen* screen = QGuiApplication::screenAt(global_pos)) {
        const QRect available = screen->availableGeometry();
        position.setX(qBound(available.left(), position.x(), qMax(available.left(), available.right() - width + 1)));
        position.setY(qBound(available.top(), position.y(), qMax(available.top(), available.bottom() - height + 1)));
    }
    move(position);
    show();
    raise();
    search_box_->setFocus(Qt::PopupFocusReason);
}

void PopupList::SetFilter(const QString& text) {
    const QString needle = text.trimmed().toLower();
    QString last_group;
    rebuilding_rows_ = true;
    list_->clear();
    int visible = 0;
    for (size_t index = 0; index < all_items_.size(); ++index) {
        const Item& item = all_items_[index];
        const QString hay =
            item.search.isEmpty() ? (item.label + QLatin1Char(' ') + item.title).toLower() : item.search.toLower();
        if (!needle.isEmpty() && !hay.contains(needle))
            continue;
        if (!item.group.isEmpty() && item.group != last_group) {
            auto* header = new QTreeWidgetItem(list_);
            header->setText(0, item.group);
            header->setData(0, kGroupHeaderRole, true);
            header->setFlags(Qt::NoItemFlags);
            last_group = item.group;
        }
        auto* row = new QTreeWidgetItem(list_);
        row->setText(0, item.label);
        row->setData(0, Qt::UserRole, item.payload);
        row->setData(0, kItemIndexRole, static_cast<int>(index));
        row->setData(0, kTitleRole, item.title);
        row->setData(0, kDetailRole, item.detail);
        row->setData(0, kBadgeRole, item.badge);
        row->setData(0, kGroupHeaderRole, false);
        if (item.checkable) {
            row->setFlags(item.enabled ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable)
                                       : Qt::NoItemFlags);
            row->setCheckState(0, item.checked ? Qt::Checked : Qt::Unchecked);
        } else {
            row->setFlags(item.enabled ? (Qt::ItemIsSelectable | Qt::ItemIsEnabled) : Qt::NoItemFlags);
        }
        ++visible;
    }
    if (visible == 0) {
        // 区分“没有可选项”（例如尚未导入文献）与“当前搜索无命中”。
        const bool no_items = all_items_.empty();
        auto* empty = new QTreeWidgetItem(list_);
        empty->setText(0, no_items && !options_.empty_title.isEmpty() ? options_.empty_title
                                                                      : QStringLiteral("No matches"));
        empty->setData(0, kTitleRole, no_items ? options_.empty_hint : QString());
        empty->setData(0, Qt::UserRole, QString());
        empty->setData(0, kGroupHeaderRole, false);
        empty->setFlags(Qt::NoItemFlags);
    } else {
        for (int i = 0; i < list_->topLevelItemCount(); ++i) {
            if (list_->topLevelItem(i)->flags() & Qt::ItemIsSelectable) {
                list_->setCurrentItem(list_->topLevelItem(i));
                break;
            }
        }
    }
    rebuilding_rows_ = false;
    UpdateFooter();
    if (isVisible()) {
        adjustSize();
        resize(width(), qMin(options_.max_height_px, sizeHint().height()));
    }
}

void PopupList::Rebuild() {
    SetFilter(search_box_ ? search_box_->text() : QString());
}

QStringList PopupList::CheckedPayloads() const {
    QStringList payloads;
    for (const auto& item : all_items_) {
        if (item.checkable && item.checked && !item.payload.isEmpty())
            payloads << item.payload;
    }
    return payloads;
}

QStringList PopupList::SelectedPayloadsForTest() const {
    return CheckedPayloads();
}

void PopupList::UpdateFooter() {
    if (!summary_ || !options_.multi_select)
        return;
    const int count = CheckedPayloads().size();
    summary_->setText(count == 0 ? QStringLiteral("Select one or more references")
                                 : QStringLiteral("%1 selected%2")
                                       .arg(count)
                                       .arg(options_.show_toggle && toggle_ && toggle_->isChecked()
                                                ? QStringLiteral(" · Narrative")
                                                : QString()));
    if (insert_button_)
        insert_button_->setEnabled(count > 0);
}

void PopupList::ToggleCurrentItem() {
    QTreeWidgetItem* item = list_->currentItem();
    if (!item || !(item->flags() & Qt::ItemIsUserCheckable))
        return;
    item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
}

void PopupList::MoveCurrent(int delta) {
    auto* current = list_->currentItem();
    int next = current ? list_->indexOfTopLevelItem(current) + delta : (delta > 0 ? 0 : list_->topLevelItemCount() - 1);
    for (int i = next; i >= 0 && i < list_->topLevelItemCount(); i += delta) {
        if (list_->topLevelItem(i)->flags() & Qt::ItemIsSelectable) {
            list_->setCurrentItem(list_->topLevelItem(i));
            return;
        }
    }
}

bool PopupList::IsGroupHeader(const QTreeWidgetItem* item) const {
    return item && item->data(0, kGroupHeaderRole).toBool();
}

bool PopupList::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        // Narrative 开关自身也要能用键盘操作：Space/Enter 交给 QCheckBox，
        // 这里只接管 Esc（关闭整个弹层）。
        if (watched == toggle_) {
            if (key_event->key() == Qt::Key_Escape) {
                emit dismissed();
                close();
                return true;
            }
            return QFrame::eventFilter(watched, event);
        }
        switch (key_event->key()) {
        case Qt::Key_Down:
            MoveCurrent(1);
            return true;
        case Qt::Key_Up:
            MoveCurrent(-1);
            return true;
        case Qt::Key_Space:
            if (options_.multi_select && watched != search_box_) {
                ToggleCurrentItem();
                return true;
            }
            break;
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
    if (event->type() == QEvent::MouseButtonRelease && options_.multi_select && watched == list_->viewport()) {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        QTreeWidgetItem* item = list_->itemAt(mouse_event->pos());
        if (!item || IsGroupHeader(item))
            return true;
        if (!(item->flags() & Qt::ItemIsUserCheckable))
            return true;
        list_->setCurrentItem(item);
        item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        return true;
    }
    if (event->type() == QEvent::FocusOut) {
        QWidget* focus = QApplication::focusWidget();
        if (focus && (focus == this || isAncestorOf(focus)))
            return QFrame::eventFilter(watched, event);
        emit dismissed();
        close();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void PopupList::ConfirmCurrent() {
    if (options_.multi_select) {
        const QStringList payloads = CheckedPayloads();
        if (payloads.isEmpty())
            return;
        const bool toggle = options_.show_toggle && toggle_ && toggle_->isChecked();
        close();
        emit chosenMany(payloads, toggle);
        return;
    }
    auto* current = list_->currentItem();
    if (!current)
        return;
    const QString payload = current->data(0, Qt::UserRole).toString();
    if (payload.isEmpty())
        return;
    close();
    emit chosen(payload);
}

} // namespace pf::gui
