// Document 模型测试：schema、编辑器约束、索引。
#include "TestMain.hpp"

#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentIndex.h"
#include "document/InlineText.h"

using namespace pf;

namespace {

Document MakeSampleDoc() {
    Document doc;
    DocumentEditor editor(doc);
    editor.SetTitle(InlineFromText("Test Paper"));
    auto s1 = editor.InsertSection(0, InlineFromText("Intro"));
    auto s2 = editor.InsertSection(1, InlineFromText("Method"));
    (void)s2;

    Paragraph p;
    p.content = InlineFromText("Hello world with **bold** text.");
    editor.InsertBlock(s1.value(), std::nullopt, p);

    EquationBlock eq;
    eq.expression.latex = "x = y + 1";
    editor.InsertBlock(s1.value(), std::nullopt, eq);

    return doc;
}

}  // namespace

PF_TEST(DocumentVersionBumpsOnEdit) {
    Document doc;
    DocumentVersion v0 = doc.version();
    DocumentEditor editor(doc);
    editor.SetTitle(InlineFromText("t"));
    PF_CHECK(doc.version().value == v0.value + 1);
}

PF_TEST(EditorTextPreservesCitationsAndCrossReferences) {
    auto content = InlineFromEditorText(
        "Prior work [cite:smith2024,doe2025] supports "
        "[ref:figure-results].");
    PF_CHECK(content.size() == 5);
    PF_CHECK(std::holds_alternative<TextRun>(content[0]));
    const auto* citation = std::get_if<Citation>(&content[1]);
    PF_CHECK(citation != nullptr);
    PF_CHECK(citation->keys.size() == 2);
    PF_CHECK(citation->keys[0] == "smith2024");
    const auto* reference = std::get_if<CrossReference>(&content[3]);
    PF_CHECK(reference != nullptr);
    PF_CHECK(reference->target == NodeId("figure-results"));
    PF_CHECK(InlineToPlainText(content) ==
             "Prior work [cite:smith2024,doe2025] supports "
             "[ref:figure-results].");
}

PF_TEST(DocumentEditorInsertAndFindBlocks) {
    const Document doc = MakeSampleDoc();
    PF_CHECK(doc.body().sections.size() == 2);
    PF_CHECK(doc.body().sections[0].blocks.size() == 2);
    PF_CHECK(doc.GetNodeKind(doc.body().sections[0].id).has_value());
    PF_CHECK(doc.GetNodeKind(NodeId("missing")) == std::nullopt);
}

PF_TEST(DocumentEditorTableRectangularity) {
    auto columns = std::vector<TableColumn>{{ColumnAlignment::Left},
                                            {ColumnAlignment::Right}};
    auto table = DocumentEditor::MakeTable(columns, 3, true);
    PF_CHECK(table.ok());
    PF_CHECK(table.value().IsRectangular());
    PF_CHECK(table.value().cells.size() == 3);
    PF_CHECK(table.value().cells[0].size() == 2);

    // 空列会被拒绝
    auto bad = DocumentEditor::MakeTable({}, 2, false);
    PF_CHECK(!bad.ok());
}

PF_TEST(DocumentEditorTableMutationsKeepRectangle) {
    Document doc = MakeSampleDoc();
    DocumentEditor editor(doc);
    auto columns = std::vector<TableColumn>{{ColumnAlignment::Left},
                                            {ColumnAlignment::Center}};
    Table table = DocumentEditor::MakeTable(columns, 2, false).value();
    NodeId s1;
    {
        const Document& cd = doc;
        s1 = cd.body().sections[0].id;
    }
    auto id = editor.InsertBlock(s1, std::nullopt, table);
    PF_CHECK(id.ok());

    PF_CHECK(editor.InsertTableRow(id.value(), 0).ok());
    PF_CHECK(editor.InsertTableColumn(id.value(), 1, ColumnAlignment::Right).ok());
    PF_CHECK(editor.DeleteTableRow(id.value(), 1).ok());
    PF_CHECK(editor.DeleteTableColumn(id.value(), 0).ok());

    Block* block = editor.FindBlock(id.value());
    auto* t = std::get_if<Table>(block);
    PF_CHECK(t != nullptr);
    PF_CHECK(t->IsRectangular());
    PF_CHECK(t->cells.size() == 2);
    PF_CHECK(t->columns.size() == 2);
}

