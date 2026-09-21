// 数学输入重设计测试（equation.txt）：
//   * MathExpression 是唯一的数学内容类型
//   * MathGenerator 掌管定界符与外层环境
//   * MathValidator 强制约束 LaTeX 输入边界
//   * persistence 只存储裸 body（绝不存储生成的环境）
//   * 渲染器把交叉引用解析为生效的 label
#include "TestMain.hpp"

#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "math/MathGenerator.h"
#include "math/MathValidator.h"
#include "persistence/ProjectPersistence.h"
#include "render/LatexRenderer.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

Body& BodyOf(Document& doc) { return DocumentMutableAccess::body(doc); }

std::string RenderTex(const Document& doc,
                      const std::string& template_id = "generic-article") {
    RenderRequest request;
    request.document = &doc;
    request.template_id = template_id;
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    if (rendered.package.files.empty()) return {};
    return rendered.package.files[0].content;
}

// 一个包含一个带编号公式以及一个引用该公式的段落的文档。
struct EquationFixture {
    Document doc;
    NodeId equation;
};

EquationFixture MakeEquationDoc(const std::string& body,
                                const std::string& label, bool numbered) {
    EquationFixture fixture;
    DocumentEditor editor(fixture.doc);
    (void)editor.SetTitle(InlineFromText("t"));
    auto section = editor.InsertSection(0, InlineFromText("S"));
    EquationBlock eq;
    eq.expression.latex = body;
    eq.numbered = numbered;
    eq.label = label;
    auto inserted = editor.InsertBlock(section.value(), std::nullopt, eq);
    fixture.equation = inserted.value();
    return fixture;
}

}  // namespace

// ---------------- 生成器 ----------------

PF_TEST(MathGeneratorWrapsInlineBodyInParenDelimiters) {
    MathExpression expression;
    expression.latex = "\\frac{a}{b}";
    const std::string tex = GenerateInlineMath(expression);
    PF_CHECK(tex == "\\(\\frac{a}{b}\\)");
    // 用户 body 中绝不包含定界符。
    PF_CHECK(expression.latex.find('$') == std::string::npos);
}

PF_TEST(MathGeneratorBuildsNumberedEnvironmentWithLabel) {
    MathExpression expression;
    expression.latex = "E = mc^2";
    const std::string tex = GenerateDisplayMath(expression, true, "eq:energy");
    PF_CHECK(tex.find("\\begin{equation}") != std::string::npos);
    PF_CHECK(tex.find("\\label{eq:energy}") != std::string::npos);
    PF_CHECK(tex.find("E = mc^2") != std::string::npos);
    PF_CHECK(tex.find("\\end{equation}") != std::string::npos);
    // 环境不属于用户的源码内容。
    PF_CHECK(expression.latex.find("begin{equation}") == std::string::npos);
}

PF_TEST(MathGeneratorBuildsUnnumberedDisplayEnvironment) {
    MathExpression expression;
    expression.latex = "a = b";
    const std::string tex = GenerateDisplayMath(expression, false, "eq:x");
    PF_CHECK(tex.find("\\[") != std::string::npos);
    PF_CHECK(tex.find("\\]") != std::string::npos);
    PF_CHECK(tex.find("\\begin{equation}") == std::string::npos);
    PF_CHECK(tex.find("\\label") == std::string::npos);
}

// ---------------- 校验器 ----------------

PF_TEST(MathValidatorAcceptsMathInternalStructures) {
    const char* accepted[] = {
        "\\frac{a}{b}",
        "\\sqrt[3]{x}",
        "\\sum_{i=1}^{n} i^2",
        "\\int_0^1 f(x)\\,dx",
        "\\left( \\frac{a}{b} \\right)^2",
        "\\mathbf{v} \\cdot \\mathcal{L}",
        "\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}",
        "\\begin{cases} 1 & x > 0 \\\\ 0 & x \\le 0 \\end{cases}",
        "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}",
        "x_i^2",
        "\\text{where } n \\to \\infty",
    };
    for (const char* source : accepted) {
        const MathValidation result = ValidateMath(source, MathFlavor::Inline);
        if (!result.valid()) {
            std::cout << "    rejected: " << source << " -> " << result.code
                      << " " << result.error << "\n";
        }
        PF_CHECK(result.valid());
    }
}

PF_TEST(MathValidatorRejectsDocumentLevelCommands) {
    const char* rejected[] = {
        "\\documentclass{article}",
        "\\usepackage{amsmath}",
        "\\section{Intro}",
        "\\input{other.tex}",
        "\\newcommand{\\R}{\\mathbb{R}}",
        "\\begin{document} x \\end{document}",
        "\\label{eq:x}",
    };
    for (const char* source : rejected) {
        const MathValidation result = ValidateMath(source, MathFlavor::Display);
        PF_CHECK(result.invalid());
        if (result.invalid()) {
            std::cout << "    " << source << " -> " << result.code << "\n";
        }
    }
}

