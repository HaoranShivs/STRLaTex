#pragma once
// 未打开任何项目时显示的欢迎页（设计 #51）：标题、标语、
// New/Open 操作以及最近项目列表。

#include <QWidget>

class QLabel;
class QPushButton;
class QListWidget;

namespace pf::gui {

class WelcomePage : public QWidget {
    Q_OBJECT

  public:
    explicit WelcomePage(QWidget* parent = nullptr);

    void AddRecent(const QString& name, const QString& path);

  signals:
    void NewProjectRequested();
    void OpenProjectRequested();
    void RecentActivated(const QString& path);

  private:
    QListWidget* recent_list_;
};

} // namespace pf::gui
