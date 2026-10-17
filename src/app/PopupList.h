#pragma once
// PopupList：轻量的非模态浮动列表，供「/」块命令菜单、「@」引用菜单和
// Citation picker v2 共用（设计 #6、#9、#13、#69）。它是编辑器上方的
// QFrame 弹出层——绝不使用 QDialog。支持完整的键盘操作：
// Up / Down / Enter / Esc、实时文本过滤，以及 v2 的：
//
//   * 两行条目（主行 + 标题/描述次行，右列显示 venue / 编号），
//   * 多选（复选框 + Enter 确认）与底部的已选数量提示，
//   * 可选开关（例如 Narrative），
//   * 空状态标题 / 提示（不再只是一行灰字）。
//
// 兼容性：旧的 Item 字段与 chosen(QString) 语义保持不变；旧调用方
// （块菜单、交叉引用菜单）不传 Options，行为与 v1 一致。

#include <QFontMetrics>
#include <QFrame>
#include <QRect>
#include <QStringList>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace pf::gui {

class PopupList : public QFrame {
    Q_OBJECT

  public:
    struct Item {
        // 前五个字段与 v1 完全相同。
        QString label;   // 主文本：作者 · 年份 / 命令名
        QString detail;  // 右侧提示：venue + 编号 / "Insert" / 类型
        QString group;   // 分组标题（Basic / CITED IN THIS PAPER…）
        QString payload; // 命令词、引用键或节点 id
        QString search;  // 小写过滤文本（默认为 label）

        // v2 追加（Appending keeps old aggregate initialisers
        // source-compatible）。
        QString title; // 次行：论文标题、描述（可为空）
        // 第一行末尾的固定 badge（WP-01 修复）：引用编号 "[3]"、状态等。
        // badge 有自己的绘制区域，venue/detail 再长也不会把它挤出屏幕。
        QString badge;
        bool checkable = false;
        bool checked = false;
        bool enabled = true;
    };

    // QTreeWidget item 数据角色（测试与宿主需要读取 delegate 绘制的内容）。
    // payload 保持 Qt::UserRole，与 v1 兼容。
    enum DataRole {
        PayloadRole = Qt::UserRole,
        TitleRole = Qt::UserRole + 2,
        DetailRole = Qt::UserRole + 3,
        GroupHeaderRole = Qt::UserRole + 4,
        BadgeRole = Qt::UserRole + 5,
    };

    // 第一行的布局结果。badge 是独立的右侧区域，任何 detail 长度都不会
    // 改变它的宽度或位置；UI 与回归测试共用同一套计算。
    struct FirstLineLayout {
        QRect label;
        QRect detail;
        QRect badge;
    };
    static FirstLineLayout LayoutFirstLine(const QRect& content, int line_height,
                                           const QFontMetrics& detail_metrics,
                                           const QFontMetrics& badge_metrics, const QString& detail,
                                           const QString& badge);

    struct Options {
        bool multi_select = false;
        bool show_toggle = false; // 例如 Narrative 开关
        QString toggle_label;
        bool toggle_initial = false;
        int width_px = 340;
        int max_height_px = 420;
        QString empty_title = QStringLiteral("No matches");
        QString empty_hint;
    };

    explicit PopupList(QWidget* parent = nullptr);

    // 兼容旧调用（块命令、交叉引用）：单选，Enter 立即确认。
    void popup(const QPoint& global_pos, const std::vector<Item>& items, const QString& filter = {});
    // v2：多选 / 开关 / 更宽的两行布局。
    void popup(const QPoint& global_pos, const std::vector<Item>& items, const Options& options);

    void SetFilter(const QString& text);
    bool eventFilter(QObject* watched, QEvent* event) override;

    // 测试接缝：widget 级测试需要检查过滤、分组、勾选状态与空状态。
    QTreeWidget* ListForTest() const {
        return list_;
    }
    QLineEdit* SearchForTest() const {
        return search_box_;
    }
    QCheckBox* ToggleForTest() const {
        return toggle_;
    }
    QPushButton* InsertButtonForTest() const {
        return insert_button_;
    }
    QStringList SelectedPayloadsForTest() const;

  signals:
    // 旧事件：单选时仍发出，保持现有调用方不变。
    void chosen(const QString& payload);
    // 新事件：多选/开关模式。单选模式下不发出（避免旧调用方重复处理）。
    void chosenMany(const QStringList& payloads, bool toggle_checked);
    void dismissed();

  private:
    void BuildUi();
    void Show(const QPoint& global_pos, const std::vector<Item>& items, const Options& options, const QString& filter);
    void Rebuild();
    void ConfirmCurrent();
    void UpdateFooter();
    void ToggleCurrentItem();
    void MoveCurrent(int delta);
    QStringList CheckedPayloads() const;
    bool IsGroupHeader(const QTreeWidgetItem* item) const;

    QLineEdit* search_box_ = nullptr;
    QCheckBox* toggle_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* insert_button_ = nullptr;
    QTreeWidget* list_ = nullptr;
    std::vector<Item> all_items_;
    Options options_;
    // 重建期间由程序设置 checkState；不得把它误认为用户勾选。
    bool rebuilding_rows_ = false;
};

} // namespace pf::gui
