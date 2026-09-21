#pragma once
// PopupList：轻量的非模态浮动列表，供「/」块命令菜单和「@」引用菜单共用
// （设计 #6、#9、#13、#69）。它是编辑器上方的 QFrame 弹出层——绝不使用
// QDialog。支持完整的键盘操作：Up / Down / Enter / Esc，并支持实时文本过滤。

#include <QFrame>
#include <QLineEdit>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pf::gui {

class PopupList : public QFrame {
    Q_OBJECT

public:
    struct Item {
        QString label;        // 主文本
        QString detail;       // 右侧提示（例如 "Equation"、作者·年份）
        QString group;        // 分组标题（Basic / Academic / References…）
        QString payload;      // 命令词、引用键或节点 id
        QString search;       // 小写过滤文本（默认为 label）
    };

    explicit PopupList(QWidget* parent = nullptr);

    // 在全局坐标处显示弹出层（锚定在光标上方）。
    void popup(const QPoint& global_pos, const std::vector<Item>& items,
               const QString& filter = {});
    void SetFilter(const QString& text);
    bool eventFilter(QObject* watched, QEvent* event) override;

    bool is_active() const { return isVisible(); }

signals:
    void chosen(const QString& payload);
    void dismissed();

private:
    void Refilter();
    void ConfirmCurrent();

    QLineEdit* search_box_;
    QTreeWidget* list_;
    std::vector<Item> all_items_;
};

}  // namespace pf::gui
