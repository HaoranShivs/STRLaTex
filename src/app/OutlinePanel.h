#pragma once
// Left panel with two tabs: Document outline and References library
// (design #16, #17). Outline shows only structural blocks; clicking
// highlights + scrolls the editor (design #18).

#include <QLabel>
#include <QWidget>

#include "app/ProjectController.h"

class QTreeWidget;
class QLineEdit;
class QListWidget;
class QTabBar;
class QStackedWidget;

namespace pf::gui {

class OutlinePanel : public QWidget {
    Q_OBJECT

public:
    explicit OutlinePanel(QWidget* parent = nullptr);

    void RebuildFromDocument(const Document& doc,
                             const std::vector<BibEntry>& references);

signals:
    void NodeActivated(const QString& node_id);
    void CitationChosen(const QString& key);  // double-click inserts @cite

private slots:
    void OnTabChanged(int index);

private:
    QWidget* BuildDocumentTab();
    QWidget* BuildReferencesTab();

    QTabBar* tabs_;
    QStackedWidget* stack_;
    // Document tab
    QTreeWidget* outline_;
    // References tab
    QLineEdit* ref_search_;
    QListWidget* ref_list_;
    QLabel* ref_count_;
    std::vector<BibEntry> references_;
};

}  // namespace pf::gui
