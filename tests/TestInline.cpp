// Stage B 测试：富文本行内编辑（方案 §4-§6、§14）。
//   * TextMark 组合在往返过程中保持不变
//   * 渲染器嵌套 \textbf{}/\emph{}，而不是丢弃某个标记
//   * 富文本编辑器表示是无损且可 diff 的
//   * 行内 token 在整个编辑协议中始终保有语义
#include "TestMain.hpp"

#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/Document.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "editing/EditingSystem.h"
#include "project/ProjectSession.h"
#include "render/LatexRenderer.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

ProjectState MakeState() {
    ProjectState state;
    state.SetId(ProjectId("p-rich"));
    return state;
}

Body& BodyOf(Document& doc) { return DocumentMutableAccess::body(doc); }

EditingSystem MakeEditing(ProjectState& state) {
    EditingSystem::Host host;
    host.project_id = [&] { return state.id(); };
    host.revision = [&] { return state.revision(); };
    host.bump_revision = [&] { return state.BumpRevision(); };
    host.document = [&]() -> Document& { return state.mutable_document(); };
    return EditingSystem(host);
}

EditCommand MakeCmd(ProjectState& state, FullEditPayload payload) {
    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = state.id();
    cmd.base_revision = state.revision();
    cmd.origin = EditOrigin::User;
    cmd.payload = std::move(payload);
    return cmd;
}

// 一个包含各种标记组合的 TextRun 的段落。
Paragraph MakeMixedParagraph() {
    Paragraph para;
    para.content.push_back(TextRun{"plain ", 0});
    para.content.push_back(TextRun{"bold ", static_cast<uint8_t>(TextMark::Strong)});
    para.content.push_back(TextRun{"italic ",
                                   static_cast<uint8_t>(TextMark::Emphasis)});
    para.content.push_back(
        TextRun{"both", static_cast<uint8_t>(TextMark::Strong | TextMark::Emphasis)});
    return para;
}

}  // namespace

PF_TEST(RichTextSerializationRoundTripsMarks) {
    InlineContent content;
    content.push_back(TextRun{"The method ", 0});
    content.push_back(TextRun{"significantly improves",
                              static_cast<uint8_t>(TextMark::Strong)});
    content.push_back(TextRun{" detection ", 0});
    content.push_back(TextRun{"accuracy",
                              static_cast<uint8_t>(TextMark::Emphasis)});
    content.push_back(TextRun{".", 0});

    const std::string text = InlineToRichText(content);
    // 可读、可 diff，且不再使用旧的 [cite:] 风格编码。
    std::cout << "    rich = \"" << text << "\"\n";
    PF_CHECK(text.find("significantly improves") != std::string::npos);
    PF_CHECK(text.find("**") != std::string::npos);
    PF_CHECK(text.find("*accuracy*") != std::string::npos);

    const InlineContent back = InlineFromRichText(text);
    PF_CHECK(back == content);
}

PF_TEST(RichTextSerializationHandlesBoldItalicCombination) {
    InlineContent content;
    content.push_back(
        TextRun{"both", static_cast<uint8_t>(TextMark::Strong | TextMark::Emphasis)});
    content.push_back(TextRun{" plain", 0});

    const std::string text = InlineToRichText(content);
    PF_CHECK(text.find("***both***") != std::string::npos);
    const InlineContent back = InlineFromRichText(text);
    PF_CHECK(back.size() == 2);
    const auto* first = std::get_if<TextRun>(&back[0]);
    PF_CHECK(first != nullptr);
    if (first) {
        PF_CHECK(first->text == "both");
        PF_CHECK(HasMark(first->marks, TextMark::Strong));
        PF_CHECK(HasMark(first->marks, TextMark::Emphasis));
    }
    // 末尾的普通文本不得继承这些标记。
    const auto* second = std::get_if<TextRun>(&back[1]);
    PF_CHECK(second != nullptr);
    if (second) PF_CHECK(second->marks == 0);
}

