// WP-06：列表与脚注的模型、编辑、遍历、渲染、校验、持久化回归。
#include "TestMain.hpp"

#include <string>
#include <vector>

#include "bibliography/BibliographyService.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "numbering/CitationNumberResolver.h"
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

BibliographyDatabase MakeDb(const std::vector<std::string>& keys) {
    std::string bib;
    for (const auto& key : keys)
        bib += "@article{" + key + ", author={A. Author}, title={T}, year={2020}}\n";
    BibliographyDatabase db;
    BibliographyService service(db);
    service.ImportText(bib);
    return db;
}

const Body& BodyOf(const Document& doc) {
    return DocumentMutableAccess::body(const_cast<Document&>(doc));
}

int CountDiagnostics(const ValidationResult& result, const char* code) {
    int count = 0;
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code)
            ++count;
    return count;
}

InlineContent MakeNoteBody() {
    InlineContent body;
    body.push_back(TextRun{"A note with ", 0});
    InlineMath math;
    math.expression.latex = "\\alpha";
    body.push_back(std::move(math));
    return body;
}

} // namespace

PF_TEST(FootnoteBodyIsVisitedAndCitationsInsideCount) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Paragraph para;
    para.content.push_back(TextRun{"See ", 0});
    para.content.push_back(MakeFootnote(MakeNoteBody()));
    Citation citation;
    citation.keys = {"a"};
    para.content.push_back(citation);
    auto paragraph = editor.InsertBlock(section.value(), std::nullopt, para);

    // 脚注内的 citation 必须先于正文 citation 参与首次引用编号。
    InlineContent body;
    body.push_back(TextRun{"Cite ", 0});
    Citation inside;
    inside.keys = {"a"};
    body.push_back(inside);
    PF_CHECK(editor.InsertFootnote(paragraph.value(), 1, body).ok());

    const auto db = MakeDb({"a"});
    const auto resolver = CitationNumberResolver::Build(doc, db);
    PF_CHECK(resolver.Find("a")->number == 1);
    const std::string tex = RenderTex(doc);
    PF_CHECK(tex.find("\\footnote{") != std::string::npos);
    PF_CHECK(tex.find("\\citep{a}") != std::string::npos);
}

PF_TEST(FootnoteEditingTargetsNthReferenceAndRejectsNested) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Paragraph para;
    para.content.push_back(TextRun{"A", 0});
    para.content.push_back(MakeFootnote(MakeNoteBody()));
    para.content.push_back(TextRun{"B", 0});
    auto paragraph = editor.InsertBlock(section.value(), std::nullopt, para);

    PF_CHECK(editor.InsertFootnote(paragraph.value(), std::nullopt, InlineFromText("note")).ok());
    PF_CHECK(editor.SetFootnote(paragraph.value(), 1, InlineFromText("edited")).ok());
    PF_CHECK(editor.DeleteFootnote(paragraph.value(), 0).ok());
    const auto* stored = std::get_if<Paragraph>(editor.FindBlock(paragraph.value()));
    PF_CHECK(stored != nullptr);
    if (!stored)
        return;
    int footnotes = 0;
    for (const auto& node : stored->content) {
        const auto* footnote = std::get_if<Footnote>(&node);
        if (!footnote)
            continue;
        ++footnotes;
        PF_CHECK(footnote->body && InlineToPlainText(footnote->body->content) == "edited");
    }
    PF_CHECK(footnotes == 1);

    // 嵌套脚注：模型允许构造，但 Validator 必须给出定位诊断。
    Paragraph nested_para;
    nested_para.content.push_back(TextRun{"nested", 0});
    InlineContent inner;
    inner.push_back(MakeFootnote(InlineFromText("inner")));
    InlineContent outer;
    outer.push_back(MakeFootnote(std::move(inner)));
    nested_para.content.push_back(MakeFootnote(std::move(outer)));
    auto nested = editor.InsertBlock(section.value(), std::nullopt, nested_para);

    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.revision = ProjectRevision{1};
    auto validation = Validator().Validate(input);
    PF_CHECK(CountDiagnostics(validation, "E-FOOTNOTE-NESTED") >= 1);
    if (nested.ok())
        PF_CHECK(doc.ContainsNode(nested.value()));
}

PF_TEST(FootnoteClipboardCodecPreservesBody) {
    InlineContent content;
    content.push_back(TextRun{"before ", 0});
    content.push_back(MakeFootnote(MakeNoteBody()));
    content.push_back(TextRun{" after", 0});
    const std::string encoded = InlineToClipboard(content);
    PF_CHECK(encoded.rfind("PFINLINE2", 0) == 0);
    const auto decoded = InlineFromClipboard(encoded);
    PF_CHECK(decoded.has_value());
    if (!decoded)
        return;
    PF_CHECK(*decoded == content);
}

