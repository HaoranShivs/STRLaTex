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

    // Reverse navigation (UI plan §10): the editor tells the outline which
    // section owns the row under the caret, so the highlighted item follows
    // the user through a long manuscript. An empty key clears the selection
    // (front-matter rows have no outline node).
    void SelectNode(const QString& outline_key);

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
    // The key SelectNode last highlighted: RebuildFromDocument re-applies it
    // so the outline keeps tracking the caret across document refreshes.
    QString selected_key_;
    // References tab
    QLineEdit* ref_search_;

    // Re-applies the current reference search term to the list contents.
    void ApplyReferenceFilter();
    QListWidget* ref_list_;
    QLabel* ref_count_;
    std::vector<BibEntry> references_;
};

}  // namespace pf::gui