PF_TEST(RichTextSerializationKeepsSemanticTokens) {
    InlineContent content;
    content.push_back(TextRun{"As shown in ", 0});
    Citation citation;
    citation.keys = {"smith2024", "li2023"};
    content.push_back(citation);
    content.push_back(TextRun{" and ", 0});
    CrossReference ref;
    ref.target = NodeId("n42");
    content.push_back(ref);
    content.push_back(TextRun{" we ...", 0});
    InlineMath eq;
    eq.expression.latex = "\\alpha";
    content.push_back(eq);

    const std::string text = InlineToRichText(content);
    PF_CHECK(text.find("[cite:smith2024,li2023]") != std::string::npos);
    PF_CHECK(text.find("[ref:n42]") != std::string::npos);
    // 规范写法使用自动生成的 \(...\) 定界符；用户从不输入定界符，
    // 而旧的 $...$ 形式仍可解析。
    PF_CHECK(text.find("\\(\\alpha\\)") != std::string::npos);

    const InlineContent back = InlineFromRichText(text);
    PF_CHECK(back == content);
    PF_CHECK(InlineIsRich(content));
}

PF_TEST(RichTextSerializationOfPlainTextStaysPlainText) {
    const InlineContent content = InlineFromText("nothing special here");
    PF_CHECK(!InlineIsRich(content));
    const std::string text = InlineToRichText(content);
    PF_CHECK(text == "nothing special here");
    PF_CHECK(InlineFromRichText(text) == content);
}

PF_TEST(RichTextParseNeverDropsUnknownMarkup) {
    // 没有闭合定界符的孤立星号仍作为字面文本保留。
    const InlineContent parsed = InlineFromRichText("a * b ** c");
    std::string joined;
    for (const auto& node : parsed) {
        if (const auto* run = std::get_if<TextRun>(&node)) joined += run->text;
    }
    PF_CHECK(joined.find("a * b ** c") != std::string::npos);
}

// ---------------- 渲染器 ----------------

PF_TEST(RendererNestsStrongAndEmphasis) {
    InlineContent content;
    content.push_back(TextRun{"plain ", 0});
    content.push_back(TextRun{"b", static_cast<uint8_t>(TextMark::Strong)});
    content.push_back(TextRun{" ", 0});
    content.push_back(TextRun{"i", static_cast<uint8_t>(TextMark::Emphasis)});
    content.push_back(TextRun{" ", 0});
    content.push_back(
        TextRun{"bi", static_cast<uint8_t>(TextMark::Strong | TextMark::Emphasis)});

    Document doc;
    DocumentEditor editor(doc);
    (void)editor.SetTitle(InlineFromText("t"));
    auto section = editor.InsertSection(0, InlineContent{});
    Paragraph para;
    para.content = content;
    (void)editor.InsertBlock(section.value(), std::nullopt, para);

    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    const std::string& tex = rendered.package.files[0].content;
    PF_CHECK(tex.find("plain \\textbf{b} \\emph{i} \\textbf{\\emph{bi}}") !=
             std::string::npos);
}

PF_TEST(RendererKeepsMarksThroughDocumentRender) {
    Document doc;
    DocumentEditor editor(doc);
    (void)editor.SetTitle(InlineFromText("Marks"));
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Paragraph para = MakeMixedParagraph();
    (void)editor.InsertBlock(section.value(), std::nullopt, para);

    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    const std::string& tex = rendered.package.files[0].content;
    PF_CHECK(tex.find("\\textbf{bold }") != std::string::npos);
    PF_CHECK(tex.find("\\emph{italic }") != std::string::npos);
    // 组合标记会嵌套；不得丢失任何一个标记。
    PF_CHECK(tex.find("\\textbf{\\emph{both}}") != std::string::npos);
}

// ---------------- 编辑协议 ----------------

