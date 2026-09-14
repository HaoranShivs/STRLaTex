// GUI end-to-end smoke test: drive the controller directly (no display
// needed) - new project, edit via the same code paths the UI uses, build,
// and verify the PDF exists. Run under any QPA platform.
#include <QCoreApplication>
#include <QTimer>
#include <chrono>
#include <filesystem>
#include <iostream>

#include "app/ProjectController.h"
#include "document/InlineText.h"

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

        // Superscript markers in the author row bind an author to the
        // institution with that number; the marker is stripped from the name.
        check(controller
                  .SetAuthorsText(QString::fromUtf8(
                      "Alice\u00b9 \u00b7 Bob\u00b2, Carol"))
                  .status == pf::EditStatus::Applied,
              "set authors with institution markers");
        {
            const auto& fm =
                controller.session().state().document().front_matter();
            check(fm.authors.size() == 3, "marked authors parsed (3)");
            check(fm.authors[0].affiliations.size() == 1 &&
                      fm.authors[0].affiliations[0] == fm.affiliations[0].id,
                  "author 1 bound to institution 1");
            check(fm.authors[1].affiliations.size() == 1 &&
                      fm.authors[1].affiliations[0] == fm.affiliations[1].id,
                  "author 2 bound to institution 2");
            check(fm.authors[2].affiliations.empty(),
                  "an unmarked author has no institution link");
            check(fm.authors[0].name == "Alice",
                  "the marker is removed from the author name");
        }
        // More than one institution has to be supported: the row used to keep
        // only the first one.
        check(controller.SetAffiliationsText(
                  "School of Computing, University One\n"
                  "Institute of Optics, Institute Two\n"
                  "National Key Lab, Institute Three")
                  .status == pf::EditStatus::Applied,
              "three institutions accepted");
        {
            const auto& fm =
                controller.session().state().document().front_matter();
            check(fm.affiliations.size() == 3, "affiliations parsed (3)");
            check(fm.affiliations[0].name ==
                      "School of Computing, University One",
                  "first institution kept");
            check(fm.affiliations[2].name ==
                      "National Key Lab, Institute Three",
                  "third institution kept");
            // Editing the list keeps ids stable, so author links survive.
            check(fm.authors[0].affiliations.size() == 1 &&
                      fm.authors[0].affiliations[0] == fm.affiliations[0].id,
                  "institution ids are stable across edits");
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

        // 4b. Reordering and subsection placement.
        {
            auto move_sec = controller.InsertSection("MoveTest");
            auto p1 = controller.InsertParagraph(move_sec.created_node, "one");
            auto p2 = controller.InsertParagraph(move_sec.created_node, "two");
            auto p3 = controller.InsertParagraph(move_sec.created_node, "three");
            auto order = [&]() {
                QStringList texts;
                const auto& doc = controller.session().state().document();
                for (const auto& s : doc.body().sections) {
                    if (s.id != move_sec.created_node) continue;
                    for (const auto& b : s.blocks) {
                        if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                            texts << QString::fromStdString(
                                pf::InlineToPlainText(p->content));
                        }
                    }
                }
                return texts.join(",");
            };
            check(order() == "one,two,three", "paragraphs start in order");

            check(controller.MoveNode(p3.created_node, -1).status ==
                      pf::EditStatus::Applied, "move a block up is accepted");
            check(order() == "one,three,two", "block moved up one place");

            check(controller.MoveNode(p3.created_node, +1).status ==
                      pf::EditStatus::Applied, "move a block down is accepted");
            check(order() == "one,two,three", "block moved down one place");

            // A subsection inserted after the first paragraph has to land
            // between "one" and "two": the blocks below it become its body.
            check(controller.InsertSubsectionAfter(p1.created_node, "Sub")
                      .status == pf::EditStatus::Applied,
                  "insert subsection after a paragraph");
            QStringList flow;
            {
                const auto& doc = controller.session().state().document();
                for (const auto& s : doc.body().sections) {
                    if (s.id != move_sec.created_node) continue;
                    for (const auto& b : s.blocks) {
                        if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                            flow << QString::fromStdString(
                                pf::InlineToPlainText(p->content));
                        }
                    }
                    for (const auto& sub : s.subsections) {
                        flow << ("[" + QString::fromStdString(
                                           pf::InlineToPlainText(sub.title)) +
                                 "]");
                        for (const auto& b : sub.blocks) {
                            if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                                flow << QString::fromStdString(
                                    pf::InlineToPlainText(p->content));
                            }
                        }
                    }
                }
            }
            std::cout << "  flow=" << flow.join(" | ").toStdString() << "\n";
            check(flow.join(",") == "one,[Sub],two,three",
                  "subsection lands where it was inserted");
        }

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
