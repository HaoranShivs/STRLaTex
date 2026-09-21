// ProblemsPanel widget 测试（Build Diagnostics 方案 §20-§24、§30、§42、§49）：
// 排序、计数、空状态、流式 build 日志与 build 隔离。
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
    // 从未 build 过（方案 §42）。
    PF_CHECK_EQ(list->count(), 1);
    PF_CHECK(list->item(0)->text().contains("No build diagnostics available"));
    // build 进行中……
    panel.AppendEvent(MakeEvent(BuildId("b1"), BuildEventType::BuildStarted,
                                "Build started (snapshot s1)"));
    PF_CHECK_EQ(list->count(), 1);
    PF_CHECK(list->item(0)->text().contains("Building"));
    // 完成且零问题：ok-empty 状态（方案 §42）。
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
    // 依次为 Error、Warning、Info；同级按到达（文档）顺序（方案 §21）。
    PF_CHECK_EQ(list->count(), 4);
    PF_CHECK(list->item(0)->text().contains("e1"));
    PF_CHECK(list->item(1)->text().contains("e2"));
    PF_CHECK(list->item(2)->text().contains("w1"));
    PF_CHECK(list->item(3)->text().contains("i1"));
    // 不依赖颜色也能读出严重级别：每行一个图标 + 文字（方案 §23）。
    PF_CHECK(list->item(0)->text().contains("ERROR"));
    PF_CHECK(list->item(2)->text().contains("WARNING"));
    // 标签页标题显示实时计数（方案 §24）。
    PF_CHECK(Tabs(panel)->tabText(0).contains("(4)"));
}

PF_TEST(ProblemsPanelStaleMarker) {
    ProblemsPanel panel;
    std::vector<Diagnostic> input;
    input.push_back(MakeDiag(DiagnosticSeverity::Error, "boom", "E-1"));
    panel.SetDiagnostics(input);
    PF_CHECK(!Tabs(panel)->tabText(0).contains("Outdated"));
    // 文档已变更但未重新 build：仅显示 Outdated 状态文字（§49）。
    panel.SetStale(true, 3);
    PF_CHECK(Tabs(panel)->tabText(0).contains("Outdated"));
    PF_CHECK_EQ(ProblemList(panel)->count(), 1);  // 各行保持不变
    // 下一次 build 的结果会清除该标记（方案 §49）。
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
    // 生命周期事件渲染为 [HH:mm:ss.zzz] message（方案 §7）。
    PF_CHECK(text.contains("Build started"));
    PF_CHECK(text.contains("Generating LaTeX"));
    // 原始编译器输出在流式标头之后原样保留（§7）。
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
    // 新的 build 开始时清除上一次的日志，绝不合并
    // （方案 §8/§30）。
    panel.AppendEvent(
        MakeEvent(second, BuildEventType::BuildStarted, "Build started"));
    panel.AppendEvent(
        MakeEvent(second, BuildEventType::StdOut, "new build text"));
    const QString text = LogView(panel)->toPlainText();
    PF_CHECK(!text.contains("old build text"));
    PF_CHECK(text.contains("new build text"));
}
