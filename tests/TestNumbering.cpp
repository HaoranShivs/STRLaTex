// WP-03 编号与 label 目录的单元测试。
// 这些值对象同时被 GUI（picker/胶囊/卡片标题）、Renderer 与 Validator
// 消费，因此必须独立于 Qt 和 build 流程进行验证。
#include "TestMain.hpp"

#include <string>
#include <vector>

#include "document/DocumentEditor.h"
#include "document/InlineText.h"
#include "numbering/DocumentNumberer.h"
#include "numbering/LabelRegistry.h"
#include "render/LatexRenderer.h"
#include "template/TemplateRegistry.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

NodeId AddBlock(DocumentEditor& editor, const NodeId& parent, Block block) {
    auto result = editor.InsertBlock(parent, std::nullopt, std::move(block));
    return result.ok() ? result.value() : NodeId{};
}

Figure MakeFigure(NodeId id = NodeId{}) {
    Figure figure;
    figure.id = std::move(id);
    figure.asset_id = AssetId("asset-1");
    figure.caption = InlineFromText("System overview");
    return figure;
}

Table MakeTable() {
    Table table;
    table.columns = {TableColumn{ColumnAlignment::Left}};
    table.cells = {{{TableCell{InlineFromText("value")}}}};
    table.caption = InlineFromText("Dataset statistics");
    return table;
}

EquationBlock MakeEquation(bool numbered, std::string label = {}) {
    EquationBlock equation;
    equation.expression.latex = "x^2";
    equation.numbered = numbered;
    equation.label = std::move(label);
    return equation;
}

} // namespace

PF_TEST(DocumentNumbererNumbersHeadingsFloatsAndEquations) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("Introduction"));
    PF_CHECK(section.ok());
    auto subsection = editor.InsertSubsection(0, 0, InlineFromText("Background"));
    PF_CHECK(subsection.ok());
    auto subsubsection = editor.InsertSubsubsection(0, 0, 0, InlineFromText("Details"));
    PF_CHECK(subsubsection.ok());

    // 文档顺序：section blocks -> subsection blocks -> subsubsection blocks。
    auto figure1 = AddBlock(editor, section.value(), MakeFigure());
    auto table1 = AddBlock(editor, section.value(), MakeTable());
    auto equation1 = AddBlock(editor, section.value(), MakeEquation(true, "eq:first"));
    auto figure2 = AddBlock(editor, subsection.value(), MakeFigure());
    auto unnumbered = AddBlock(editor, subsection.value(), MakeEquation(false, "eq:ignored"));
    auto equation2 = AddBlock(editor, subsubsection.value(), MakeEquation(true));

    const auto numbering = DocumentNumberer::Build(doc);
    PF_CHECK(numbering.Find(section.value())->display == "Section 1");
    PF_CHECK(numbering.Find(subsection.value())->display == "Section 1.1");
    PF_CHECK(numbering.Find(subsubsection.value())->display == "Section 1.1.1");
    PF_CHECK(numbering.Find(figure1)->display == "Figure 1");
    PF_CHECK(numbering.Find(figure2)->display == "Figure 2");
    PF_CHECK(numbering.Find(table1)->display == "Table 1");
    PF_CHECK(numbering.Find(equation1)->display == "Eq. (1)");
    PF_CHECK(numbering.Find(equation2)->display == "Eq. (2)");
    // 未编号公式不占号、不进入交叉引用目录。
    PF_CHECK(!numbering.Find(unnumbered).has_value());
    PF_CHECK(numbering.DisplayFor(unnumbered).empty());
    PF_CHECK(numbering.Find(equation1)->latex_label == "eq:first");
    PF_CHECK(numbering.Find(figure1)->latex_label == figure1.value());
    PF_CHECK(numbering.Find(table1)->latex_label == table1.value());

    const auto references = CollectCrossReferences(doc, numbering);
    bool saw_unnumbered = false;
    bool saw_figure = false;
    bool saw_table = false;
    bool saw_equation = false;
    for (const auto& item : references) {
        if (item.node == unnumbered)
            saw_unnumbered = true;
        if (item.node == figure1) {
            saw_figure = true;
            PF_CHECK(item.display == "Figure 1");
            PF_CHECK(item.title == "System overview");
        }
        if (item.node == table1) {
            saw_table = true;
            PF_CHECK(item.display == "Table 1");
            PF_CHECK(item.title == "Dataset statistics");
        }
        if (item.node == equation1) {
            saw_equation = true;
            PF_CHECK(item.display == "Eq. (1)");
        }
    }
    PF_CHECK(!saw_unnumbered);
    PF_CHECK(saw_figure && saw_table && saw_equation);
}

