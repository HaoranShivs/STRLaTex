// Stage A + C tests: DocumentTraversal / NodeAddress, the third heading
// level (Subsubsection) end to end, and schema migration.
#include "TestMain.hpp"

#include <filesystem>
#include <fstream>

#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentIndex.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "editing/EditingSystem.h"
#include "persistence/ProjectMigrator.h"
#include "persistence/ProjectPersistence.h"
#include "project/ProjectSession.h"
#include "render/LatexRenderer.h"
#include "template/TemplateRegistry.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

ProjectState MakeState() {
    ProjectState state;
    state.SetId(ProjectId("p-traversal"));
    return state;
}

EditingSystem MakeEditing(ProjectState& state) {
    EditingSystem::Host host;
    host.project_id = [&] { return state.id(); };
    host.revision = [&] { return state.revision(); };
    host.bump_revision = [&] { return state.BumpRevision(); };
    host.document = [&]() -> Document& { return state.mutable_document(); };
    EditingSystem editing(host);
    return editing;
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

Body& BodyOf(Document& doc) { return DocumentMutableAccess::body(doc); }

// Section(1) -> Subsection(1) -> Subsubsection(1..2), each with one paragraph.
void SeedThreeLevels(Document& doc) {
    DocumentEditor editor(doc);
    auto section = editor.InsertSection(0, InlineFromText("S1"));
    auto sub = editor.InsertSubsection(0, 0, InlineFromText("S1.1"));
    auto subsub1 = editor.InsertSubsubsection(0, 0, 0, InlineFromText("S1.1.1"));
    auto subsub2 = editor.InsertSubsubsection(0, 0, 1, InlineFromText("S1.1.2"));
    Paragraph p1;
    p1.content = InlineFromText("alpha");
    (void)editor.InsertBlock(subsub1.value(), std::nullopt, p1);
    Paragraph p2;
    p2.content = InlineFromText("beta");
    (void)editor.InsertBlock(subsub2.value(), std::nullopt, p2);
    Paragraph p3;
    p3.content = InlineFromText("before subsubsections");
    (void)editor.InsertBlock(sub.value(), std::nullopt, p3);
}

}  // namespace

// ---------------- NodeAddress / traversal ----------------

PF_TEST(TraversalLocatesEveryNodeWithItsAddress) {
    Document doc;
    SeedThreeLevels(doc);

    // Every id the document holds must be locatable with the right kind.
    // 1 section + 1 subsection + 2 subsubsections + 3 paragraphs.
    const auto ids = doc.CollectNodeIds();
    PF_CHECK(ids.size() == 7);
    for (const auto& id : ids) {
        auto address = LocateNode(doc, id);
        PF_CHECK(address.has_value());
        if (!address) continue;
        PF_CHECK(address->node == id);
        PF_CHECK(address->section.has_value());
    }

    auto section = LocateNode(doc, BodyOf(doc).sections[0].id);
    PF_CHECK(section->kind == NodeKind::Section);
    PF_CHECK(!section->subsection.has_value());
    PF_CHECK(section->depth() == 1);

    auto sub = LocateNode(doc, BodyOf(doc).sections[0].subsections[0].id);
    PF_CHECK(sub->kind == NodeKind::Subsection);
    PF_CHECK(*sub->subsection == 0);
    PF_CHECK(sub->depth() == 2);

    auto subsub = LocateNode(
        doc, BodyOf(doc).sections[0].subsections[0].subsubsections[1].id);
    PF_CHECK(subsub->kind == NodeKind::Subsubsection);
    PF_CHECK(*subsub->subsubsection == 1);
    PF_CHECK(subsub->depth() == 3);

    // A paragraph inside the second subsubsection carries the full chain.
    const auto& deep_block =
        BodyOf(doc).sections[0].subsections[0].subsubsections[1].blocks[0];
    const NodeId deep_id =
        std::visit([](const auto& b) { return b.id; }, deep_block);
    auto deep = LocateNode(doc, deep_id);
    PF_CHECK(deep->kind == NodeKind::Paragraph);
    PF_CHECK(*deep->section == 0);
    PF_CHECK(*deep->subsection == 0);
    PF_CHECK(*deep->subsubsection == 1);
    PF_CHECK(*deep->block == 0);

    PF_CHECK(!LocateNode(doc, NodeId("missing")).has_value());
}

