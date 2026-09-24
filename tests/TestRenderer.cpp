// Renderer + SourceMap 测试。
#include "TestMain.hpp"

#include "document/InlineText.h"
#include <fstream>

#include "document/DocumentEditor.h"
#include "render/LatexRenderer.h"

using namespace pf;

namespace {

Document MakeDoc() {
    Document doc;
    DocumentEditor editor(doc);
    editor.SetTitle(InlineFromText("Sample & Title"));
    auto s = editor.InsertSection(0, InlineFromText("Intro_1"));

    Paragraph p;
    TextRun bold;
    bold.text = "bold";
    SetMark(bold.marks, TextMark::Strong, true);
    p.content.push_back(TextRun{"plain ", 0});
    p.content.push_back(bold);
    editor.InsertBlock(s.value(), std::nullopt, p);

    EquationBlock eq;
    eq.expression.latex = "E = mc^2";
    eq.numbered = true;
    editor.InsertBlock(s.value(), std::nullopt, eq);

    return doc;
}

} // namespace

PF_TEST(RendererProducesDocumentClass) {
    Document doc = MakeDoc();
    LatexRenderer renderer;
    RenderRequest req;
    req.build_id = "b1";
    req.snapshot_id = "snap1";
    req.revision = ProjectRevision{7};
    req.document = &doc;
    req.template_id = "generic-article";
    auto result = renderer.Render(req);

    PF_CHECK(result.status == RenderResult::Status::Ok);
    PF_CHECK(result.package.files.size() >= 1);
    const std::string& tex = result.package.files[0].content;
    PF_CHECK(tex.find("\\documentclass[11pt,a4paper]{article}") != std::string::npos);
    PF_CHECK(tex.find("\\end{document}") != std::string::npos);
}

PF_TEST(RendererEscapesLatexSpecials) {
    Document doc = MakeDoc();
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    auto result = renderer.Render(req);
    const std::string& tex = result.package.files[0].content;
    // 标题中的「&」必须转义；节名中的「_」必须转义。
    PF_CHECK(tex.find("Sample \\& Title") != std::string::npos);
    PF_CHECK(tex.find("Intro\\_1") != std::string::npos);
    PF_CHECK(tex.find("\\section{Intro\\_1}") != std::string::npos);
}

PF_TEST(RendererMarksAndEquation) {
    Document doc = MakeDoc();
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    auto result = renderer.Render(req);
    const std::string& tex = result.package.files[0].content;
    PF_CHECK(tex.find("\\textbf{bold}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{equation}") != std::string::npos);
    PF_CHECK(tex.find("E = mc^2") != std::string::npos);
}

PF_TEST(RendererSourceMapCoversEquation) {
    Document doc = MakeDoc();
    const Document& cdoc = doc;
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    auto result = renderer.Render(req);

    // 统计行数以在 tex 中定位该公式，然后解析。
    const std::string& tex = result.package.files[0].content;
    size_t pos = tex.find("\\begin{equation}");
    PF_CHECK(pos != std::string::npos);
    std::uint32_t line = 1;
    for (size_t i = 0; i < pos; ++i) {
        if (tex[i] == '\n')
            ++line;
    }
    auto node = result.source_map.Resolve(line);
    PF_CHECK(node.has_value());
    // 解析出的节点必须是公式节点（第二个 block）。
    PF_CHECK(cdoc.body().sections[0].blocks[1].index() == 3); // EquationBlock
    PF_CHECK(std::get<EquationBlock>(cdoc.body().sections[0].blocks[1]).id == *node);
}

PF_TEST(RendererRejectsUnknownTemplate) {
    Document doc = MakeDoc();
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "no-such-template";
    auto result = renderer.Render(req);
    PF_CHECK(result.status == RenderResult::Status::Failed);
    PF_CHECK(!result.diagnostics.empty());
}

PF_TEST(RendererBibliographyPackaging) {
    Document doc = MakeDoc();
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "ieee-conference";
    req.bibliography_bibtex = "@article{k1, title={T}}";
    auto result = renderer.Render(req);
    bool has_bib = false;
    for (const auto& f : result.package.files) {
        if (f.path == "references.bib")
            has_bib = true;
    }
    PF_CHECK(has_bib);
    // files[0] 是 main.tex（bib 文件追加在其后）
    std::string tex;
    for (const auto& f : result.package.files) {
        if (f.path == "main.tex")
            tex = f.content;
    }
    PF_CHECK(!tex.empty());
    PF_CHECK(tex.find("\\bibliography{references}") != std::string::npos);
    PF_CHECK(tex.find("\\bibliographystyle") != std::string::npos);
}

