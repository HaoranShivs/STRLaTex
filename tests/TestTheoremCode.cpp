// WP-07：定理/证明与代码块。
#include "TestMain.hpp"

#include <string>

#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "numbering/DocumentNumberer.h"
#include "numbering/LabelRegistry.h"
#include "persistence/ProjectPersistence.h"
#include "render/LatexRenderer.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

std::string RenderTex(const Document& doc) {
    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    if (rendered.package.files.empty())
        return {};
    return rendered.package.files[0].content;
}

int CountDiagnostics(const ValidationResult& result, const char* code) {
    int count = 0;
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code)
            ++count;
    return count;
}

} // namespace

PF_TEST(TheoremAndCodeRenderAndNumber) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    auto theorem = editor.InsertTheorem(section.value(), std::nullopt, TheoremKind::Theorem,
                                        InlineFromText("Main result"), InlineFromText("Every A is B."), true,
                                        "thm:main");
    auto lemma = editor.InsertTheorem(section.value(), std::nullopt, TheoremKind::Lemma, {},
                                      InlineFromText("A helper."), true, "lem:helper");
    auto proof = editor.InsertTheorem(section.value(), std::nullopt, TheoremKind::Proof, {},
                                      InlineFromText("By induction."), false, "");
    auto unnumbered = editor.InsertTheorem(section.value(), std::nullopt, TheoremKind::Theorem, {},
                                           InlineFromText("Not numbered."), false, "");
    auto code = editor.InsertCode(section.value(), std::nullopt, "python", "print('hi')\n",
                                  InlineFromText("Example listing"), true, "lst:main");
    PF_CHECK(theorem.ok() && lemma.ok() && proof.ok() && unnumbered.ok() && code.ok());

    const std::string tex = RenderTex(doc);
    PF_CHECK(tex.find("\\usepackage{amsthm}") != std::string::npos);
    PF_CHECK(tex.find("\\newtheorem{theorem}{Theorem}") != std::string::npos);
    PF_CHECK(tex.find("\\newtheorem*{theorem*}{Theorem}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{theorem}[Main result]") != std::string::npos);
    PF_CHECK(tex.find("\\label{thm:main}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{lemma}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{proof}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{theorem*}") != std::string::npos);
    PF_CHECK(tex.find("\\paperforgeListing{lst:main}{Example listing}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{verbatim}") != std::string::npos);
    PF_CHECK(tex.find("1  print('hi')") != std::string::npos);

    const auto numbering = DocumentNumberer::Build(doc);
    PF_CHECK(numbering.Find(theorem.value())->display == "Theorem 1");
    PF_CHECK(numbering.Find(lemma.value())->display == "Lemma 1");
    PF_CHECK(!numbering.Find(proof.value()).has_value());
    PF_CHECK(!numbering.Find(unnumbered.value()).has_value());
    PF_CHECK(numbering.Find(code.value())->display == "Listing 1");
    PF_CHECK(numbering.Find(code.value())->latex_label == "lst:main");
}

PF_TEST(TheoremAndCodeEditingPersistAndValidate) {
    SerializedProject project;
    project.project_id = "p-thm";
    project.revision = ProjectRevision{1};
    project.template_id = "generic-article";
    DocumentEditor editor(project.document);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    auto theorem = editor.InsertTheorem(section.value(), std::nullopt, TheoremKind::Definition,
                                        InlineFromText("Definition"), InlineFromText("A thing."), true, "def:a");
    PF_CHECK(theorem.ok());
    PF_CHECK(editor.SetTheorem(theorem.value(), InlineFromText("A changed thing."),
                               InlineFromText("Changed title"), false, "def:b", TheoremKind::Example)
                  .ok());
    auto code = editor.InsertCode(section.value(), std::nullopt, "cpp", "int main() {}\n", {}, false, "lst:x");
    PF_CHECK(code.ok());
    PF_CHECK(editor.SetCode(code.value(), "c++", "int main() { return 0; }\n", InlineFromText("main"),
                            true, "lst:y")
                  .ok());

    const std::string json = ProjectSerializer::Serialize(project);
    auto parsed = ProjectSerializer::Deserialize(json);
    PF_CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    const auto& blocks = DocumentMutableAccess::body(parsed.value().document).sections.at(0).blocks;
    const auto* theorem_back = std::get_if<TheoremBlock>(&blocks.at(0));
    PF_CHECK(theorem_back != nullptr);
    if (theorem_back) {
        PF_CHECK(theorem_back->kind == TheoremKind::Example);
        PF_CHECK(theorem_back->numbered == false);
        PF_CHECK(theorem_back->label == "def:b");
        PF_CHECK(InlineToPlainText(theorem_back->body) == "A changed thing.");
    }
    const auto* code_back = std::get_if<CodeBlock>(&blocks.at(1));
    PF_CHECK(code_back != nullptr);
    if (code_back) {
        PF_CHECK(code_back->language == "c++");
        PF_CHECK(code_back->line_numbers);
        PF_CHECK(code_back->label == "lst:y");
        PF_CHECK(code_back->code.find("return 0") != std::string::npos);
    }

    // 代码结束标记必须被拒绝；未知语言只警告。
    Document bad;
    DocumentEditor bad_editor(bad);
    auto bad_section = bad_editor.InsertSection(0, InlineFromText("S"));
    bad_editor.InsertCode(bad_section.value(), std::nullopt, "python", "x\n\\end{lstlisting}\ny\n", {}, true, "");
    bad_editor.InsertCode(bad_section.value(), std::nullopt, "nosuch", "x\n", {}, true, "");
    ValidationInput input;
    input.document = &bad;
    input.template_id = "generic-article";
    input.revision = ProjectRevision{1};
    const auto validation = Validator().Validate(input);
    PF_CHECK(CountDiagnostics(validation, "E-CODE-END-MARKER") == 1);
    PF_CHECK(CountDiagnostics(validation, "W-CODE-LANGUAGE") == 1);
}