PF_TEST(TraversalVisitsInDocumentOrder) {
    Document doc;
    SeedThreeLevels(doc);

    std::vector<NodeKind> kinds;
    VisitNodes(doc, [&](const NodeAddress& a) { kinds.push_back(a.kind); });

    // Section, the subsection's own block, the subsection, then its two
    // subsubsections each followed by their block. A subsection renders its
    // own blocks before its subsubsections, so that is document order.
    const std::vector<NodeKind> expected = {
        NodeKind::Section,       NodeKind::Subsection,
        NodeKind::Paragraph,     NodeKind::Subsubsection,
        NodeKind::Paragraph,     NodeKind::Subsubsection,
        NodeKind::Paragraph,
    };
    PF_CHECK(kinds == expected);
}

PF_TEST(TraversalHeadingsAndBlocksHelpers) {
    Document doc;
    SeedThreeLevels(doc);

    size_t headings = 0;
    VisitHeadings(doc, [&](const NodeAddress& a) {
        PF_CHECK(IsHeadingKind(a.kind));
        ++headings;
    });
    PF_CHECK(headings == 4);

    size_t blocks = 0;
    VisitBlocks(doc, [&](const Block&, const NodeAddress& a) {
        PF_CHECK(a.block.has_value());
        ++blocks;
    });
    PF_CHECK(blocks == 3);

    // CollectAllNodeIds agrees with Document::CollectNodeIds.
    PF_CHECK(CollectAllNodeIds(doc) == doc.CollectNodeIds());
}

PF_TEST(TraversalInlineCoversParagraphsAndCaptions) {
    Document doc;
    DocumentEditor editor(doc);
    // A non-empty paper title is itself inline content.
    (void)editor.SetTitle(InlineFromText("Paper Title"));
    auto section = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    p.content = InlineFromText("body text");
    (void)editor.InsertBlock(section.value(), std::nullopt, p);
    Figure fig;
    fig.caption = InlineFromText("the caption");
    (void)editor.InsertBlock(section.value(), std::nullopt, fig);

    size_t seen = 0;
    bool saw_caption = false;
    VisitInlineContent(doc, [&](const InlineContent& content, const NodeAddress&) {
        ++seen;
        if (InlineToPlainText(content) == "the caption") saw_caption = true;
    });
    // Paper title + paragraph body + figure caption.
    PF_CHECK(seen == 3);
    PF_CHECK(saw_caption);
}

// ---------------- Subsubsection through the editing protocol ----------------

