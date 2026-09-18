// P0-05 regression test: every block kind must have a GUI projection at every
// heading level.
//
// Reported symptom: a Figure or Table inserted inside a Subsection or a
// Subsubsection was invisible in the editor - the document stored it, the
// outline listed it, but no card existed, so it could not be seen, captioned,
// moved or deleted. Root cause was three hand-copied block loops: the Section
// loop handled Paragraph/Equation/Figure/Table while the Subsection and
// Subsubsection loops only handled Paragraph/Equation.
//
// The invariant enforced here: for a document with all four block kinds at
// all three levels, the editor shows exactly one operable card per block,
// with the node id, caption editor and commands enabled.
//
// Run with: QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-block-projection-test
#include <QApplication>
#include <QFrame>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTableWidget>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include "app/BlockEditor.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "document/InlineText.h"

using namespace pf;
using namespace pf::gui;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "[ OK  ] " : "[FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

// A 1x1 PNG so the figure path resolver has a real file to preview.
std::filesystem::path WriteTinyPng() {
    static const unsigned char png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
        0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
        0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60,
        0x82};
    auto path = std::filesystem::temp_directory_path() / "pf-projection.png";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png), sizeof(png));
    return path;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    auto dir = std::filesystem::temp_directory_path() / "pf-block-projection";
    std::filesystem::remove_all(dir);

    MainWindow window;
    auto* controller = window.controller();
    Check(controller->NewProject(QString::fromStdString(dir.string())),
          "project created");

    auto& session = controller->session();
    DocumentEditor editor(session.mutable_document());

    // Section
    //   Figure, Table, Paragraph
    //   Subsection
    //     Figure, Table, Paragraph
    //     Subsubsection
    //       Figure, Table, Paragraph
    auto section = editor.InsertSection(0, InlineFromText("Section A"));
    Check(section.ok(), "section inserted");
    auto sub = editor.InsertSubsection(0, 0, InlineFromText("Sub A"));
    Check(sub.ok(), "subsection inserted");
    auto subsub =
        editor.InsertSubsubsection(0, 0, 0, InlineFromText("Subsub A"));
    Check(subsub.ok(), "subsubsection inserted");

    const auto png = WriteTinyPng();

    // Insert one figure, one table and one paragraph at each level.
    struct Placed {
        NodeId figure;
        NodeId table;
        NodeId paragraph;
    };
    auto place = [&](const NodeId& parent) -> Placed {
        Placed placed;

        auto figure = session.InsertFigureFromSource(png, parent, std::nullopt);
        Check(figure.status == EditStatus::Applied, "figure inserted");
        placed.figure = NodeId(figure.created_node);

        Table table = DocumentEditor::MakeTable(
                          std::vector<TableColumn>{{ColumnAlignment::Left}},
                          1, true)
                          .value();
        auto table_result = editor.InsertBlock(parent, std::nullopt, table);
        Check(table_result.ok(), "table inserted");
        placed.table = table_result.value();

        Paragraph para;
        para.content = InlineFromText("body");
        auto para_result = editor.InsertBlock(parent, std::nullopt, para);
        Check(para_result.ok(), "paragraph inserted");
        placed.paragraph = para_result.value();
        return placed;
    };

    const Placed at_section = place(section.value());
    const Placed at_sub = place(sub.value());
    const Placed at_subsub = place(subsub.value());

    // Persist, then reopen so the GUI is rebuilt from the file exactly as a
    // user would see it on a fresh start (this also proves the nested blocks
    // survive the save/load round trip).
    controller->Save();
    controller->FlushSaves();
    controller->CloseProject();
    Check(window.OpenProjectDir(QString::fromStdString(dir.string())),
          "project reopened for a clean rebuild");
    // The rebuild is driven by the documentChanged signal, which runs
    // synchronously on open; give Qt one event pass for the deferred deletes
    // of the previous widget generation.
    QCoreApplication::processEvents();

    // ---- The actual projection assertions ----
    std::set<QString> figure_nodes;
    std::set<QString> table_nodes;
    std::set<QString> caption_editors;
    int table_widgets = 0;

    // Figure cards announce themselves with row_kind == "Figure".
    for (QFrame* frame : window.findChildren<QFrame*>()) {
        const QString kind = frame->property("row_kind").toString();
        if (kind == "Figure" || kind == "Table") {
            const QString node = frame->property("row_node").toString();
            if (kind == "Figure")
                figure_nodes.insert(node);
            else
                table_nodes.insert(node);
        }
    }
    // Table previews are real QTableWidgets inside their card.
    for (QTableWidget* grid : window.findChildren<QTableWidget*>()) {
        if (grid->isVisible() || grid->parentWidget() != nullptr)
            ++table_widgets;
    }
    // Every figure/table card carries an operable caption editor: a
    // QPlainTextEdit with row_node set and commands_enabled true. (Text rows
    // use InlineEditor, a QTextEdit subclass, so they are not counted here.)
    for (QPlainTextEdit* edit : window.findChildren<QPlainTextEdit*>()) {
        if (!edit->property("commands_enabled").toBool())
            continue;
        const QString node = edit->property("row_node").toString();
        if (!node.isEmpty())
            caption_editors.insert(node);
    }

    std::cout << "  figure cards=" << figure_nodes.size()
              << " table cards=" << table_nodes.size()
              << " table widgets=" << table_widgets << "\n";

    // Three of each, one per level.
    Check(figure_nodes.size() == 3, "three figure cards are projected");
    Check(table_nodes.size() == 3, "three table cards are projected");
    Check(table_widgets >= 3, "three table grid previews exist");

    const std::set<QString> expected_figures = {
        QString::fromStdString(at_section.figure.value()),
        QString::fromStdString(at_sub.figure.value()),
        QString::fromStdString(at_subsub.figure.value())};
    const std::set<QString> expected_tables = {
        QString::fromStdString(at_section.table.value()),
        QString::fromStdString(at_sub.table.value()),
        QString::fromStdString(at_subsub.table.value())};

    Check(figure_nodes == expected_figures,
          "every figure node id has a card at its own level");
    Check(table_nodes == expected_tables,
          "every table node id has a card at its own level");

    // Captions are editable: each figure/table node owns a caption editor with
    // commands enabled, which is what makes Move/Delete reachable.
    for (const auto& expected : expected_figures)
        Check(caption_editors.count(expected) == 1,
              "figure caption editor operable for " + expected.toStdString());
    for (const auto& expected : expected_tables)
        Check(caption_editors.count(expected) == 1,
              "table caption editor operable for " + expected.toStdString());

    // The paragraphs at every level are projected too (this part worked before,
    // asserted so the shared path does not regress it). Text rows are
    // InlineEditor instances (QTextEdit), not QPlainTextEdit.
    std::set<QString> paragraph_nodes;
    for (QTextEdit* edit : window.findChildren<QTextEdit*>()) {
        if (qobject_cast<QPlainTextEdit*>(edit) != nullptr)
            continue;  // caption editors were counted above
        const QString node = edit->property("row_node").toString();
        if (!node.isEmpty())
            paragraph_nodes.insert(node);
    }
    Check(paragraph_nodes.count(
              QString::fromStdString(at_subsub.paragraph.value())) == 1,
          "the subsubsection paragraph is projected");
    Check(paragraph_nodes.count(
              QString::fromStdString(at_sub.paragraph.value())) == 1,
          "the subsection paragraph is projected");

    std::filesystem::remove(png);
    std::filesystem::remove_all(dir);
    std::cout << (failures == 0 ? "[ DONE ] all checks passed\n"
                                : "[ DONE ] failures detected\n");
    return failures == 0 ? 0 : 1;
}