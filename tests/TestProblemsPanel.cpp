// ProblemsPanel widget tests (Build Diagnostics plan §20-§24, §30, §42, §49):
// sorting, counts, empty states, streamed build log, and build isolation.
#include "TestMain.hpp"

#include <QListWidget>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QTabBar>

#include "app/ProblemsPanel.h"
#include "build/BuildEvent.h"

using namespace pf;
using namespace pf::gui;

namespace {

QListWidget* ProblemList(ProblemsPanel& panel) {
    return panel.findChild<QListWidget*>("problemsList");
}
QPlainTextEdit* LogView(ProblemsPanel& panel) {
    return panel.findChild<QPlainTextEdit*>("buildLogView");
}
QTabBar* Tabs(ProblemsPanel& panel) {
    return panel.findChild<QTabBar*>("problemTabs");
}

Diagnostic MakeDiag(DiagnosticSeverity severity, std::string message,
                    std::string code = "X") {
    Diagnostic d;
    d.severity = severity;
    d.message = std::move(message);
    d.code = std::move(code);
    return d;
}

BuildEvent MakeEvent(const BuildId& id, BuildEventType type,
                     std::string message) {
    BuildEvent e;
    e.build_id = id;
    e.timestamp_ms = BuildEventNowMs();
    e.type = type;
    e.message = std::move(message);
    return e;
}

}  // namespace

PF_TEST(ProblemsPanelEmptyStates) {
    ProblemsPanel panel;
    auto* list = ProblemList(panel);
    PF_CHECK(list != nullptr);
    // Never built (plan §42).
    PF_CHECK_EQ(list->count(), 1);
    PF_CHECK(list->item(0)->text().contains("No build diagnostics available"));
    // Building...
    panel.AppendEvent(MakeEvent(BuildId("b1"), BuildEventType::BuildStarted,
                                "Build started (snapshot s1)"));
    PF_CHECK_EQ(list->count(), 1);
    PF_CHECK(list->item(0)->text().contains("Building"));
    // Done with zero problems: the ok-empty state (plan §42).
    panel.SetDiagnostics({});
    PF_CHECK_EQ(list->count(), 1);
    PF_CHECK(list->item(0)->text().contains("No problems detected"));
}

PF_TEST(ProblemsPanelSortsBySeverity) {
    ProblemsPanel panel;
    std::vector<Diagnostic> input;
    input.push_back(MakeDiag(DiagnosticSeverity::Warning, "w1", "W-1"));
    input.push_back(MakeDiag(DiagnosticSeverity::Info, "i1", "I-1"));
    input.push_back(MakeDiag(DiagnosticSeverity::Error, "e1", "E-1"));
    input.push_back(MakeDiag(DiagnosticSeverity::Error, "e2", "E-2"));
    panel.SetDiagnostics(input);

    auto* list = ProblemList(panel);
    // Error, Warning, Info; ties in arrival (document) order (plan §21).
    PF_CHECK_EQ(list->count(), 4);
    PF_CHECK(list->item(0)->text().contains("e1"));
    PF_CHECK(list->item(1)->text().contains("e2"));
    PF_CHECK(list->item(2)->text().contains("w1"));
    PF_CHECK(list->item(3)->text().contains("i1"));
    // Severity is readable without color: an icon + word per row (plan §23).
    PF_CHECK(list->item(0)->text().contains("ERROR"));
    PF_CHECK(list->item(2)->text().contains("WARNING"));
    // Tab header shows the live count (plan §24).
    PF_CHECK(Tabs(panel)->tabText(0).contains("(4)"));
}

PF_TEST(ProblemsPanelStaleMarker) {
    ProblemsPanel panel;
    std::vector<Diagnostic> input;
    input.push_back(MakeDiag(DiagnosticSeverity::Error, "boom", "E-1"));
    panel.SetDiagnostics(input);
    PF_CHECK(!Tabs(panel)->tabText(0).contains("Outdated"));
    // Document moved on without a rebuild: Outdated status text only (§49).
    panel.SetStale(true, 3);
    PF_CHECK(Tabs(panel)->tabText(0).contains("Outdated"));
    PF_CHECK_EQ(ProblemList(panel)->count(), 1);  // rows untouched
    // The next build's result clears it (plan §49).
    panel.SetDiagnostics({});
    PF_CHECK(!Tabs(panel)->tabText(0).contains("Outdated"));
}

PF_TEST(ProblemsPanelStreamedBuildLog) {
    ProblemsPanel panel;
    const BuildId build("b1");
    panel.AppendEvent(MakeEvent(build, BuildEventType::BuildStarted,
                                "Build started (snapshot s1)"));
    panel.AppendEvent(
        MakeEvent(build, BuildEventType::GenerationStarted, "Generating LaTeX"));
    panel.AppendEvent(MakeEvent(build, BuildEventType::StdOut,
                                "This is pdfTeX\nOutput written"));
    panel.AppendEvent(MakeEvent(build, BuildEventType::StdErr,
                                "kaboom on stderr"));
    panel.AppendEvent(MakeEvent(build, BuildEventType::ProcessFinished,
                                "Process exited with code 0"));
    panel.AppendEvent(
        MakeEvent(build, BuildEventType::BuildSucceeded,
                  "Build succeeded (1.662 s)"));
    auto* view = LogView(panel);
    const QString text = view->toPlainText();
    // Lifecycle events render [HH:mm:ss.zzz] message (plan §7).
    PF_CHECK(text.contains("Build started"));
    PF_CHECK(text.contains("Generating LaTeX"));
    // Raw compiler output survives unmodified behind stream headers (§7).
    PF_CHECK(text.contains("[stdout]"));
    PF_CHECK(text.contains("This is pdfTeX\nOutput written"));
    PF_CHECK(text.contains("[stderr]"));
    PF_CHECK(text.contains("kaboom on stderr"));
    PF_CHECK(text.contains("Process exited with code 0"));
    PF_CHECK(text.contains("Build succeeded (1.662 s)"));
    QRegularExpression ts(QStringLiteral(R"(\[\d{2}:\d{2}:\d{2}\.\d{3}\])"));
    PF_CHECK(ts.match(text).hasMatch());
}

PF_TEST(ProblemsPanelLogIsolatedPerBuild) {
    ProblemsPanel panel;
    const BuildId first("b1");
    const BuildId second("b2");
    panel.AppendEvent(
        MakeEvent(first, BuildEventType::BuildStarted, "Build started"));
    panel.AppendEvent(MakeEvent(first, BuildEventType::StdOut, "old build text"));
    // A newer build starts: the previous log is cleared, never merged
    // (plan §8/§30).
    panel.AppendEvent(
        MakeEvent(second, BuildEventType::BuildStarted, "Build started"));
    panel.AppendEvent(
        MakeEvent(second, BuildEventType::StdOut, "new build text"));
    const QString text = LogView(panel)->toPlainText();
    PF_CHECK(!text.contains("old build text"));
    PF_CHECK(text.contains("new build text"));
}