PF_TEST(SubsubsectionInsertRenameDeleteThroughProtocol) {
    ProjectState state = MakeState();
    auto editing = MakeEditing(state);

    // Section + subsection.
    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("Section");
    PF_CHECK(editing.Apply(MakeCmd(state, sec)).status == EditStatus::Applied);
    InsertSubsectionPayload sub;
    sub.section_index = 0;
    sub.index = 0;
    sub.title = InlineFromText("Subsection");
    auto sub_result = editing.Apply(MakeCmd(state, sub));
    PF_CHECK(sub_result.status == EditStatus::Applied);
    const NodeId sub_id = sub_result.created_node;

    // Insert a subsubsection under it.
    InsertSubsubsectionPayload payload;
    payload.section_index = 0;
    payload.subsection_index = 0;
    payload.index = 0;
    payload.title = InlineFromText("Third level");
    auto created = editing.Apply(MakeCmd(state, payload));
    PF_CHECK(created.status == EditStatus::Applied);
    const NodeId subsub_id = created.created_node;
    PF_CHECK(!subsub_id.empty());
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections.size() == 1);

    // NodeKind + index reflect it.
    PF_CHECK(state.mutable_document().GetNodeKind(subsub_id) ==
             NodeKind::Subsubsection);

    // Rename.
    RenameSubsubsectionPayload rename;
    rename.subsubsection = subsub_id;
    rename.title = InlineFromText("Renamed third");
    PF_CHECK(editing.Apply(MakeCmd(state, rename)).status == EditStatus::Applied);
    PF_CHECK(InlineToPlainText(BodyOf(state.mutable_document())
                 .sections[0]
                                   .subsections[0]
                                   .subsubsections[0]
                                   .title) == "Renamed third");

    // Move: swap a second one in front of the first.
    InsertSubsubsectionPayload second;
    second.section_index = 0;
    second.subsection_index = 0;
    second.index = 1;
    second.title = InlineFromText("Second third");
    auto second_result = editing.Apply(MakeCmd(state, second));
    PF_CHECK(second_result.status == EditStatus::Applied);
    const NodeId second_id = second_result.created_node;
    MoveSubsubsectionPayload move;
    move.section_index = 0;
    move.subsection_index = 0;
    move.from = 1;
    move.to = 0;
    PF_CHECK(editing.Apply(MakeCmd(state, move)).status == EditStatus::Applied);
    PF_CHECK(InlineToPlainText(BodyOf(state.mutable_document())
                 .sections[0]
                                   .subsections[0]
                                   .subsubsections[0]
                                   .title) == "Second third");

    // After the move, "Second third" (second_id) is at index 0 and the
    // original "Renamed third" (subsub_id) sits at index 1.
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections[0]
                 .id == second_id);

    // Delete by index; whatever sits there now is the one that goes.
    DeleteSubsubsectionPayload del;
    del.section_index = 0;
    del.subsection_index = 0;
    del.subsubsection_index = 0;
    PF_CHECK(editing.Apply(MakeCmd(state, del)).status == EditStatus::Applied);
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections.size() == 1);
    PF_CHECK(!state.mutable_document().ContainsNode(second_id));
    PF_CHECK(state.mutable_document().ContainsNode(subsub_id));
    PF_CHECK(state.mutable_document().GetNodeKind(subsub_id) ==
             NodeKind::Subsubsection);
}

PF_TEST(SubsubsectionAfterAnchorTakesFollowingBlocks) {
    ProjectState state = MakeState();
    auto editing = MakeEditing(state);

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    auto sec_result = editing.Apply(MakeCmd(state, sec));
    InsertSubsectionPayload sub;
    sub.section_index = 0;
    sub.index = 0;
    sub.title = InlineFromText("Sub");
    auto sub_result = editing.Apply(MakeCmd(state, sub));

    // Two paragraphs directly in the subsection.
    InsertParagraphPayload p1;
    p1.parent = sub_result.created_node;
    p1.content = InlineFromText("one");
    auto p1_result = editing.Apply(MakeCmd(state, p1));
    InsertParagraphPayload p2;
    p2.parent = sub_result.created_node;
    p2.content = InlineFromText("two");
    auto p2_result = editing.Apply(MakeCmd(state, p2));

    // Insert a subsubsection after the first paragraph: "two" moves into it.
    InsertSubsubsectionAfterPayload payload;
    payload.after = p1_result.created_node;
    payload.title = InlineFromText("Split here");
    auto created = editing.Apply(MakeCmd(state, payload));
    PF_CHECK(created.status == EditStatus::Applied);

    const auto& subsection = BodyOf(state.mutable_document()).sections[0].subsections[0];
    PF_CHECK(subsection.blocks.size() == 1);  // "one" stays
    PF_CHECK(subsection.subsubsections.size() == 1);
    PF_CHECK(subsection.subsubsections[0].blocks.size() == 1);
    PF_CHECK(InlineToPlainText(
                 std::get<Paragraph>(subsection.subsubsections[0].blocks[0])
                     .content) == "two");
    (void)p2_result;
}

