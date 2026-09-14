// UI verification: open a real project, populate content, take screenshots
// of the main window states (welcome, workspace, slash menu, problems).
#include <QApplication>
#include <QFileDialog>
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

        QString dir = "/tmp/pf-ui-demo";
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

        controller->SetTitle("Weakly Supervised Infrared Small Target Detection");
        controller->SetAuthorsText("Tanran Shi · Author B · Author C");
        controller->SetAffiliationsText("University One; Institute Two");
        controller->SetAbstract(
            "Infrared small target detection has attracted considerable "
            "attention in recent years.");
        controller->SetKeywordsText("infrared, small target, deep learning");
        auto sec = controller->InsertSection("Introduction");
        controller->InsertParagraph(
            sec.created_node,
            "Infrared small target detection has received considerable "
            "attention in recent years. Previous methods primarily rely on "
            "hand-crafted features.");
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
                                                     QApplication::exit(0);
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