PF_TEST(ListEditingRendersAndPersists) {
    SerializedProject project;
    project.project_id = "p-list";
    project.revision = ProjectRevision{1};
    project.template_id = "generic-article";
    DocumentEditor editor(project.document);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    auto list = editor.InsertList(section.value(), std::nullopt, ListStyle::Bullet, InlineFromText("First"));
    PF_CHECK(list.ok());
    auto list_id = list.value();
    auto second = editor.InsertListItem(list_id, 1, InlineFromText("Second"));
    PF_CHECK(second.ok());
    PF_CHECK(editor.SetListItemContent(list_id, 1, InlineFromText("Second edited")).ok());
    PF_CHECK(editor.MoveListItem(list_id, 1, 0).ok());
    PF_CHECK(editor.SetListStyle(list_id, ListStyle::Numbered).ok());
    PF_CHECK(editor.InsertListItem(list_id, 2, InlineFromText("Third")).ok());

    auto rendered = RenderTex(project.document);
    PF_CHECK(rendered.find("\\begin{enumerate}") != std::string::npos);
    PF_CHECK(rendered.find("\\item Second edited") != std::string::npos);
    PF_CHECK(rendered.find("\\end{enumerate}") != std::string::npos);

    const std::string json = ProjectSerializer::Serialize(project);
    auto parsed = ProjectSerializer::Deserialize(json);
    PF_CHECK(parsed.ok());
    if (!parsed)
        return;
    const auto& blocks = BodyOf(parsed.value().document).sections.at(0).blocks;
    const auto* list_back = blocks.empty() ? nullptr : std::get_if<ListBlock>(&blocks.front());
    PF_CHECK(list_back != nullptr);
    if (!list_back)
        return;
    PF_CHECK(list_back->style == ListStyle::Numbered);
    PF_CHECK(list_back->items.size() == 3);
    PF_CHECK(InlineToPlainText(list_back->items[0].content) == "Second edited");
    PF_CHECK(InlineToPlainText(list_back->items[1].content) == "First");
    PF_CHECK(InlineToPlainText(list_back->items[2].content) == "Third");
}

PF_TEST(FootnotePersistsWithSchemaV5) {
    SerializedProject project;
    project.project_id = "p-footnote";
    project.revision = ProjectRevision{1};
    project.template_id = "generic-article";
    DocumentEditor editor(project.document);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Paragraph para;
    para.content.push_back(TextRun{"See ", 0});
    auto paragraph = editor.InsertBlock(section.value(), std::nullopt, para);
    PF_CHECK(editor.InsertFootnote(paragraph.value(), std::nullopt, MakeNoteBody()).ok());

    const std::string json = ProjectSerializer::Serialize(project);
    PF_CHECK(json.find("\"schemaVersion\": \"5\"") != std::string::npos);
    PF_CHECK(json.find("\"type\": \"footnote\"") != std::string::npos);
    auto parsed = ProjectSerializer::Deserialize(json);
    PF_CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    const auto& blocks = BodyOf(parsed.value().document).sections.at(0).blocks;
    const auto* back = blocks.empty() ? nullptr : std::get_if<Paragraph>(&blocks.front());
    PF_CHECK(back != nullptr);
    if (!back)
        return;
    const auto* footnote = std::get_if<Footnote>(&back->content.at(1));
    PF_CHECK(footnote != nullptr && footnote->body);
    if (footnote && footnote->body)
        PF_CHECK(InlineToPlainText(footnote->body->content) == "A note with \\alpha");
}

PF_TEST(FootnoteCitationsUseReferencePointOrder) {
    Document doc;
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Paragraph para;
    InlineContent body;
    Citation inside;
    inside.keys = {"b"};
    body.push_back(inside);
    para.content.push_back(Footnote{std::make_shared<const FootnoteBody>(FootnoteBody{std::move(body)})});
    Citation after;
    after.keys = {"a"};
    para.content.push_back(after);
    auto paragraph = editor.InsertBlock(section.value(), std::nullopt, para);
    PF_CHECK(paragraph.ok());

    const auto db = MakeDb({"a", "b"});
    const auto resolver = CitationNumberResolver::Build(doc, db);
    PF_CHECK(resolver.Find("b")->number == 1);
    PF_CHECK(resolver.Find("a")->number == 2);
}
