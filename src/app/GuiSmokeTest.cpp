// GUI end-to-end smoke test: drive the controller directly (no display
// needed) - new project, edit via the same code paths the UI uses, build,
// and verify the PDF exists. Run under any QPA platform.
#include <QCoreApplication>
#include <QTimer>
#include <chrono>
#include <filesystem>
#include <iostream>

#include "app/ProjectController.h"

using namespace pf::gui;

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    auto dir = std::filesystem::temp_directory_path() / "pf-gui-e2e";
    std::filesystem::remove_all(dir);

    ProjectController controller;

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::cout << (ok ? "[ OK  ] " : "[FAIL] ") << what << "\n";
        if (!ok) ++failures;
    };

    auto finished = [&](int code) {
        std::filesystem::remove_all(dir);
        QCoreApplication::exit(code);
    };

    QTimer::singleShot(0, [&]() {
        // 1. New project
        check(controller.NewProject(QString(dir.string().c_str())),
              "new project");
        controller.StartAutosave();

        // 2. Edit exactly as the UI would
        check(controller.SetTitle("GUI E2E Paper").status ==
                  pf::EditStatus::Applied, "set title");
        check(controller.SetAbstract("Written through the GUI controller.")
                  .status == pf::EditStatus::Applied, "set abstract");
        auto sec = controller.InsertSection("Introduction");
        check(sec.status == pf::EditStatus::Applied, "insert section");
        auto paragraph_result = controller.InsertParagraph(
            sec.created_node, "Paragraph inserted by GUI smoke test.");
        check(paragraph_result.status == pf::EditStatus::Applied,
              "insert paragraph");
        auto inserted_after = controller.InsertParagraphAfter(
            paragraph_result.created_node, "Inserted at the visual anchor.");
        check(inserted_after.status == pf::EditStatus::Applied,
              "insert paragraph after block");
        check(controller
                  .InsertEquation(sec.created_node, "e^{i\\pi} + 1 = 0", true)
                  .status == pf::EditStatus::Applied, "insert equation");
        // 2b. Author metadata (new flow-editor path)
        check(controller.SetAuthorsText("Alice · Bob, Carol").status ==
                  pf::EditStatus::Applied, "set authors (flow row)");
        check(controller.SetAffiliationsText("University One; Institute Two")
                  .status == pf::EditStatus::Applied,
              "set affiliations (flow row)");
        check(controller.SetKeywordsText("gui, e2e, latex").status ==
                  pf::EditStatus::Applied, "set keywords (flow row)");
        {
            const auto& fm =
                controller.session().state().document().front_matter();
            check(fm.authors.size() == 3, "authors parsed (3)");
            check(fm.affiliations.size() == 2, "affiliations parsed (2)");
            check(fm.keywords.size() == 3, "keywords parsed (3)");
        }
        check(controller.ImportBibliographyText(QString(
                  "@article{gui2024, author={G. User}, title={GUI Paper}, "
                  "year={2024}}")) == true, "import bibliography");
        // Citations target a Paragraph block (find the one we inserted).
        pf::NodeId paragraph;
        {
            const auto& blocks = controller.session()
                                     .state()
                                     .document()
                                     .body()
                                     .sections[0]
                                     .blocks;
            for (const auto& block : blocks) {
                if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                    paragraph = para->id;
                    break;
                }
            }
        }
        check(!paragraph.empty(), "found paragraph for citation");
        check(controller.InsertCitation(paragraph, {"gui2024"})
                  .status == pf::EditStatus::Applied, "insert citation");
        check(controller.InsertCrossReference(paragraph, sec.created_node)
                  .status == pf::EditStatus::Applied,
              "insert cross reference");
        check(controller.EditParagraph(
                  paragraph,
                  "Edited [cite:gui2024] near [ref:" +
                      QString::fromStdString(sec.created_node.value()) + "].")
                  .status == pf::EditStatus::Applied,
              "edit paragraph without flattening inline references");
        {
            const auto& blocks = controller.session()
                                     .state()
                                     .document()
                                     .body()
                                     .sections[0]
                                     .blocks;
            const auto* edited = std::get_if<pf::Paragraph>(&blocks[0]);
            check(edited != nullptr && edited->content.size() == 5,
                  "citation and cross reference remain semantic nodes");
        }
        check(controller.InsertTableAfter(paragraph).status ==
                  pf::EditStatus::Applied,
              "insert table after visual anchor");

        // 3. Undo/redo through the GUI path
        controller.Undo();
        controller.Redo();

        // 4. Build and wait for the result signal
        QObject::connect(&controller, &ProjectController::buildFinished,
                         [&](bool success, const QString& pdf_path) {
                             check(success, "build via GUI controller");
                             check(std::filesystem::exists(
                                       pdf_path.toStdString()),
                                   "pdf exists");
                             finished(failures == 0 ? 0 : 1);
                         });
        controller.RequestBuild(true);
    });

    // Global timeout
    QTimer::singleShot(240000, [&]() {
        std::cout << "[FAIL] timeout\n";
        finished(1);
    });

    return app.exec();
}