PF_TEST(DocumentNumbererResetsSubsectionCounterPerSection) {
    Document doc;
    DocumentEditor editor(doc);
    auto first = editor.InsertSection(0, InlineFromText("One"));
    auto first_sub = editor.InsertSubsection(0, 0, InlineFromText("One.A"));
    auto second = editor.InsertSection(1, InlineFromText("Two"));
    auto second_sub = editor.InsertSubsection(1, 0, InlineFromText("Two.A"));
    PF_CHECK(first.ok() && first_sub.ok() && second.ok() && second_sub.ok());

    const auto numbering = DocumentNumberer::Build(doc);
    PF_CHECK(numbering.Find(first.value())->display == "Section 1");
    PF_CHECK(numbering.Find(first_sub.value())->display == "Section 1.1");
    PF_CHECK(numbering.Find(second.value())->display == "Section 2");
    PF_CHECK(numbering.Find(second_sub.value())->display == "Section 2.1");
}

PF_TEST(LabelRegistryDetectsDuplicatesAndIgnoresUnnumberedEquations) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    PF_CHECK(section.ok());

    Figure first_figure = MakeFigure();
    first_figure.label = "fig:shared";
    auto figure1 = AddBlock(editor, section.value(), first_figure);
    Table table = MakeTable();
    table.label = "fig:shared";
    auto table1 = AddBlock(editor, section.value(), table);
    auto equation1 = AddBlock(editor, section.value(), MakeEquation(true, "fig:shared"));
    // 未编号公式不写 \label，因此即使 label 重复也不构成冲突。
    auto unnumbered = AddBlock(editor, section.value(), MakeEquation(false, "fig:shared"));

    const auto registry = LabelRegistry::Build(doc);
    PF_CHECK(registry.LabelFor(figure1) == "fig:shared");
    PF_CHECK(registry.LabelFor(table1) == "fig:shared");
    PF_CHECK(registry.LabelFor(unnumbered) == unnumbered.value());
    PF_CHECK(registry.conflicts().size() == 2);
    if (registry.conflicts().size() == 2) {
        PF_CHECK(registry.conflicts()[0].label == "fig:shared");
        PF_CHECK(registry.conflicts()[0].first == figure1);
        PF_CHECK(registry.conflicts()[0].second == table1);
        PF_CHECK(registry.conflicts()[1].second == equation1);
    }
    (void)equation1;
}

PF_TEST(LabelRegistrySanitizesAndFallsBackToNodeId) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Figure figure = MakeFigure();
    figure.label = "  fig: my figure  ";
    auto figure_id = AddBlock(editor, section.value(), figure);
    Table table = MakeTable();
    table.label = "!!!";
    auto table_id = AddBlock(editor, section.value(), table);

    const auto registry = LabelRegistry::Build(doc);
    PF_CHECK(registry.LabelFor(figure_id) == "fig:-my-figure");
    // 清洗后为空 -> 回退 node id，绝不生成空的 \label{}。
    PF_CHECK(registry.LabelFor(table_id) == table_id.value());
    PF_CHECK(!registry.has_conflicts());
}

