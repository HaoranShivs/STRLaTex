// Document model tests: schema, editor constraints, index.
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

    DisplayEquation eq;
    eq.math_source = "x = y + 1";
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

    // Empty columns are rejected
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

    // Move A from s1 to s2.
    PF_CHECK(editor.MoveBlock(pa.value(), s2, 0).ok());
    {
        const Document& cd = doc;
        PF_CHECK(cd.body().sections[0].blocks.size() == 3);  // B + para + eq
        PF_CHECK(cd.body().sections[1].blocks.size() == 2);  // C + A
    }

    Block* moved = editor.FindBlock(pa.value());
    auto* p = std::get_if<Paragraph>(moved);
    PF_CHECK(p != nullptr);
    PF_CHECK(InlineToPlainText(p->content) == "A");
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

    // Invalid parent rejected
    Paragraph q;
    q.content = InlineFromText("bad");
    auto r = editor.InsertBlock(NodeId("nope"), std::nullopt, q);
    PF_CHECK(!r.ok());
}

PF_TEST(DocumentIndexRebuild) {
    const Document doc = MakeSampleDoc();
    DocumentIndex index;
    index.Rebuild(doc);
    PF_CHECK(index.Size() == 4);  // 2 sections + 2 blocks in section 0
    size_t expected = doc.CollectNodeIds().size();
    PF_CHECK(index.Size() == expected);

    NodeId sid = doc.body().sections[0].id;
    auto loc = index.Find(sid);
    PF_CHECK(loc.has_value());
    PF_CHECK(loc->kind == NodeKind::Section);

    // Block location
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

PF_TEST(StrongIdsAreDistinct) {
    NodeId n1 = IdGenerator::NewNode();
    NodeId n2 = IdGenerator::NewNode();
    PF_CHECK(n1 != n2);
    PF_CHECK(n1 == n1);
}
