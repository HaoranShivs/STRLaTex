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
  const std::string &tex = result.package.files[0].content;
  PF_CHECK(tex.find("\\documentclass[11pt,a4paper]{article}") !=
           std::string::npos);
  PF_CHECK(tex.find("\\end{document}") != std::string::npos);
}

PF_TEST(RendererEscapesLatexSpecials) {
  Document doc = MakeDoc();
  LatexRenderer renderer;
  RenderRequest req;
  req.document = &doc;
  req.template_id = "generic-article";
  auto result = renderer.Render(req);
  const std::string &tex = result.package.files[0].content;
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
  const std::string &tex = result.package.files[0].content;
  PF_CHECK(tex.find("\\textbf{bold}") != std::string::npos);
  PF_CHECK(tex.find("\\begin{equation}") != std::string::npos);
  PF_CHECK(tex.find("E = mc^2") != std::string::npos);
}

PF_TEST(RendererSourceMapCoversEquation) {
  Document doc = MakeDoc();
  const Document &cdoc = doc;
  LatexRenderer renderer;
  RenderRequest req;
  req.document = &doc;
  req.template_id = "generic-article";
  auto result = renderer.Render(req);

  // Count lines to find the equation in the tex, then resolve.
  const std::string &tex = result.package.files[0].content;
  size_t pos = tex.find("\\begin{equation}");
  PF_CHECK(pos != std::string::npos);
  std::uint32_t line = 1;
  for (size_t i = 0; i < pos; ++i) {
    if (tex[i] == '\n')
      ++line;
  }
  auto node = result.source_map.Resolve(line);
  PF_CHECK(node.has_value());
  // The resolved node must be the equation node (second block).
  PF_CHECK(cdoc.body().sections[0].blocks[1].index() == 3); // EquationBlock
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
  for (const auto &f : result.package.files) {
    if (f.path == "references.bib")
      has_bib = true;
  }
  PF_CHECK(has_bib);
  // files[0] is main.tex (bib file appended after it)
  std::string tex;
  for (const auto &f : result.package.files) {
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
  const std::string &tex = result.package.files[0].content;
  PF_CHECK(tex.find("\\citep{k1,k2}") != std::string::npos);
}

// The figure's single-/double-column attribute is the only thing that decides
// which float environment is emitted: `figure` stays inside one column,
// `figure*` spans both. Both images resolve through the assets/ package path.
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
  const std::string &tex = result.package.files[0].content;

  // Single column: the ordinary float.
  PF_CHECK(tex.find("\\begin{figure}[htbp]") != std::string::npos);
  PF_CHECK(tex.find("\\end{figure}\n") != std::string::npos);
  // Double column: the starred float, opened and closed.
  PF_CHECK(tex.find("\\begin{figure*}[htbp]") != std::string::npos);
  PF_CHECK(tex.find("\\end{figure*}\n") != std::string::npos);
  // The starred float must not accidentally be closed by `\end{figure}`.
  PF_CHECK(tex.find("\\end{figure}\n\n\\begin{figure*}") != std::string::npos);
  // Both images are referenced by their real file inside assets/.
  PF_CHECK(tex.find("{assets/figure_one.png}") != std::string::npos);
  PF_CHECK(tex.find("{assets/figure_two.png}") != std::string::npos);
}

// A double-column figure in a template that has only one column must still
// produce a complete, matched float: `figure*` is valid in a one-column class
// and behaves like `figure`. That is what lets the attribute be stored before a
// two-column template is chosen.
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
  const std::string &tex = result.package.files[0].content;

  auto count = [&tex](const std::string &needle) {
    std::size_t n = 0;
    for (std::size_t at = tex.find(needle); at != std::string::npos;
         at = tex.find(needle, at + 1)) {
      ++n;
    }
    return n;
  };
  // The float opens and closes exactly once, with the matching name: an
  // unbalanced environment would abort the build in a one-column template.
  PF_CHECK_EQ(count("\\begin{figure*}"), std::size_t{1});
  PF_CHECK_EQ(count("\\end{figure*}"), std::size_t{1});
  PF_CHECK_EQ(count("\\begin{figure}"), std::size_t{0});
  PF_CHECK_EQ(count("\\end{figure}"), std::size_t{0});
  PF_CHECK(tex.find("\\end{document}") != std::string::npos);
}