PF_TEST(DocumentEditorMoveBlock) {
    Document doc = MakeSampleDoc();
    DocumentEditor editor(doc);
    NodeId s1, s2;
    {
        const Document& cd = doc;
        s1 = cd.body().sections[0].id;
        s2 = cd.body().sections[1].id;
    }

    Paragraph a;
    a.content = InlineFromText("A");
    auto pa = editor.InsertBlock(s1, 0, a);
    Paragraph b;
    b.content = InlineFromText("B");
    editor.InsertBlock(s1, 1, b);
    Paragraph c;
    c.content = InlineFromText("C");
    editor.InsertBlock(s2, std::nullopt, c);

    // 把 A 从 s1 移动到 s2。
    PF_CHECK(editor.MoveBlock(pa.value(), s2, 0).ok());
    {
        const Document& cd = doc;
        PF_CHECK(cd.body().sections[0].blocks.size() == 3);  // B + 段落 + eq
        PF_CHECK(cd.body().sections[1].blocks.size() == 2);  // C + A
    }

    Block* moved = editor.FindBlock(pa.value());
    auto* p = std::get_if<Paragraph>(moved);
    PF_CHECK(p != nullptr);
    PF_CHECK(InlineToPlainText(p->content) == "A");
}

PF_TEST(InsertSubsectionAfterAnchor) {
    Document doc;
    DocumentEditor editor(doc);
    const Document& cd = doc;  // 只读视图：body() 在外部仅提供 const 版本
    auto sec = editor.InsertSection(cd.body().sections.size(),
                                    InlineFromText("S"));
    PF_CHECK(sec.ok());
    const NodeId section = sec.value();

    Paragraph a;
    a.content = InlineFromText("one");
    Paragraph b;
    b.content = InlineFromText("two");
    Paragraph c;
    c.content = InlineFromText("three");
    auto pa = editor.InsertBlock(section, std::nullopt, a);
    auto pb = editor.InsertBlock(section, std::nullopt, b);
    auto pc = editor.InsertBlock(section, std::nullopt, c);
    PF_CHECK(pa.ok() && pb.ok() && pc.ok());

    // 在「one」之后插入标题必须把「two」和「three」移入该标题之下，因此
    // 该标题确实出现在「one」与「two」之间。
    auto sub = editor.InsertSubsectionAfter(pa.value(), InlineFromText("Sub"));
    PF_CHECK(sub.ok());
    {
        const Section& s = cd.body().sections.front();
        PF_CHECK(s.blocks.size() == 1);
        PF_CHECK(s.subsections.size() == 1);
        PF_CHECK(InlineToPlainText(s.subsections[0].title) == "Sub");
        PF_CHECK(s.subsections[0].blocks.size() == 2);
        const auto* first = std::get_if<Paragraph>(&s.subsections[0].blocks[0]);
        const auto* second =
            std::get_if<Paragraph>(&s.subsections[0].blocks[1]);
        PF_CHECK(first && InlineToPlainText(first->content) == "two");
        PF_CHECK(second && InlineToPlainText(second->content) == "three");
    }

    // 子节内部某个 block 之后的标题会跟随该子节。
    const NodeId inner = std::visit([](const auto& v) { return v.id; },
                                    cd.body()
                                        .sections.front()
                                        .subsections[0]
                                        .blocks[0]);
    PF_CHECK(editor.InsertSubsectionAfter(inner, InlineFromText("Deeper")).ok());
    {
        const Section& s = cd.body().sections.front();
        PF_CHECK(s.subsections.size() == 2);
        PF_CHECK(InlineToPlainText(s.subsections[1].title) == "Deeper");
        PF_CHECK(s.subsections[1].blocks.size() == 1);
    }

    // 子节标题之后不会发生任何移动。
    PF_CHECK(editor
                 .InsertSubsectionAfter(cd.body().sections.front()
                                            .subsections[0]
                                            .id,
                                        InlineFromText("Sibling"))
                 .ok());
    PF_CHECK(cd.body().sections.front().subsections.size() == 3);
    PF_CHECK(InlineToPlainText(
                 cd.body().sections.front().subsections[1].title) ==
             "Sibling");

    // 在 section 标题之后，该 section 的全部正文会成为子节的内容。
    Document second;
    DocumentEditor editor2(second);
    const Document& cd2 = second;
    auto sec2 = editor2.InsertSection(cd2.body().sections.size(),
                                      InlineFromText("T"));
    Paragraph p1;
    p1.content = InlineFromText("alpha");
    Paragraph p2;
    p2.content = InlineFromText("beta");
    editor2.InsertBlock(sec2.value(), std::nullopt, p1);
    editor2.InsertBlock(sec2.value(), std::nullopt, p2);
    PF_CHECK(editor2
                 .InsertSubsectionAfter(sec2.value(), InlineFromText("First"))
                 .ok());
    {
        const Section& s = cd2.body().sections.front();
        PF_CHECK(s.blocks.empty());
        PF_CHECK(s.subsections.size() == 1);
        PF_CHECK(s.subsections[0].blocks.size() == 2);
    }

    // 未知锚点会被拒绝，而不是静默追加。
    PF_CHECK(!editor2.InsertSubsectionAfter(NodeId("nope"), InlineFromText("x"))
                  .ok());
}