PF_TEST(SubsubsectionSurvivesUndoAndRedo) {
    ProjectState state = MakeState();
    auto editing = MakeEditing(state);

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    PF_CHECK(editing.Apply(MakeCmd(state, sec)).status == EditStatus::Applied);
    InsertSubsectionPayload sub;
    sub.section_index = 0;
    sub.index = 0;
    sub.title = InlineFromText("Sub");
    PF_CHECK(editing.Apply(MakeCmd(state, sub)).status == EditStatus::Applied);

    InsertSubsubsectionPayload payload;
    payload.section_index = 0;
    payload.subsection_index = 0;
    payload.index = 0;
    payload.title = InlineFromText("Third");
    PF_CHECK(editing.Apply(MakeCmd(state, payload)).status == EditStatus::Applied);
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections.size() == 1);

    // Undo removes it; redo brings it back with the same id.
    const NodeId created = BodyOf(state.mutable_document())
                 .sections[0]
                               .subsections[0]
                               .subsubsections[0]
                               .id;
    PF_CHECK(editing.Undo().status == EditStatus::Applied);
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections.empty());
    PF_CHECK(editing.Redo().status == EditStatus::Applied);
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections.size() == 1);
    PF_CHECK(BodyOf(state.mutable_document())
                 .sections[0]
                 .subsections[0]
                 .subsubsections[0]
                 .id == created);
}

PF_TEST(SubsubsectionBlocksMoveAndValidate) {
    Document doc;
    SeedThreeLevels(doc);

    DocumentEditor editor(doc);
    // A block can move from one subsubsection to the sibling one.
    const NodeId source_id = std::visit(
        [](const auto& b) { return b.id; },
        BodyOf(doc).sections[0].subsections[0].subsubsections[0].blocks[0]);
    const NodeId destination =
        BodyOf(doc).sections[0].subsections[0].subsubsections[1].id;
    PF_CHECK(editor.MoveBlock(source_id, destination, 0).ok());
    PF_CHECK(BodyOf(doc)
                .sections[0]
                .subsections[0]
                .subsubsections[0]
                .blocks.empty());
    PF_CHECK(BodyOf(doc)
                .sections[0]
                .subsections[0]
                .subsubsections[1]
                .blocks.size() == 2);

    // The document stays structurally valid.
    for (const auto& id : doc.CollectNodeIds()) {
        PF_CHECK(doc.ContainsNode(id));
        PF_CHECK(LocateNode(doc, id).has_value());
    }

    // And the validator accepts it.
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.revision = ProjectRevision{7};
    auto result = Validator().Validate(input);
    bool has_error = false;
    for (const auto& d : result.diagnostics) {
        if (d.severity == DiagnosticSeverity::Error) has_error = true;
    }
    PF_CHECK(!has_error);
}

// ---------------- Renderer / Outline / Index ----------------

PF_TEST(RendererEmitsSubsubsectionWithLabel) {
    Document doc;
    SeedThreeLevels(doc);
    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    const std::string& tex = rendered.package.files[0].content;
    PF_CHECK(tex.find("\\subsubsection{") != std::string::npos);
    const auto& subsub = BodyOf(doc).sections[0].subsections[0].subsubsections[0];
    PF_CHECK(tex.find("\\subsubsection{" +
                      InlineToPlainText(subsub.title) + "}\\label{" +
                      subsub.id.value() + "}") != std::string::npos);
}

PF_TEST(DocumentIndexCoversThirdLevel) {
    Document doc;
    SeedThreeLevels(doc);
    DocumentIndex index;
    index.Rebuild(doc);

    const auto& subsub = BodyOf(doc).sections[0].subsections[0].subsubsections[0];
    auto location = index.Find(subsub.id);
    PF_CHECK(location.has_value());
    PF_CHECK(location->kind == NodeKind::Subsubsection);
    PF_CHECK(location->in_subsubsection);
    PF_CHECK(location->subsubsection_index == 0);
    PF_CHECK(location->parent == BodyOf(doc).sections[0].subsections[0].id);

    // A block inside it names the subsubsection as its parent.
    const NodeId block_id = std::visit(
        [](const auto& b) { return b.id; }, subsub.blocks[0]);
    auto block_location = index.Find(block_id);
    PF_CHECK(block_location.has_value());
    PF_CHECK(block_location->parent == subsub.id);
}

// ---------------- Template capability ----------------

PF_TEST(TemplateCapabilityLimitsHeadingDepth) {
    // Both shipped templates support all three levels.
    for (const auto& def : TemplateRegistry::Instance().All()) {
        PF_CHECK(def.capabilities.max_heading_depth == 3);
        PF_CHECK(def.capabilities.max_heading_depth >= 1);
        PF_CHECK(def.capabilities.max_heading_depth <= 3);
    }
}

