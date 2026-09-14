#pragma once
// PopupList: lightweight non-modal floating list used by both the "/" block
// command menu and the "@" reference menu (design #6, #9, #13, #69).
// A QFrame popup above the editor - never a QDialog. Full keyboard support:
// Up / Down / Enter / Esc, plus live text filtering.

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
        QString label;        // main text
        QString detail;       // right-side hint (e.g. "Equation", author·year)
        QString group;        // group header (Basic / Academic / References…)
        QString payload;      // command word or citation key or node id
        QString search;       // lowercase filter text (defaults to label)
    };

    explicit PopupList(QWidget* parent = nullptr);

    // Show the popup anchored at global position (above caret).
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
