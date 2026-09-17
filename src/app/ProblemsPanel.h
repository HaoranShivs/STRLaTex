#pragma once
// Problems + Build Log panel (Build Diagnostics plan §6-§8, §20-§29, §41-§43).
//
// The panel is a *renderer* over structured data: Problems come from
// Diagnostic value objects, the Build Log from BuildEvent value objects. It
// never parses compiler output or log text itself - classification belongs to
// DiagnosticMapper in the build layer.

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

    // ---- Problems tab ----
    // Replaces the whole diagnostic set of one build (plan §30: the panel is
    // updated once per completed build, not per arriving diagnostic).
    void SetDiagnostics(const std::vector<Diagnostic>& diagnostics);
    void SetStale(bool stale, int behind_by);
    void ShowProblemsTab();

    // ---- Build Log tab ----
    // Streams one lifecycle event; a BuildStarted event begins a new build
    // and clears the previous log (plan §8/§30).
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

    QString filter_ = "all";  // all / error / warning
    std::vector<Diagnostic> diagnostics_;
    ProblemsState state_ = ProblemsState::NeverBuilt;
    bool stale_ = false;

    // Current build log (plan §6 BuildLogModel): events of the build being
    // displayed; Clear View only resets the widget, not this model.
    BuildId log_build_id_;
    std::vector<pf::BuildEvent> events_;
    std::string last_stream_;   // "out"/"err" for [stdout]/[stderr] headers
    bool log_needs_newline_ = false;  // last raw chunk ended mid-line
};

}  // namespace pf::gui
