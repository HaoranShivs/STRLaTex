#pragma once
// Welcome page shown when no project is open (design #51): title, tagline,
// New/Open actions, and recent projects list.

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

}  // namespace pf::gui