PF_TEST(MathValidatorRejectsOuterFormulaEnvironments) {
    const char* rejected[] = {
        "\\begin{equation} x \\end{equation}",
        "\\begin{equation*} x \\end{equation*}",
        "\\begin{align} x \\end{align}",
        "\\begin{figure} x \\end{figure}",
        "\\begin{table} x \\end{table}",
        "\\begin{displaymath} x \\end{displaymath}",
    };
    for (const char* source : rejected) {
        const MathValidation result = ValidateMath(source, MathFlavor::Display);
        PF_CHECK(result.invalid());
        if (result.invalid()) PF_CHECK(result.code == "E-MATH-FORBIDDEN-ENV");
    }
}

PF_TEST(MathValidatorRejectsUserSuppliedDelimiters) {
    const char* rejected[] = {"$x$", "\\(x\\)", "\\[x\\]"};
    for (const char* source : rejected) {
        const MathValidation result = ValidateMath(source, MathFlavor::Inline);
        PF_CHECK(result.invalid());
        if (result.invalid()) PF_CHECK(result.code == "E-MATH-DELIMITER");
    }
}

PF_TEST(MathValidatorFlagsUnbalancedSource) {
    PF_CHECK(ValidateMath("\\frac{a}{b", MathFlavor::Inline).invalid());
    PF_CHECK(ValidateMath("\\begin{aligned} x", MathFlavor::Display).invalid());
    PF_CHECK(ValidateMath("\\left( x", MathFlavor::Inline).invalid());
    PF_CHECK(ValidateMath("x \\right)", MathFlavor::Inline).invalid());
}

PF_TEST(MathValidatorTreatsEmptyAsPendingNotInvalid) {
    const MathValidation empty = ValidateMath("", MathFlavor::Inline);
    PF_CHECK(empty.pending());
    PF_CHECK(!empty.invalid());
    PF_CHECK(ToString(empty.state) == std::string("Pending"));
}

PF_TEST(InvalidMathKeepsTheOriginalSource) {
    // 校验是只读的：body 保持不变（设计 §8）。
    MathExpression expression;
    expression.latex = "\\begin{equation} x";
    (void)ValidateMath(expression.latex, MathFlavor::Display);
    PF_CHECK(expression.latex == "\\begin{equation} x");
}

// ---------------- 渲染器 ----------------

PF_TEST(RendererEmitsNumberedEnvironmentWithUserLabel) {
    EquationFixture fixture = MakeEquationDoc("E = mc^2", "eq:energy", true);
    const std::string tex = RenderTex(fixture.doc);
    PF_CHECK(tex.find("\\begin{equation}\\label{eq:energy}") !=
             std::string::npos);
    PF_CHECK(tex.find("E = mc^2") != std::string::npos);
    PF_CHECK(tex.find("\\end{equation}") != std::string::npos);
}

PF_TEST(RendererEmitsBracketEnvironmentForUnnumberedEquation) {
    EquationFixture fixture = MakeEquationDoc("a = b", "eq:x", false);
    const std::string tex = RenderTex(fixture.doc);
    PF_CHECK(tex.find("\\[\n") != std::string::npos);
    PF_CHECK(tex.find("\n\\]") != std::string::npos);
    PF_CHECK(tex.find("\\begin{equation}") == std::string::npos);
}

PF_TEST(CrossReferenceResolvesToTheEquationLabel) {
    EquationFixture fixture = MakeEquationDoc("E = mc^2", "eq:energy", true);
    DocumentEditor editor(fixture.doc);
    Paragraph para;
    para.content.push_back(TextRun{"See ", 0});
    CrossReference ref;
    ref.target = fixture.equation;
    para.content.push_back(ref);
    para.content.push_back(TextRun{" for the relation.", 0});
    auto section = editor.InsertSection(1, InlineFromText("Refs"));
    (void)editor.InsertBlock(section.value(), std::nullopt, para);

    const std::string tex = RenderTex(fixture.doc);
    PF_CHECK(tex.find("\\ref{eq:energy}") != std::string::npos);
    PF_CHECK(tex.find("\\ref{" + fixture.equation.value() + "}") ==
             std::string::npos);
}

PF_TEST(CrossReferenceFallsBackToTheNodeId) {
    EquationFixture fixture = MakeEquationDoc("E = mc^2", "", true);
    DocumentEditor editor(fixture.doc);
    Paragraph para;
    CrossReference ref;
    ref.target = fixture.equation;
    para.content.push_back(ref);
    auto section = editor.InsertSection(1, InlineFromText("Refs"));
    (void)editor.InsertBlock(section.value(), std::nullopt, para);

    const std::string tex = RenderTex(fixture.doc);
    PF_CHECK(tex.find("\\label{" + fixture.equation.value() + "}") !=
             std::string::npos);
    PF_CHECK(tex.find("\\ref{" + fixture.equation.value() + "}") !=
             std::string::npos);
}

// ---------------- 面向文档的校验器 ----------------

