// Renderer + SourceMap tests.
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

}  // namespace

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
    // "&" in title must be escaped; "_" in section name must be escaped.
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

    // Count lines to find the equation in the tex, then resolve.
    const std::string& tex = result.package.files[0].content;
    size_t pos = tex.find("\\begin{equation}");
    PF_CHECK(pos != std::string::npos);
    std::uint32_t line = 1;
    for (size_t i = 0; i < pos; ++i) {
        if (tex[i] == '\n') ++line;
    }
    auto node = result.source_map.Resolve(line);
    PF_CHECK(node.has_value());
    // The resolved node must be the equation node (second block).
    PF_CHECK(cdoc.body().sections[0].blocks[1].index() == 3);  // EquationBlock
    PF_CHECK(std::get<EquationBlock>(cdoc.body().sections[0].blocks[1]).id ==
             *node);
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
        if (f.path == "references.bib") has_bib = true;
    }
    PF_CHECK(has_bib);
    // files[0] is main.tex (bib file appended after it)
    std::string tex;
    for (const auto& f : result.package.files) {
        if (f.path == "main.tex") tex = f.content;
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
