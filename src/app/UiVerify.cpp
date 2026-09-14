// UI verification: open a real project, populate content, take screenshots
// of the main window states (welcome, workspace, slash menu, problems).
#include <QApplication>
#include <QFileDialog>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QTimer>
#include <filesystem>
#include <iostream>

#include "app/MainWindow.h"
#include "app/ProjectController.h"

using namespace pf::gui;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    std::filesystem::remove_all("/tmp/pf-ui-demo");
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::cout << (ok ? "[ OK  ] " : "[FAIL] ") << what << "\n";
        if (!ok) ++failures;
    };

    MainWindow window;
    window.show();

    QTimer::singleShot(300, [&]() {
        // 1. Create + populate a project through the controller.
        ProjectController* controller = window.controller();
        check(controller != nullptr, "controller reachable");

        // PF_UI_PROJECT opens an existing project instead of building the
        // demo one, so a real manuscript can be inspected as it renders.
        const QString existing = qEnvironmentVariable("PF_UI_PROJECT");
        QString dir = existing.isEmpty() ? QStringLiteral("/tmp/pf-ui-demo")
                                        : existing;
        if (!existing.isEmpty()) {
            check(window.OpenProjectDir(dir), "open existing project");
        } else {
        check(controller->NewProject(dir), "new project");
        controller->StartAutosave();
        // Switch the main window into workspace view programmatically.
        // (NewProject via dialog calls ShowWorkspace; direct controller use
        // bypasses it, so open through the window API.)
        controller->session().Save();
        controller->CloseProject();
        bool opened = window.OpenProjectDir(dir);
        if (!opened) {
            // Recovery open can fail only if project.paper is missing; retry
            // direct to surface the error.
            std::string err;
            opened = controller->session().OpenProject(dir.toStdString(), &err);
            std::cout << "  open error: " << err << "\n";
        }
        check(opened, "open via window (workspace shown)");
        }

        if (!existing.isEmpty()) {
            // Existing project: capture it as opened, then show what the
            // re-flow and the hover insert affordance look like on real
            // content.
            QTimer::singleShot(1800, [&window]() {
                window.grab().save("/tmp/pf-ui-workspace.png");
                std::cout << "[ SAVE ] workspace (as opened)\n";

                auto pump = [](int ms) {
                    QElapsedTimer timer;
                    timer.start();
                    while (timer.elapsed() < ms) {
                        QApplication::processEvents(QEventLoop::AllEvents, 20);
                    }
                };
                auto find_row = [&window](const QString& key) -> QPlainTextEdit* {
                    for (QPlainTextEdit* edit :
                         window.findChildren<QPlainTextEdit*>()) {
                        if (edit->isVisible() &&
                            edit->property("row_focus_key").toString() ==
                                key) {
                            return edit;
                        }
                    }
                    return nullptr;
                };

                // Clicking into the abstract and leaving it commits the row,
                // which is the path that softens a hard-wrapped paste.
                if (QPlainTextEdit* abstract = find_row("front:abstract")) {
                    abstract->setFocus(Qt::MouseFocusReason);
                    pump(150);
                    if (QPlainTextEdit* again = find_row("front:abstract")) {
                        again->clearFocus();
                    }
                    pump(600);
                    window.grab().save("/tmp/pf-ui-reflowed.png");
                    std::cout << "[ SAVE ] abstract after commit (re-flowed)\n";
                }

                // Hover the insert strip after the last block.
                std::vector<QWidget*> gaps;
                for (QWidget* w : window.findChildren<QWidget*>()) {
                    if (w->property("gap_anchor").isValid() && w->isVisible()) {
                        gaps.push_back(w);
                    }
                }
                std::cout << "[ INFO ] live gaps: " << gaps.size() << "\n";
                if (!gaps.empty()) {
                    QWidget* gap = gaps.back();
                    QEvent enter(QEvent::Enter);
                    QApplication::sendEvent(gap, &enter);
                    pump(400);
                    window.grab().save("/tmp/pf-ui-gap.png");
                    std::cout << "[ SAVE ] hover insert affordance\n";
                }
                QApplication::exit(0);
            });
            return;
        }

        controller->SetTitle("Weakly Supervised Infrared Small Target Detection");
        controller->SetAffiliationsText(
            "School of Information and Communication Engineering, University "
            "One; Institute of Optoelectronics, Institute Two");
        controller->SetAuthorsText(
            "Tanran Shi\u00b9\u00b2 · Author B\u00b9 · Author C\u00b2");
        controller->SetAbstract(
            "Infrared small target detection has attracted considerable "
            "attention in recent years, yet robust detection under complex "
            "backgrounds remains difficult because targets occupy only a few "
            "pixels and carry almost no texture. This paper studies a weakly "
            "supervised formulation that learns from image-level labels.");
        controller->SetAuthorsText(
            "Tanran Shi\u00b9\u00b2 · Author B\u00b9 · Author C\u00b2");
        controller->SetKeywordsText("infrared, small target, deep learning");
        auto sec = controller->InsertSection("Introduction");
        // Long enough to reach a second page, so the preview shows the seam
        // between sheet 1 and sheet 2.
        for (int i = 0; i < 26; ++i) {
            controller->InsertParagraph(
                sec.created_node,
                QString("Paragraph %1. Infrared small target detection under "
                        "complex backgrounds remains difficult because "
                        "targets occupy only a few pixels, carry almost no "
                        "texture, and are easily confused with cloud edges, "
                        "rooftops and wave crests in the surrounding scene.")
                    .arg(i + 1));
        }
        controller->InsertParagraph(
            sec.created_node,
            "Infrared small target detection has received considerable "
            "attention in recent years. Previous methods primarily rely on "
            "hand-crafted features, which degrade quickly when the target "
            "contrast drops or when the background contains cluttered "
            "structures such as cloud edges, rooftops and wave crests. Deep "
            "models trained end to end have improved recall, but they still "
            "need pixel-level annotations that are expensive to obtain and "
            "inconsistent between annotators. We therefore investigate a "
            "weakly supervised pipeline: image-level labels drive a "
            "coarse-to-fine refinement stage, and a segmentation head "
            "recovers the target mask without any per-pixel supervision. "
            "Experiments on public benchmarks show consistent gains over "
            "hand-crafted baselines while reducing annotation cost.");
        controller->InsertEquation(sec.created_node, "L = L_{seg} + \\lambda L_{aux}", true);
        controller->ImportBibliographyText(QString(
            "@article{wang2025, author={W. Wang}, title={Infrared Target "
            "Detection}, year={2025}}\n"
            "@article{li2024, author={L. Li}, title={Small Target Survey}, "
            "year={2024}}"));

        // Trigger the view refresh (documentChanged fires on each edit; give
        // the loop a moment).
        QTimer::singleShot(400, [&window, controller, &failures]() {
            window.grab().save("/tmp/pf-ui-workspace.png");
            std::cout << "[ SAVE ] workspace screenshot\n";

            // 3. Build; the controller emits buildFinished when done.
            // Setup edits drain through debounced auto-builds and can
            // supersede the first request (latest-wins), so retry a few
            // times until a build for the current revision succeeds.
            static int build_attempts = 0;
            ProjectController* ctl = controller;
            QObject::connect(controller, &ProjectController::buildFinished,
                             [&window, ctl](bool success, const QString&) {
                                 std::cout << "[ EVNT ] buildFinished success="
                                           << success << "\n";
                                 if (success) {
                                     // buildFinished arrives on the worker
                                     // thread; hop to the main thread before
                                     // touching widgets.
                                     QMetaObject::invokeMethod(
                                         &window,
                                         [&window]() {
                                             QTimer::singleShot(
                                                 1200, [&window]() {
                                                     window.grab().save(
                                                         "/tmp/pf-ui-built.png");
                                                     std::cout
                                                         << "[ SAVE ] built "
                                                            "screenshot\n";
                                                     // Zoom the preview and
                                                     // capture the result.
                                                     // Show the pages as a
                                                     // continuous column.
                                                     window.ZoomPreviewForTest(
                                                         0.0, 0.0, 0.45);
                                                     QTimer::singleShot(
                                                         600, [&window]() {
                                                             window.grab().save(
                                                                 "/tmp/"
                                                                 "pf-ui-pages."
                                                                 "png");
                                                             std::cout
                                                                 << "[ SAVE ] "
                                                                    "page seam"
                                                                    " screenshot"
                                                                    "\n";
                                                             window.ZoomPreviewForTest(
                                                                 2.5, 0.05,
                                                                 0.12);
                                                         });
                                                     QTimer::singleShot(
                                                         1500, [&window]() {
                                                             window.grab().save(
                                                                 "/tmp/"
                                                                 "pf-ui-zoomed."
                                                                 "png");
                                                             std::cout
                                                                 << "[ SAVE ] "
                                                                    "zoomed "
                                                                    "screenshot"
                                                                    "\n";
                                                             QApplication::
                                                                 exit(0);
                                                         });
                                                 });
                                         },
                                         Qt::QueuedConnection);
                                     return;
                                 }
                                 if (++build_attempts >= 6) {
                                     std::cout << "[FAIL] build did not succeed\n";
                                     QApplication::exit(1);
                                 }
                                 QMetaObject::invokeMethod(
                                     ctl,
                                     [ctl]() { ctl->RequestBuild(true); },
                                     Qt::QueuedConnection);
                             });
            controller->RequestBuild(true);
        });
    });

    QTimer::singleShot(60000, [&]() { QApplication::exit(1); });
    return app.exec();
}