PF_TEST(DocumentEditorSubsectionStructure) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    PF_CHECK(s.ok());
    auto sub = editor.InsertSubsection(0, 0, InlineFromText("Sub"));
    PF_CHECK(sub.ok());

    Paragraph p;
    p.content = InlineFromText("in sub");
    PF_CHECK(editor.InsertBlock(sub.value(), std::nullopt, p).ok());
    {
        const Document& cd = doc;
        PF_CHECK(cd.body().sections[0].subsections[0].blocks.size() == 1);
    }

    // 无效父节点会被拒绝
    Paragraph q;
    q.content = InlineFromText("bad");
    auto r = editor.InsertBlock(NodeId("nope"), std::nullopt, q);
    PF_CHECK(!r.ok());
}

PF_TEST(DocumentIndexRebuild) {
    const Document doc = MakeSampleDoc();
    DocumentIndex index;
    index.Rebuild(doc);
    PF_CHECK(index.Size() == 4);  // section 0 中的 2 个 section + 2 个 block
    size_t expected = doc.CollectNodeIds().size();
    PF_CHECK(index.Size() == expected);

    NodeId sid = doc.body().sections[0].id;
    auto loc = index.Find(sid);
    PF_CHECK(loc.has_value());
    PF_CHECK(loc->kind == NodeKind::Section);

    // block 位置
    for (const auto& block : doc.body().sections[0].blocks) {
        NodeId bid = std::visit([](const auto& b) { return b.id; }, block);
        auto bloc = index.Find(bid);
        PF_CHECK(bloc.has_value());
        PF_CHECK(bloc->parent == sid);
    }
}

PF_TEST(InlineTextHelpers) {
    auto content = InlineFromText("Hello");
    PF_CHECK(InlineToPlainText(content) == "Hello");
    PF_CHECK(!InlineIsBlank(content));
    PF_CHECK(InlineIsBlank(InlineContent{}));
    PF_CHECK(InlineIsBlank(InlineFromText("   ")));

    TextRun bold;
    bold.text = "bold";
    SetMark(bold.marks, TextMark::Strong, true);
    PF_CHECK(HasMark(bold.marks, TextMark::Strong));
    PF_CHECK(!HasMark(bold.marks, TextMark::Emphasis));
}

PF_TEST(ReflowHardWrappedText) {
    // 从 PDF 复制出的段落会按固定列宽预先换行；
    // 必须把这里的单个换行变成空格，它才能重新排版。
    const std::string pasted =
        "The success of deep learning in infrared small\n"
        "target detection relies on large-scale annotations, yet\n"
        "their acquisition cost impedes further progress.";
    const std::string reflowed = ReflowHardWrappedText(pasted);
    PF_CHECK(reflowed.find('\n') == std::string::npos);
    PF_CHECK(reflowed.find("infrared small target detection") !=
             std::string::npos);
    PF_CHECK(reflowed.find("  ") == std::string::npos);

    // 空行是真正的段落分隔，必须保留。
    const std::string two_paragraphs =
        "First paragraph that is long enough to look wrapped.\n"
        "It continues here on a second line of the same paragraph.\n"
        "\n"
        "Second paragraph that is also long enough to look wrapped.\n"
        "And it has a continuation line as well, right here.";
    const std::string joined = ReflowHardWrappedText(two_paragraphs);
    PF_CHECK(joined.find("\n\n") != std::string::npos);
    PF_CHECK(joined.find("First paragraph that is long enough to look "
                         "wrapped. It continues here") != std::string::npos);
    PF_CHECK(joined.find("Second paragraph that is also long enough") !=
             std::string::npos);

    // 有意设置的结构永远不会被重写。
    const std::string bullets =
        "- first item of a list that is long enough to wrap somewhere\n"
        "- second item of the same list, also long enough to wrap\n"
        "- third item, likewise long enough to look like a hard wrap";
    PF_CHECK(ReflowHardWrappedText(bullets) == bullets);

    const std::string enumerated =
        "1. first numbered item that is long enough to look wrapped\n"
        "2. second numbered item, also long enough to look wrapped\n"
        "3. third numbered item, likewise long enough to be wrapped";
    PF_CHECK(ReflowHardWrappedText(enumerated) == enumerated);

    // 显式的 LaTeX 换行属于内容，而不是自动换行。
    const std::string explicit_break =
        "first line that ends with an explicit break\\\\\n"
        "second line that is long enough to look like a hard wrap\n"
        "third line that is also long enough to be considered wrapped";
    PF_CHECK(ReflowHardWrappedText(explicit_break) == explicit_break);

    // 太短，不足以判定为预先换行的文本块：完全按输入原样保留。
    const std::string typed = "line one\nline two";
    PF_CHECK(ReflowHardWrappedText(typed) == typed);
    PF_CHECK(ReflowHardWrappedText("single line") == "single line");
    PF_CHECK(ReflowHardWrappedText("") == "");
}

PF_TEST(StrongIdsAreDistinct) {
    NodeId n1 = IdGenerator::NewNode();
    NodeId n2 = IdGenerator::NewNode();
    PF_CHECK(n1 != n2);
    PF_CHECK(n1 == n1);
}
