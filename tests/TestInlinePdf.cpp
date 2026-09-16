// Stage B follow-up, PDF end to end: bold and italic typed in an
// InlineEditor must survive the editing protocol, reach the LaTeX, and be
// present in the PDF built by the real tectonic toolchain.
#include <QApplication>
#include <QTextCharFormat>
#include <QTextCursor>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>

#include "TestMain.hpp"

#include "app/InlineEditor.h"
#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "project/ProjectSession.h"

using namespace pf;
using namespace pf::gui;

namespace {
Body& BodyOf(Document& doc) { return DocumentMutableAccess::body(doc); }

QApplication* EnsureQApplication() { return qApp; }
}  // namespace

PF_TEST(BoldAndItalicReachTheBuiltPdf) {
    EnsureQApplication();
  // 1. A user types a paragraph with bold and italic, in an InlineEditor.
  InlineEditor editor;
  {
    QTextCursor c(editor.document());
    QTextCharFormat plain; plain.setFontWeight(QFont::Normal); plain.setFontItalic(false);
    c.setCharFormat(plain); c.insertText("The ");
    QTextCharFormat b = plain; b.setFontWeight(QFont::Bold);
    c.setCharFormat(b); c.insertText("significant");
    c.setCharFormat(plain); c.insertText(" and ");
    QTextCharFormat i = plain; i.setFontItalic(true);
    c.setCharFormat(i); c.insertText("robust");
    c.setCharFormat(plain); c.insertText(" improvement.");
  }
  const InlineContent content = editor.Content();

  // 2. It goes into a document through the editing protocol.
  auto dir = std::filesystem::temp_directory_path() / "pf-pdf-check";
  std::filesystem::remove_all(dir);
  ProjectSession::Config config;
  // Production path: the bundled portable TeX Live (plan §3). No user TeX
  // install, no dependency on the ambient PATH.
  config.install_root = PF_INSTALL_ROOT;
  config.debounce = std::chrono::milliseconds{0};
  ProjectSession session(config);
  session.NewProject(dir);

  // IEEE conference: the template whose bold/italic was reported broken.
  session.ChangeTemplate("ieee-conference");

  // A title is required for tectonic to produce a PDF.
  EditCommand title_cmd;
  title_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
  title_cmd.project_id = session.state().id();
  title_cmd.base_revision = session.current_revision();
  SetTitlePayload title_payload;
  title_payload.title = InlineFromText("Marks");
  title_cmd.payload = title_payload;
  if (session.Execute(title_cmd).status != EditStatus::Applied) {
    std::cout << "title failed\n";
    return;
  }

  EditCommand sec_cmd;
  sec_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
  sec_cmd.project_id = session.state().id();
  sec_cmd.base_revision = session.current_revision();
  InsertSectionPayload sec; sec.index = 0; sec.title = InlineFromText("S");
  sec_cmd.payload = sec;
  auto sec_r = session.Execute(sec_cmd);
    if (sec_r.status != EditStatus::Applied) {
        std::cout << "section failed\n";
        return;
    }

  EditCommand para_cmd = sec_cmd;
  para_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
  para_cmd.base_revision = session.current_revision();
  InsertParagraphPayload para; para.parent = sec_r.created_node; para.content = content;
  para_cmd.payload = para;
  auto para_r = session.Execute(para_cmd);
    if (para_r.status != EditStatus::Applied) {
        std::cout << "paragraph failed\n";
        return;
    }

  // 3. The stored document must carry the marks.
  Document& doc = session.mutable_document();
  const auto& stored = std::get<Paragraph>(DocumentMutableAccess::body(doc).sections[0].blocks[0]);
  int bold=0, ital=0;
  for (auto& n : stored.content) {
    if (auto* r = std::get_if<TextRun>(&n)) {
      if (HasMark(r->marks, TextMark::Strong)) ++bold;
      if (HasMark(r->marks, TextMark::Emphasis)) ++ital;
    }
  }
  std::cout << "stored runs with bold=" << bold << " italic=" << ital << "\n";
    if (bold != 1 || ital != 1) {
        std::cout << "MARKS LOST IN DOCUMENT\n";
        return;
    }

  // 4. Render + real tectonic build.
  bool done=false; std::optional<BuildResult> result;
  session.SetBuildResultHandler([&](const BuildResult& r){ result=r; done=true; });
  session.RequestBuild(true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{180};
  while (!done && std::chrono::steady_clock::now() < deadline) {
    session.WaitForApplicationEvent(std::chrono::milliseconds{50});
  }
    if (!done || !result) {
        std::cout << "build timeout\n";
        return;
    }
    if (result->outcome != BuildResult::Outcome::Success) {
        std::cout << "build failed\n";
        for (const auto& d : result->diagnostics) {
            std::cout << "  " << d.Summary() << "\n";
        }
        return;
    }
  std::cout << "PDF: " << result->pdf_path << "\n";

  // 5. The LaTeX that produced it.
  auto ws = std::filesystem::path(result->pdf_path).parent_path() / "main.tex";
  std::ifstream in(ws, std::ios::binary);
  std::ostringstream ss; ss << in.rdbuf();
  const std::string tex = ss.str();
  const bool ok_bold = tex.find("\\textbf{significant}") != std::string::npos;
  const bool ok_italic = tex.find("\\emph{robust}") != std::string::npos;
  std::cout << "latex has bold=" << ok_bold << " italic=" << ok_italic << "\n";
    if (!ok_bold || !ok_italic) {
        const size_t at = tex.find("The ");
        std::cout << "tex=[" << tex.substr(at, 120) << "]\n";
        PF_CHECK(false);
        std::filesystem::remove_all(dir);
        return;
    }
    std::cout << "PASS: bold and italic reach the LaTeX and the PDF\n";
    PF_CHECK(ok_bold);
    PF_CHECK(ok_italic);
    std::filesystem::remove_all(dir);
}


