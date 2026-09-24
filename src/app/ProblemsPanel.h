#pragma once
// Problems + Build Log 面板（Build Diagnostics 方案 §6-§8、§20-§29、§41-§43）。
//
// 该面板是结构化数据之上的 *渲染器*：Problems 来自 Diagnostic 值对象，
// Build Log 来自 BuildEvent 值对象。它自身从不解析编译器输出或日志文本——
// 分类工作属于 build 层的 DiagnosticMapper。

#include <QString>
#include <QWidget>
#include <vector>

#include "build/BuildEvent.h"
#include "core/Diagnostic.h"

class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTabBar;
class QStackedWidget;
class QLabel;

namespace pf::gui {

class ProblemsPanel : public QWidget {
    Q_OBJECT

  public:
    explicit ProblemsPanel(QWidget* parent = nullptr);

    // ---- Problems 标签页 ----
    // 整体替换一次 build 的 Diagnostic 集合（方案 §30：面板在每次 build
    // 完成后更新一次，而不是每到达一条 Diagnostic 就更新）。
    void SetDiagnostics(const std::vector<Diagnostic>& diagnostics);
    void SetStale(bool stale, int behind_by);
    void ShowProblemsTab();

    // ---- Build Log 标签页 ----
    // 流式接收一个生命周期事件；BuildStarted 事件会开启新的 build
    // 并清空此前的日志（方案 §8/§30）。
    void AppendEvent(const pf::BuildEvent& event);
    void ShowBuildLog();

  signals:
    void DiagnosticActivated(const pf::Diagnostic& diagnostic);

  private:
    enum class ProblemsState { NeverBuilt, Building, Done };

    QWidget* BuildProblemsTab();
    QWidget* BuildLogTab();
    void Refilter();
    void UpdateHeader();
    QString LocationText(const pf::Diagnostic& d) const;

    QTabBar* tabs_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QListWidget* list_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* count_label_ = nullptr;
    QPushButton* auto_scroll_button_ = nullptr;

    QString filter_ = "all"; // all / error / warning
    std::vector<Diagnostic> diagnostics_;
    ProblemsState state_ = ProblemsState::NeverBuilt;
    bool stale_ = false;

    // 当前 build 日志（方案 §6 BuildLogModel）：正在显示的 build 的事件；
    // Clear View 只重置控件，不重置此模型。
    BuildId log_build_id_;
    std::vector<pf::BuildEvent> events_;
    std::string last_stream_;        // [stdout]/[stderr] 头部所用的 "out"/"err"
    bool log_needs_newline_ = false; // 上一个原始数据块在一行中途结束
};

} // namespace pf::gui