PF_TEST(RendererCitationsAndCrossRefs) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    Citation cit;
    cit.keys = {"k1", "k2"};
    cit.mode = CitationMode::Parenthetical;
    p.content.push_back(cit);
    editor.InsertBlock(s.value(), std::nullopt, p);

    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    auto result = renderer.Render(req);
    const std::string& tex = result.package.files[0].content;
    PF_CHECK(tex.find("\\citep{k1,k2}") != std::string::npos);
}

// 图的单栏/双栏属性是决定输出哪个 float 环境的唯一依据：`figure` 保持
// 在单栏内，`figure*` 横跨两栏。两张图片都通过 assets/ 包路径解析。
PF_TEST(RendererFigureSpanChoosesFloatEnvironment) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("Intro"));

    Figure single;
    single.asset_id = AssetId("a1");
    single.caption = InlineFromText("narrow");
    single.span = FigureSpan::SingleColumn;
    editor.InsertBlock(s.value(), std::nullopt, single);

    Figure wide;
    wide.asset_id = AssetId("a2");
    wide.caption = InlineFromText("wide");
    wide.span = FigureSpan::DoubleColumn;
    editor.InsertBlock(s.value(), std::nullopt, wide);

    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    req.asset_files["a1"] = "figure_one.png";
    req.asset_files["a2"] = "figure_two.png";
    auto result = renderer.Render(req);
    PF_CHECK(result.status == RenderResult::Status::Ok);
    const std::string& tex = result.package.files[0].content;

    // 单栏：普通的 float。
    PF_CHECK(tex.find("\\begin{figure}[htbp]") != std::string::npos);
    PF_CHECK(tex.find("\\end{figure}\n") != std::string::npos);
    // 双栏：带星号的 float，成对开闭。
    PF_CHECK(tex.find("\\begin{figure*}[htbp]") != std::string::npos);
    PF_CHECK(tex.find("\\end{figure*}\n") != std::string::npos);
    // 带星号的 float 不得被 `\end{figure}` 误关闭。
    PF_CHECK(tex.find("\\end{figure}\n\n\\begin{figure*}") != std::string::npos);
    // 两张图片都通过其在 assets/ 内的真实文件引用。
    PF_CHECK(tex.find("{assets/figure_one.png}") != std::string::npos);
    PF_CHECK(tex.find("{assets/figure_two.png}") != std::string::npos);
}

// 在只有单栏的模板中，双栏图仍必须生成完整且配对的 float：`figure*`
// 在单栏文档类中合法，行为与 `figure` 相同。正因如此，该属性可以在
// 选定双栏模板之前就先存储。
PF_TEST(RendererDoubleColumnFigureStaysCompleteInSingleColumnTemplate) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("Intro"));
    Figure wide;
    wide.asset_id = AssetId("a1");
    wide.span = FigureSpan::DoubleColumn;
    editor.InsertBlock(s.value(), std::nullopt, wide);

    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    req.asset_files["a1"] = "figure_one.png";
    auto result = renderer.Render(req);
    PF_CHECK(result.status == RenderResult::Status::Ok);
    const std::string& tex = result.package.files[0].content;

    auto count = [&tex](const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t at = tex.find(needle); at != std::string::npos; at = tex.find(needle, at + 1)) {
            ++n;
        }
        return n;
    };
    // 该 float 以匹配的名称恰好开闭一次：在单栏模板中，
    // 不配对的环境会导致 build 中止。
    PF_CHECK_EQ(count("\\begin{figure*}"), std::size_t{1});
    PF_CHECK_EQ(count("\\end{figure*}"), std::size_t{1});
    PF_CHECK_EQ(count("\\begin{figure}"), std::size_t{0});
    PF_CHECK_EQ(count("\\end{figure}"), std::size_t{0});
    PF_CHECK(tex.find("\\end{document}") != std::string::npos);
}
