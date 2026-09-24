// GUI 端到端冒烟测试：直接驱动控制器（无需显示器）——新建项目、沿 UI
// 所用的同一条代码路径编辑、build，并校验 PDF 是否存在。可在任意 QPA
// 平台上运行。
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
        if (!ok)
            ++failures;
    };

    auto finished = [&](int code) {
        std::filesystem::remove_all(dir);
        QCoreApplication::exit(code);
    };

    QTimer::singleShot(0, [&]() {
        // 1. 新建项目
        check(controller.NewProject(QString(dir.string().c_str())), "new project");
        controller.StartAutosave();

        // 2. 完全按照 UI 的方式编辑
        check(controller.SetTitle("GUI E2E Paper").status == pf::EditStatus::Applied, "set title");
        check(controller.SetAbstract("Written through the GUI controller.").status == pf::EditStatus::Applied,
              "set abstract");
        auto sec = controller.InsertSection("Introduction");
        check(sec.status == pf::EditStatus::Applied, "insert section");
        auto paragraph_result = controller.InsertParagraph(sec.created_node, "Paragraph inserted by GUI smoke test.");
        check(paragraph_result.status == pf::EditStatus::Applied, "insert paragraph");
        auto inserted_after =
            controller.InsertParagraphAfter(paragraph_result.created_node, "Inserted at the visual anchor.");
        check(inserted_after.status == pf::EditStatus::Applied, "insert paragraph after block");
        check(controller.InsertEquation(sec.created_node, "e^{i\\pi} + 1 = 0", true).status == pf::EditStatus::Applied,
              "insert equation");
        // 2b. 作者元数据（新的 flow-editor 路径）
        check(controller.SetAuthorsText("Alice · Bob, Carol").status == pf::EditStatus::Applied,
              "set authors (flow row)");
        check(controller.SetAffiliationsText("University One; Institute Two").status == pf::EditStatus::Applied,
              "set affiliations (flow row)");
        check(controller.SetKeywordsText("gui, e2e, latex").status == pf::EditStatus::Applied,
              "set keywords (flow row)");
        {
            const auto& fm = controller.session().state().document().front_matter();
            check(fm.authors.size() == 3, "authors parsed (3)");
            check(fm.affiliations.size() == 2, "affiliations parsed (2)");
            check(fm.keywords.size() == 3, "keywords parsed (3)");
        }

        // 作者行中的上标标记把作者绑定到该编号对应的机构；
        // 该标记会从姓名中剥离。
        check(controller.SetAuthorsText(QString::fromUtf8("Alice\u00b9 \u00b7 Bob\u00b2, Carol")).status ==
                  pf::EditStatus::Applied,
              "set authors with institution markers");
        {
            const auto& fm = controller.session().state().document().front_matter();
            check(fm.authors.size() == 3, "marked authors parsed (3)");
            check(fm.authors[0].affiliations.size() == 1 && fm.authors[0].affiliations[0] == fm.affiliations[0].id,
                  "author 1 bound to institution 1");
            check(fm.authors[1].affiliations.size() == 1 && fm.authors[1].affiliations[0] == fm.affiliations[1].id,
                  "author 2 bound to institution 2");
            check(fm.authors[2].affiliations.empty(), "an unmarked author has no institution link");
            check(fm.authors[0].name == "Alice", "the marker is removed from the author name");
        }
        // 必须支持多个机构：此前该行只保留第一个。
        check(controller
                      .SetAffiliationsText("School of Computing, University One\n"
                                           "Institute of Optics, Institute Two\n"
                                           "National Key Lab, Institute Three")
                      .status == pf::EditStatus::Applied,
              "three institutions accepted");
        {
            const auto& fm = controller.session().state().document().front_matter();
            check(fm.affiliations.size() == 3, "affiliations parsed (3)");
            check(fm.affiliations[0].name == "School of Computing, University One", "first institution kept");
            check(fm.affiliations[2].name == "National Key Lab, Institute Three", "third institution kept");
            // 编辑该列表会保持 id 稳定，因此作者关联不会丢失。
            check(fm.authors[0].affiliations.size() == 1 && fm.authors[0].affiliations[0] == fm.affiliations[0].id,
                  "institution ids are stable across edits");
        }
        check(controller
                      .ImportBibliographyText(QString("@article{gui2024, author={G. User}, title={GUI Paper}, "
                                                      "year={2024}}"))
                      .status == pf::BibliographyImportResult::Status::Ok,
              "import bibliography");
        // 引用指向 Paragraph 块（找到我们插入的那个）。
        pf::NodeId paragraph;
        {
            const auto& blocks = controller.session().state().document().body().sections[0].blocks;
            for (const auto& block : blocks) {
                if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                    paragraph = para->id;
                    break;
                }
            }
        }
        check(!paragraph.empty(), "found paragraph for citation");
        // 引用方案 §5：正文文本唯一的路径是 rich commit。控件层的插入
        // （InlineEditor::InsertCitationObject）由 paperforge-inline-editor-test
        // 覆盖；这里（没有 QApplication）用编辑器本会生成的 InlineContent
        // 驱动同一个 commit。
        {
            pf::InlineContent content;
            content.push_back(pf::TextRun{"Prior work ", 0});
            pf::Citation cit;
            cit.keys = {"gui2024"};
            content.push_back(std::move(cit));
            content.push_back(pf::TextRun{" near ", 0});
            content.push_back(pf::CrossReference{sec.created_node});
            content.push_back(pf::TextRun{".", 0});
            check(controller.EditParagraphRich(paragraph, content).status == pf::EditStatus::Applied,
                  "rich paragraph commit accepted");
        }
        {
            const auto& blocks = controller.session().state().document().body().sections[0].blocks;
            const auto* edited = std::get_if<pf::Paragraph>(&blocks[0]);
            check(edited != nullptr && edited->content.size() == 5,
                  "citation and cross reference remain semantic nodes");
        }
        check(controller.InsertTableAfter(paragraph).status == pf::EditStatus::Applied,
              "insert table after visual anchor");

        // 3. 经由 GUI 路径的撤销/重做
        controller.Undo();
        controller.Redo();

        // 4b. 重排序与子节放置。
        {
            auto move_sec = controller.InsertSection("MoveTest");
            auto p1 = controller.InsertParagraph(move_sec.created_node, "one");
            auto p2 = controller.InsertParagraph(move_sec.created_node, "two");
            auto p3 = controller.InsertParagraph(move_sec.created_node, "three");
            auto order = [&]() {
                QStringList texts;
                const auto& doc = controller.session().state().document();
                for (const auto& s : doc.body().sections) {
                    if (s.id != move_sec.created_node)
                        continue;
                    for (const auto& b : s.blocks) {
                        if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                            texts << QString::fromStdString(pf::InlineToPlainText(p->content));
                        }
                    }
                }
                return texts.join(",");
            };
            check(order() == "one,two,three", "paragraphs start in order");

            check(controller.MoveNode(p3.created_node, -1).status == pf::EditStatus::Applied,
                  "move a block up is accepted");
            check(order() == "one,three,two", "block moved up one place");

            check(controller.MoveNode(p3.created_node, +1).status == pf::EditStatus::Applied,
                  "move a block down is accepted");
            check(order() == "one,two,three", "block moved down one place");

            // 插入到第一个段落之后的子节必须落在「one」与「two」之间：
            // 位于其下方的块会成为它的正文。
            check(controller.InsertSubsectionAfter(p1.created_node, "Sub").status == pf::EditStatus::Applied,
                  "insert subsection after a paragraph");
            QStringList flow;
            {
                const auto& doc = controller.session().state().document();
                for (const auto& s : doc.body().sections) {
                    if (s.id != move_sec.created_node)
                        continue;
                    for (const auto& b : s.blocks) {
                        if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                            flow << QString::fromStdString(pf::InlineToPlainText(p->content));
                        }
                    }
                    for (const auto& sub : s.subsections) {
                        flow << ("[" + QString::fromStdString(pf::InlineToPlainText(sub.title)) + "]");
                        for (const auto& b : sub.blocks) {
                            if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                                flow << QString::fromStdString(pf::InlineToPlainText(p->content));
                            }
                        }
                    }
                }
            }
            std::cout << "  flow=" << flow.join(" | ").toStdString() << "\n";
            check(flow.join(",") == "one,[Sub],two,three", "subsection lands where it was inserted");
        }

        // 4. build 并等待类型化的 preview 事件
        QObject::connect(&controller, &ProjectController::previewUpdated, [&](const pf::PreviewUpdate& update) {
            check(update.success, "build via GUI controller");
            check(update.revision == controller.current_revision(), "preview update matches revision");
            check(update.pdf.valid() && std::filesystem::exists(update.pdf.path), "pdf exists");
            finished(failures == 0 ? 0 : 1);
        });
        controller.RequestBuild(true);
    });

    // 全局超时
    QTimer::singleShot(240000, [&]() {
        std::cout << "[FAIL] timeout\n";
        finished(1);
    });

    return app.exec();
}