PF_TEST(RendererUsesCustomLabelsForFiguresTablesAndEquations) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("Results"));
    PF_CHECK(section.ok());

    Figure figure = MakeFigure();
    figure.label = "fig:arch";
    auto figure_id = AddBlock(editor, section.value(), figure);
    Table table = MakeTable();
    table.label = "tab:data";
    auto table_id = AddBlock(editor, section.value(), table);
    auto equation_id = AddBlock(editor, section.value(), MakeEquation(true, "eq:main"));

    Paragraph paragraph;
    TextRun run;
    run.text = "See ";
    paragraph.content.push_back(run);
    CrossReference figure_ref;
    figure_ref.target = figure_id;
    paragraph.content.push_back(figure_ref);
    paragraph.content.push_back(TextRun{" and ", 0});
    CrossReference table_ref;
    table_ref.target = table_id;
    paragraph.content.push_back(table_ref);
    paragraph.content.push_back(TextRun{" and ", 0});
    CrossReference equation_ref;
    equation_ref.target = equation_id;
    paragraph.content.push_back(equation_ref);
    AddBlock(editor, section.value(), paragraph);

    LatexRenderer renderer;
    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    const auto result = renderer.Render(request);
    PF_CHECK(result.status == RenderResult::Status::Ok);
    if (result.status != RenderResult::Status::Ok || result.package.files.empty())
        return;
    const std::string& tex = result.package.files[0].content;
    PF_CHECK(tex.find("\\label{fig:arch}") != std::string::npos);
    PF_CHECK(tex.find("\\label{tab:data}") != std::string::npos);
    PF_CHECK(tex.find("\\label{eq:main}") != std::string::npos);
    PF_CHECK(tex.find("\\ref{fig:arch}") != std::string::npos);
    PF_CHECK(tex.find("\\ref{tab:data}") != std::string::npos);
    PF_CHECK(tex.find("\\ref{eq:main}") != std::string::npos);
}

PF_TEST(RendererRejectsDuplicateLabels) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Figure first = MakeFigure();
    first.label = "fig:duplicate";
    Figure second = MakeFigure();
    second.label = "fig:duplicate";
    AddBlock(editor, section.value(), first);
    AddBlock(editor, section.value(), second);

    LatexRenderer renderer;
    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    const auto result = renderer.Render(request);
    PF_CHECK(result.status == RenderResult::Status::Failed);
    bool reported = false;
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.message.find("Duplicate LaTeX label") != std::string::npos)
            reported = true;
    PF_CHECK(reported);
}

PF_TEST(ValidatorReportsDuplicateLabelsAtSecondNode) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Figure first = MakeFigure();
    first.label = "fig:shared";
    Figure second = MakeFigure();
    second.label = "fig:shared";
    Table table = MakeTable();
    table.label = "fig:shared";
    AddBlock(editor, section.value(), first);
    const auto second_id = AddBlock(editor, section.value(), second);
    const auto table_id = AddBlock(editor, section.value(), table);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    const auto result = validator.Validate(input);
    int conflicts = 0;
    NodeId located;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code != "E-DUPLICATE-LABEL")
            continue;
        ++conflicts;
        if (diagnostic.location.node == second_id)
            located = diagnostic.location.node;
    }
    PF_CHECK(conflicts == 2);
    PF_CHECK(located == second_id);
    (void)table_id;
}

PF_TEST(RendererSupportsCleverefTemplateCapability) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("Intro"));
    Paragraph paragraph;
    CrossReference reference;
    reference.target = section.value();
    paragraph.content.push_back(reference);
    AddBlock(editor, section.value(), paragraph);

    auto definition = *TemplateRegistry::Instance().Find("generic-article");
    definition.capabilities.cross_reference = CrossReferenceStyle::Cleveref;
    LatexRenderer renderer;
    RenderRequest request;
    request.document = &doc;
    request.template_id = definition.id;
    request.resolved_template = std::make_shared<const TemplateDefinition>(definition);
    const auto result = renderer.Render(request);
    PF_CHECK(result.status == RenderResult::Status::Ok);
    if (result.status != RenderResult::Status::Ok || result.package.files.empty())
        return;
    const std::string& tex = result.package.files[0].content;
    PF_CHECK(tex.find("\\cref{" + section.value().value() + "}") != std::string::npos);
    const size_t hyperref = tex.find("{hyperref}");
    const size_t cleveref = tex.find("\\usepackage{cleveref}");
    PF_CHECK(hyperref != std::string::npos);
    PF_CHECK(cleveref != std::string::npos);
    PF_CHECK(hyperref < cleveref);
}