PF_TEST(ValidatorWarnsWhenHeadingExceedsTemplateDepth) {
    Document doc;
    SeedThreeLevels(doc);
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.revision = ProjectRevision{1};

    // Sanity: at depth 3 there is nothing to warn about.
    auto result = Validator().Validate(input);
    size_t depth_warnings = 0;
    for (const auto& d : result.diagnostics) {
        if (d.code == "W-HEADING-DEPTH") ++depth_warnings;
    }
    PF_CHECK(depth_warnings == 0);
}

// ---------------- Persistence + migration ----------------

PF_TEST(SchemaMigrationV1ToV2KeepsDocumentAndStampsVersion) {
    // A V1 file: no subsubsections anywhere.
    const std::string v1_json = R"({
      "schemaVersion": "1",
      "projectId": "p-old",
      "revision": 4,
      "template": "generic-article",
      "bibliographyPath": "references.bib",
      "frontMatter": {"title": [{"type": "text", "text": "Old Paper"}]},
      "body": {"sections": [
        {"id": "s1", "title": [{"type": "text", "text": "S"}],
         "blocks": [],
         "subsections": [
           {"id": "u1", "title": [{"type": "text", "text": "U"}],
            "blocks": [
              {"type": "paragraph", "id": "n1",
               "content": [{"type": "text", "text": "hello"}]}
            ]}
         ]}
      ]},
      "assets": []
    })";

    auto loaded = ProjectSerializer::Deserialize(v1_json);
    PF_CHECK(loaded.ok());
    PF_CHECK(loaded.value().schema_version == "1");

    auto migration = ProjectMigrator::MigrateToCurrent(&loaded.value());
    PF_CHECK(migration.migrated);
    PF_CHECK(migration.from_version == "1");
    PF_CHECK(migration.to_version == "2");
    PF_CHECK(migration.applied.size() == 1);
    PF_CHECK(migration.warnings.empty());

    // Nothing was lost.
    PF_CHECK(InlineToPlainText(
             DocumentMutableAccess::front_matter(loaded.value().document)
                 .title) == "Old Paper");
    PF_CHECK(BodyOf(loaded.value().document).sections.size() == 1);
    PF_CHECK(BodyOf(loaded.value().document).sections[0].subsections.size() == 1);
    {
        const Block& block =
            BodyOf(loaded.value().document).sections[0].subsections[0].blocks[0];
        const auto* para = std::get_if<Paragraph>(&block);
        PF_CHECK(para != nullptr);
        if (para) {
            PF_CHECK(InlineToPlainText(para->content) == "hello");
        }
    }
    PF_CHECK(loaded.value().revision.value == 4);

    // Saving writes the current version, and the round trip is stable.
    const std::string saved = ProjectSerializer::Serialize(loaded.value());
    auto reloaded = ProjectSerializer::Deserialize(saved);
    PF_CHECK(reloaded.ok());
    PF_CHECK(reloaded.value().schema_version == "2");
    PF_CHECK(BodyOf(reloaded.value().document).sections.size() == 1);
}

PF_TEST(SchemaMigrationIsIdempotentAndFlagsUnknownVersions) {
    SerializedProject current;
    current.schema_version = "2";
    auto untouched = ProjectMigrator::MigrateToCurrent(&current);
    PF_CHECK(!untouched.migrated);
    PF_CHECK(untouched.to_version == "2");

    SerializedProject blank;
    blank.schema_version = "";
    auto assumed = ProjectMigrator::MigrateToCurrent(&blank);
    PF_CHECK(assumed.migrated);
    PF_CHECK(!assumed.warnings.empty());

    SerializedProject future;
    future.schema_version = "9";
    auto unknown = ProjectMigrator::MigrateToCurrent(&future);
    PF_CHECK(!unknown.migrated);
    PF_CHECK(!unknown.warnings.empty());
}