PF_TEST(DocumentValidatorReportsInvalidMathBoundary) {
    EquationFixture fixture =
        MakeEquationDoc("\\begin{equation} x \\end{equation}", "", true);
    ValidationInput input;
    input.document = &fixture.doc;
    input.template_id = "generic-article";
    input.revision = ProjectRevision{1};
    auto result = Validator().Validate(input);
    bool found = false;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == "E-MATH-FORBIDDEN-ENV") found = true;
    }
    PF_CHECK(found);
}

// ---------------- 持久化 ----------------

PF_TEST(MathPersistenceStoresBareLatexFields) {
    Document doc;
    DocumentEditor editor(doc);
    (void)editor.SetTitle(InlineFromText("t"));
    auto section = editor.InsertSection(0, InlineFromText("S"));

    Paragraph para;
    para.content.push_back(TextRun{"loss ", 0});
    InlineMath math;
    math.expression.latex = "\\frac{a}{b}";
    para.content.push_back(math);
    (void)editor.InsertBlock(section.value(), std::nullopt, para);

    EquationBlock eq;
    eq.expression.latex = "E = mc^2";
    eq.numbered = true;
    eq.label = "eq:energy";
    (void)editor.InsertBlock(section.value(), std::nullopt, eq);

    SerializedProject project;
    project.project_id = "p-math";
    project.revision = ProjectRevision{3};
    project.template_id = "generic-article";
    project.document = doc;
    const std::string json = ProjectSerializer::Serialize(project);

    // 生成的环境绝不得写入磁盘。
    PF_CHECK(json.find("\"inline_math\"") != std::string::npos);
    PF_CHECK(json.find("\"latex\"") != std::string::npos);
    PF_CHECK(json.find("\"type\": \"equation\"") != std::string::npos);
    PF_CHECK(json.find("\"label\": \"eq:energy\"") != std::string::npos);
    PF_CHECK(json.find("\\begin{equation}") == std::string::npos);
    PF_CHECK(json.find("\\end{equation}") == std::string::npos);

    auto back = ProjectSerializer::Deserialize(json);
    PF_CHECK(back.ok());
    if (!back.ok()) return;
    const auto& blocks = BodyOf(back.value().document).sections[0].blocks;
    PF_CHECK(blocks.size() == 2);
    const auto* stored_para = std::get_if<Paragraph>(&blocks[0]);
    PF_CHECK(stored_para != nullptr);
    if (stored_para) {
        const auto* stored_math = std::get_if<InlineMath>(&stored_para->content[1]);
        PF_CHECK(stored_math != nullptr);
        if (stored_math) PF_CHECK(stored_math->expression.latex == "\\frac{a}{b}");
    }
    const auto* stored_eq = std::get_if<EquationBlock>(&blocks[1]);
    PF_CHECK(stored_eq != nullptr);
    if (stored_eq) {
        PF_CHECK(stored_eq->expression.latex == "E = mc^2");
        PF_CHECK(stored_eq->numbered);
        PF_CHECK(stored_eq->label == "eq:energy");
    }
}

PF_TEST(LegacyMathJsonStillLoads) {
    // 重设计之前的文件：使用 "math" 的 "inlineEquation"/"displayEquation"。
    const std::string legacy = R"({
      "schemaVersion": "2",
      "projectId": "p-legacy",
      "revision": 1,
      "template": "generic-article",
      "frontMatter": {"title": [{"type": "text", "text": "t"}]},
      "body": {"sections": [
        {"id": "s1", "title": [{"type": "text", "text": "S"}],
         "blocks": [
           {"type": "paragraph", "id": "n1",
            "content": [{"type": "text", "text": "x "},
                        {"type": "inlineEquation", "math": "\\alpha"}]},
           {"type": "displayEquation", "id": "n2", "math": "E = mc^2",
            "numbered": true}
         ]}
      ]},
      "assets": []
    })";
    auto loaded = ProjectSerializer::Deserialize(legacy);
    PF_CHECK(loaded.ok());
    if (!loaded.ok()) return;
    const auto& blocks = BodyOf(loaded.value().document).sections[0].blocks;
    PF_CHECK(blocks.size() == 2);
    const auto* para = std::get_if<Paragraph>(&blocks[0]);
    PF_CHECK(para != nullptr);
    if (para) {
        const auto* math = std::get_if<InlineMath>(&para->content[1]);
        PF_CHECK(math != nullptr);
        if (math) PF_CHECK(math->expression.latex == "\\alpha");
    }
    const auto* eq = std::get_if<EquationBlock>(&blocks[1]);
    PF_CHECK(eq != nullptr);
    if (eq) {
        PF_CHECK(eq->expression.latex == "E = mc^2");
        PF_CHECK(eq->numbered);
        PF_CHECK(eq->label.empty());
    }
}

PF_TEST(RichTextEncodingUsesGeneratedDelimiter) {
    InlineContent content;
    content.push_back(TextRun{"x "});
    InlineMath math;
    math.expression.latex = "\\alpha";
    content.push_back(math);
    const std::string encoded = InlineToRichText(content);
    PF_CHECK(encoded.find("\\(\\alpha\\)") != std::string::npos);
    PF_CHECK(InlineFromRichText(encoded) == content);
}
