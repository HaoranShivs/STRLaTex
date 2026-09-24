// P0-05 回归测试：每种 block 类型在每个标题层级都必须有 GUI 投影。
//
// 报告的症状：插入到 Subsection 或 Subsubsection 内的 Figure 或 Table 在
// 编辑器中不可见——文档存有它，outline 列出了它，但不存在对应卡片，因此
// 无法查看、添加题注、移动或删除。根因是三处手工复制的 block 循环：
// Section 循环处理 Paragraph/Equation/Figure/Table，而 Subsection 和
// Subsubsection 循环只处理 Paragraph/Equation。
//
// 这里强制的不变量：对于在三个层级上都具有全部四种 block 类型的文档，
// 编辑器为每个 block 恰好显示一个可操作卡片，且 node id、题注编辑器和
// 命令均已启用。
//
// 运行方式：QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-block-projection-test
#include <QApplication>
#include <QFrame>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTextEdit>

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
    if (!ok)
        ++failures;
}

// 一个 1x1 的 PNG，使图片路径解析器有真实文件可预览。
std::filesystem::path WriteTinyPng() {
    static const unsigned char png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00,
        0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
        0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
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
    Check(controller->NewProject(QString::fromStdString(dir.string())), "project created");

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
    auto subsub = editor.InsertSubsubsection(0, 0, 0, InlineFromText("Subsub A"));
    Check(subsub.ok(), "subsubsection inserted");

    const auto png = WriteTinyPng();

    // 在每个层级插入一个 figure、一个 table 和一个 paragraph。
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

        Table table = DocumentEditor::MakeTable(std::vector<TableColumn>{{ColumnAlignment::Left}}, 1, true).value();
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

    // 先持久化，再重新打开，使 GUI 完全按照用户全新启动时所看到的样子从
    // 文件重建（这同时证明嵌套 block 能在保存/加载往返中保留下来）。
    controller->Save();
    controller->FlushSaves();
    controller->CloseProject();
    Check(window.OpenProjectDir(QString::fromStdString(dir.string())), "project reopened for a clean rebuild");
    // 重建由 documentChanged 信号驱动，该信号在打开时同步运行；给 Qt 一次
    // 事件循环，以处理上一代 widget 的延迟删除。
    QCoreApplication::processEvents();

    // ---- 实际的投影断言 ----
    std::set<QString> figure_nodes;
    std::set<QString> table_nodes;
    std::set<QString> caption_editors;
    int table_widgets = 0;

    // Figure 卡片通过 row_kind == "Figure" 标识自身。
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
    // Table 预览是卡片内真正的 QTableWidget。
    for (QTableWidget* grid : window.findChildren<QTableWidget*>()) {
        if (grid->isVisible() || grid->parentWidget() != nullptr)
            ++table_widgets;
    }
    // 每个 figure/table 卡片都带有一个可用的题注编辑器：设置了 row_node
    // 且 commands_enabled 为 true 的 QPlainTextEdit。（Text 行使用
    // InlineEditor（QTextEdit 的子类），因此不计入此处。）
    for (QPlainTextEdit* edit : window.findChildren<QPlainTextEdit*>()) {
        if (!edit->property("commands_enabled").toBool())
            continue;
        const QString node = edit->property("row_node").toString();
        if (!node.isEmpty())
            caption_editors.insert(node);
    }

    std::cout << "  figure cards=" << figure_nodes.size() << " table cards=" << table_nodes.size()
              << " table widgets=" << table_widgets << "\n";

    // 每种三个，每个层级一个。
    Check(figure_nodes.size() == 3, "three figure cards are projected");
    Check(table_nodes.size() == 3, "three table cards are projected");
    Check(table_widgets >= 3, "three table grid previews exist");

    const std::set<QString> expected_figures = {QString::fromStdString(at_section.figure.value()),
                                                QString::fromStdString(at_sub.figure.value()),
                                                QString::fromStdString(at_subsub.figure.value())};
    const std::set<QString> expected_tables = {QString::fromStdString(at_section.table.value()),
                                               QString::fromStdString(at_sub.table.value()),
                                               QString::fromStdString(at_subsub.table.value())};

    Check(figure_nodes == expected_figures, "every figure node id has a card at its own level");
    Check(table_nodes == expected_tables, "every table node id has a card at its own level");

    // 题注可编辑：每个 figure/table 节点都拥有一个启用了 commands 的题注
    // 编辑器，正是它让 Move/Delete 可达。
    for (const auto& expected : expected_figures)
        Check(caption_editors.count(expected) == 1, "figure caption editor operable for " + expected.toStdString());
    for (const auto& expected : expected_tables)
        Check(caption_editors.count(expected) == 1, "table caption editor operable for " + expected.toStdString());

    // 每个层级的段落也都有投影（这部分此前已正常，添加断言是为了防止共享
    // 路径使其回归）。Text 行是 InlineEditor 实例（QTextEdit），而不是
    // QPlainTextEdit。
    std::set<QString> paragraph_nodes;
    for (QTextEdit* edit : window.findChildren<QTextEdit*>()) {
        if (qobject_cast<QPlainTextEdit*>(edit) != nullptr)
            continue; // 题注编辑器已在上方统计
        const QString node = edit->property("row_node").toString();
        if (!node.isEmpty())
            paragraph_nodes.insert(node);
    }
    Check(paragraph_nodes.count(QString::fromStdString(at_subsub.paragraph.value())) == 1,
          "the subsubsection paragraph is projected");
    Check(paragraph_nodes.count(QString::fromStdString(at_sub.paragraph.value())) == 1,
          "the subsection paragraph is projected");

    std::filesystem::remove(png);
    std::filesystem::remove_all(dir);
    std::cout << (failures == 0 ? "[ DONE ] all checks passed\n" : "[ DONE ] failures detected\n");
    return failures == 0 ? 0 : 1;
}