#include "ScopedTempDir.hpp"
#include "TestMain.hpp"
#include "app/MainWindow.h"
#include "app/TemplateImportDialog.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTest>
#include <fstream>

using namespace pf;
using namespace pf::gui;

PF_TEST(TemplateGuiFileActionAndSelectorFollowUndoAndReopen) {
    test::ScopedTempDir temp("pf-template-gui-selection");
    MainWindow window;
    auto* action = window.findChild<QAction*>("importTemplateAction");
    auto* selector = window.findChild<QComboBox*>("templateSelector");
    PF_CHECK(action && selector);
    if (!action || !selector)
        return;
    PF_CHECK(!action->isEnabled());
    auto* controller = window.controller();
    PF_CHECK(controller->NewProject(
        QString::fromStdString((temp / "project").string())));
    PF_CHECK(action->isEnabled());
    PF_CHECK(selector->currentData().toString() == "generic-article");
    controller->ChangeTemplate("ieee-conference");
    controller->CancelBuild();
    PF_CHECK(selector->currentData().toString() == "ieee-conference");
    controller->Undo();
    controller->CancelBuild();
    PF_CHECK(selector->currentData().toString() == "generic-article");
    controller->Redo();
    controller->CancelBuild();
    PF_CHECK(selector->currentData().toString() == "ieee-conference");
    controller->Save();
    controller->FlushSaves();
    PF_CHECK(controller->OpenProject(
        QString::fromStdString((temp / "project").string())));
    PF_CHECK(selector->currentData().toString() == "ieee-conference");
}

PF_TEST(TemplateGuiInvalidZipFailsWithoutInstallingOrChangingTheProject) {
    test::ScopedTempDir temp("pf-template-gui-invalid");
    std::ofstream(temp / "broken.zip") << "not a zip file";
    ProjectController controller;
    PF_CHECK(controller.NewProject(
        QString::fromStdString((temp / "project").string())));
    const auto revision = controller.current_revision();
    const auto binding = controller.session().state().template_binding();
    TemplateImportDialog dialog(
        &controller, QString::fromStdString((temp / "broken.zip").string()));
    dialog.show();
    auto* status = dialog.findChild<QLabel*>("templateImportStatus");
    auto* details = dialog.findChild<QPlainTextEdit*>("templateImportDetails");
    auto* import = dialog.findChild<QPushButton*>("importTemplateButton");
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000 && !status->text().contains("failed"))
        QTest::qWait(10);
    PF_CHECK(status->text().contains("failed"));
    PF_CHECK(details->toPlainText().contains("ZIP"));
    PF_CHECK(!import->isEnabled());
    PF_CHECK(controller.current_revision() == revision);
    PF_CHECK(controller.session().state().template_binding() == binding);
    PF_CHECK(!std::filesystem::exists(temp / "project/template-resources"));
    dialog.reject();
}
