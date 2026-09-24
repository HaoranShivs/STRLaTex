#pragma once
// 左侧面板包含两个标签页：Document 大纲与 References 文献库
//（设计 #16、#17）。大纲只显示结构化区块；点击会高亮并滚动编辑器
//（设计 #18）。

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

    void RebuildFromDocument(const Document& doc, const std::vector<BibEntry>& references);

    // 反向导航（UI 方案 §10）：编辑器告诉大纲光标所在行属于哪个小节，
    // 这样高亮项就能在长稿件中跟随用户移动。空 key 会清除选中
    //（front matter 行没有对应的大纲节点）。
    void SelectNode(const QString& outline_key);

  signals:
    void NodeActivated(const QString& node_id);
    void CitationChosen(const QString& key); // 双击会插入 @cite

  private slots:
    void OnTabChanged(int index);

  private:
    QWidget* BuildDocumentTab();
    QWidget* BuildReferencesTab();

    QTabBar* tabs_;
    QStackedWidget* stack_;
    // Document 标签页
    QTreeWidget* outline_;
    // SelectNode 最近一次高亮的 key：RebuildFromDocument 会重新应用它，
    // 使大纲在文档刷新后仍能持续跟踪光标。
    QString selected_key_;
    // References 标签页
    QLineEdit* ref_search_;

    // 将当前文献搜索词重新应用到列表内容上。
    void ApplyReferenceFilter();
    QListWidget* ref_list_;
    QLabel* ref_count_;
    std::vector<BibEntry> references_;
};

} // namespace pf::gui