PF_TEST(ParagraphContentRoundTripsThroughEditingProtocol) {
    ProjectState state = MakeState();
    auto editing = MakeEditing(state);

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    auto sec_result = editing.Apply(MakeCmd(state, sec));

    InsertParagraphPayload insert;
    insert.parent = sec_result.created_node;
    insert.content = InlineFromText("placeholder");
    auto para_result = editing.Apply(MakeCmd(state, insert));

    const InlineContent rich = MakeMixedParagraph().content;
    EditParagraphPayload edit;
    edit.paragraph = para_result.created_node;
    edit.content = rich;
    PF_CHECK(editing.Apply(MakeCmd(state, edit)).status == EditStatus::Applied);

    // 文档原样存储提交的内容——不做扁平化。
    const auto& stored = std::get<Paragraph>(
        BodyOf(state.mutable_document()).sections[0].blocks[0]);
    PF_CHECK(stored.content == rich);

    // Undo 恢复占位内容；redo 把富文本内容重新带回。
    PF_CHECK(editing.Undo().status == EditStatus::Applied);
    const auto& undone = std::get<Paragraph>(
        BodyOf(state.mutable_document()).sections[0].blocks[0]);
    PF_CHECK(InlineToPlainText(undone.content) == "placeholder");
    PF_CHECK(editing.Redo().status == EditStatus::Applied);
    const auto& redone = std::get<Paragraph>(
        BodyOf(state.mutable_document()).sections[0].blocks[0]);
    PF_CHECK(redone.content == rich);
}

PF_TEST(InlineCitationAndReferenceSurviveProtocolRoundTrip) {
    ProjectState state = MakeState();
    auto editing = MakeEditing(state);

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    auto sec_result = editing.Apply(MakeCmd(state, sec));

    InsertParagraphPayload insert;
    insert.parent = sec_result.created_node;
    insert.content = InlineFromText("text");
    auto para_result = editing.Apply(MakeCmd(state, insert));
    const NodeId para_id = para_result.created_node;

    InsertCitationPayload cite;
    cite.paragraph = para_id;
    cite.keys = {"smith2024"};
    PF_CHECK(editing.Apply(MakeCmd(state, cite)).status == EditStatus::Applied);

    // 交叉引用所指向的目标节点。
    InsertFigurePayload fig;
    fig.parent = sec_result.created_node;
    fig.asset_id = AssetId("a1");
    auto fig_result = editing.Apply(MakeCmd(state, fig));

    InsertCrossReferencePayload xref;
    xref.paragraph = para_id;
    xref.target = fig_result.created_node;
    PF_CHECK(editing.Apply(MakeCmd(state, xref)).status == EditStatus::Applied);

    // 此时段落按顺序持有 [text][cite][xref]，且 search/outline 使用的
    // 纯文本形式仍会显示这些 token。
    const auto& stored = std::get<Paragraph>(
        BodyOf(state.mutable_document()).sections[0].blocks[0]);
    PF_CHECK(stored.content.size() == 3);
    PF_CHECK(std::holds_alternative<Citation>(stored.content[1]));
    PF_CHECK(std::holds_alternative<CrossReference>(stored.content[2]));
    const std::string plain = InlineToPlainText(stored.content);
    PF_CHECK(plain.find("[cite:smith2024]") != std::string::npos);
    PF_CHECK(plain.find("[ref:" + fig_result.created_node.value() + "]") !=
             std::string::npos);

    // Validator 仍然接受它（该引用可以解析）。
    ValidationInput input;
    input.document = &state.mutable_document();
    input.template_id = "generic-article";
    input.revision = state.revision();
    auto result = Validator().Validate(input);
    for (const auto& d : result.diagnostics) {
        PF_CHECK(d.code != "E-MISSING-XREF-TARGET");
    }
}