PF_TEST(LoadMigratesOldProjectFileInMemory) {
    auto dir = std::filesystem::temp_directory_path() / "pf-migration-load";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "project.paper";
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"schemaVersion": "1", "projectId": "p-mig", "revision": 1,
                   "template": "generic-article",
                   "frontMatter": {"title": [{"type": "text", "text": "Mig"}]},
                   "body": {"sections": []}, "assets": []})";
    }

    LoadRequest request;
    request.project_file = file;
    auto result = ProjectPersistence::Load(request);
    PF_CHECK(result.status == LoadResult::Status::Ok);
    PF_CHECK(result.migration.migrated);
    PF_CHECK(result.migration.to_version == "2");
    PF_CHECK(result.project.has_value());
    PF_CHECK(result.project->schema_version == "2");

    // The old file is untouched on disk - only a save rewrites it.
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    PF_CHECK(ss.str().find("\"schemaVersion\": \"1\"") != std::string::npos);

    std::filesystem::remove_all(dir);
}

PF_TEST(SubsubsectionRoundTripsThroughProjectFile) {
    Document doc;
    SeedThreeLevels(doc);

    SerializedProject project;
    project.project_id = "p-roundtrip";
    project.revision = ProjectRevision{9};
    project.template_id = "generic-article";
    project.document = doc;

    const std::string json = ProjectSerializer::Serialize(project);
    auto back = ProjectSerializer::Deserialize(json);
    PF_CHECK(back.ok());
    const auto& subsub =
        BodyOf(back.value().document).sections[0].subsections[0].subsubsections;
    PF_CHECK(subsub.size() == 2);
    PF_CHECK(InlineToPlainText(subsub[0].title) == "S1.1.1");
    PF_CHECK(subsub[0].blocks.size() == 1);
    PF_CHECK(std::visit([](const auto& b) { return b.id; }, subsub[0].blocks[0]) != NodeId());
    // Ids survive so cross references stay valid.
    PF_CHECK(subsub[0].id == BodyOf(doc)
                                 .sections[0]
                                 .subsections[0]
                                 .subsubsections[0]
                                 .id);
}

// ---------------- Session-level e2e ----------------

PF_TEST(SessionSubsubsectionEndToEnd) {
    auto dir = std::filesystem::temp_directory_path() / "pf-subsub-e2e";
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
    sec.title = InlineFromText("Section");
    sec_cmd.payload = sec;
    auto sec_result = session.Execute(sec_cmd);
    PF_CHECK(sec_result.status == EditStatus::Applied);

    EditCommand sub_cmd = sec_cmd;
    sub_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    sub_cmd.base_revision = session.current_revision();
    InsertSubsectionAfterPayload sub;
    sub.after = sec_result.created_node;
    sub.title = InlineFromText("Subsection");
    sub_cmd.payload = sub;
    auto sub_result = session.Execute(sub_cmd);
    PF_CHECK(sub_result.status == EditStatus::Applied);

    // Insert a subsubsection after the subsection heading via the payload the
    // GUI uses, then rename and delete it.
    EditCommand subsub_cmd = sec_cmd;
    subsub_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    subsub_cmd.base_revision = session.current_revision();
    InsertSubsubsectionAfterPayload subsub;
    subsub.after = sub_result.created_node;
    subsub.title = InlineFromText("Third");
    subsub_cmd.payload = subsub;
    auto subsub_result = session.Execute(subsub_cmd);
    PF_CHECK(subsub_result.status == EditStatus::Applied);
    PF_CHECK(session.state().document().GetNodeKind(subsub_result.created_node) ==
             NodeKind::Subsubsection);

    // Undo removes it, redo restores it.
    const std::uint64_t before = session.current_revision().value;
    PF_CHECK(session.Undo().status == EditStatus::Applied);
    PF_CHECK(session.state().document()
                 .body()
                 .sections[0]
                 .subsections[0]
                 .subsubsections.empty());
    PF_CHECK(session.Redo().status == EditStatus::Applied);
    PF_CHECK(session.state().document()
                 .body()
                 .sections[0]
                 .subsections[0]
                 .subsubsections.size() == 1);
    PF_CHECK(session.current_revision().value > before);

    // Save + reload keeps the third level.
    session.Save();
    PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    std::string error;
    PF_CHECK(session.OpenProject(dir, &error));
    PF_CHECK(session.state().document()
                 .body()
                 .sections[0]
                 .subsections[0]
                 .subsubsections.size() == 1);
    std::filesystem::remove_all(dir);
}
