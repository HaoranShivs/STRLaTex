#pragma once
// Structured problems list with severity grouping and filtering, plus a
// raw build log tab for advanced users (design #41-#43).

#include <QWidget>

#include "core/Diagnostic.h"

class QListWidget;
class QPlainTextEdit;
class QTabBar;
class QStackedWidget;
class QLabel;

namespace pf::gui {

class ProblemsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ProblemsPanel(QWidget* parent = nullptr);

    void SetDiagnostics(const std::vector<Diagnostic>& diagnostics);
    void SetBuildLog(const QString& log);
    void SetStale(bool stale, int behind_by);

signals:
    void DiagnosticActivated(const pf::Diagnostic& diagnostic);

private:
    QWidget* BuildProblemsTab();
    QWidget* BuildLogTab();
    void Refilter();

    QTabBar* tabs_;
    QStackedWidget* stack_;
    QListWidget* list_;
    QPlainTextEdit* log_view_;
    QLabel* count_label_;
    QString filter_ = "all";  // all / error / warning
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace pf::gui