PF_TEST(InlineMathRendersAsMathInParagraph) {
    ProjectState state = MakeState();
    auto editing = MakeEditing(state);

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    auto sec_result = editing.Apply(MakeCmd(state, sec));
    InsertParagraphPayload insert;
    insert.parent = sec_result.created_node;
    InlineContent content;
    content.push_back(TextRun{"The loss ", 0});
    InlineMath eq;
    eq.expression.latex = "\\mathcal{L}";
    content.push_back(eq);
    content.push_back(TextRun{" decreases.", 0});
    insert.content = content;
    auto para_result = editing.Apply(MakeCmd(state, insert));
    PF_CHECK(para_result.status == EditStatus::Applied);

    RenderRequest request;
    request.document = &state.mutable_document();
    request.template_id = "generic-article";
    request.revision = state.revision();
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    const std::string& tex = rendered.package.files[0].content;
    PF_CHECK(tex.find("The loss \\(\\mathcal{L}\\) decreases.") != std::string::npos);
}

PF_TEST(InlineMathTokenIsNotUserEditableText) {
    // 行内公式是一个语义节点：纯文本提取会显示其 math 源码，
    // 但该节点本身并不是编辑器会重新解析的字符串。
    InlineContent content;
    content.push_back(TextRun{"Loss ", 0});
    InlineMath eq;
    eq.expression.latex = "\\mathcal{L}";
    content.push_back(eq);
    PF_CHECK(InlineToPlainText(content) == "Loss \\mathcal{L}");
    PF_CHECK(InlineIsRich(content));
    // 往返后它仍是节点，绝不会变成 TextRun。
    const InlineContent back = InlineFromRichText(InlineToRichText(content));
    PF_CHECK(std::holds_alternative<InlineMath>(back[1]));
}

PF_TEST(PasteRulesAreExpressedInTheEditorRepresentation) {
    // 富文本粘贴会保留标记；硬换行重排逻辑保持不变（方案 §14）。
    // 这些行足够长，使重排启发式判定为硬换行段落，
    // 而不是有意的分行结构。
    const std::string wrapped =
        "the first line of a paragraph pasted out of a PDF file\n"
        "the second line of that same paragraph, equally long as the first\n"
        "and a third line to make it a genuine hard-wrapped three-line block";
    const std::string reflowed = ReflowHardWrappedText(wrapped);
    PF_CHECK(reflowed.find(
                  "PDF file the second line of that same paragraph") !=
              std::string::npos);

    // 标记在重排后依然保留（重排作用于文本，而不是标记）。
    const InlineContent content = InlineFromRichText("**bold** and *italic*");
    PF_CHECK(content.size() == 3);
    const auto* bold = std::get_if<TextRun>(&content[0]);
    PF_CHECK(bold != nullptr && bold->text == "bold");
    if (bold) PF_CHECK(HasMark(bold->marks, TextMark::Strong));
}

PF_TEST(SessionPersistsRichParagraph) {
    auto dir = std::filesystem::temp_directory_path() / "pf-rich-e2e";
    std::filesystem::remove_all(dir);
    ProjectSession::Config config;
    config.tectonic_path = PF_TECTONIC_BIN;
    config.debounce = std::chrono::milliseconds{0};
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));

    EditCommand sec_cmd;
    sec_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    sec_cmd.project_id = session.state().id();
    sec_cmd.base_revision = session.current_revision();
    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    sec_cmd.payload = sec;
    auto sec_result = session.Execute(sec_cmd);
    PF_CHECK(sec_result.status == EditStatus::Applied);

    EditCommand para_cmd = sec_cmd;
    para_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    para_cmd.base_revision = session.current_revision();
    InsertParagraphPayload para;
    para.parent = sec_result.created_node;
    para.content = MakeMixedParagraph().content;
    para_cmd.payload = para;
    PF_CHECK(session.Execute(para_cmd).status == EditStatus::Applied);

    session.Save();
    PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    std::string error;
    PF_CHECK(session.OpenProject(dir, &error));

    Document& reopened = session.mutable_document();
    const auto& stored =
        std::get<Paragraph>(BodyOf(reopened).sections[0].blocks[0]);
    PF_CHECK(stored.content == MakeMixedParagraph().content);
    std::filesystem::remove_all(dir);
}
