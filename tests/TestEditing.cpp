// Editing protocol tests: revisions, undo/redo, anchors.
#include "TestMain.hpp"

#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include <memory>
#include "editing/AnchorResolver.h"
#include "editing/EditingSystem.h"
#include "project/ProjectState.h"

using namespace pf;

namespace {

struct TestHost {
    ProjectState state;
    EditingSystem::Host host;
    std::unique_ptr<EditingSystem> editing;
    std::vector<DocumentChangedEvent> events;

    TestHost() {
        state.SetId(ProjectId("test-project"));
        state.mutable_template() = "generic-article";
        EditingSystem::Host h;
        h.project_id = [this] { return state.id(); };
        h.revision = [this] { return state.revision(); };
        h.bump_revision = [this] { return state.BumpRevision(); };
        h.document = [this]() -> Document& { return state.mutable_document(); };
        h.on_document_changed = [this](const DocumentChangedEvent& e) {
            events.push_back(e);
        };
        host = std::move(h);
        editing = std::make_unique<EditingSystem>(host);
    }

    EditCommand Cmd(FullEditPayload payload) {
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = state.id();
        cmd.base_revision = state.revision();
        cmd.origin = EditOrigin::User;
        cmd.payload = std::move(payload);
        return cmd;
    }
};

}  // namespace

PF_TEST(EditingSystemRevisionMonotonic) {
    TestHost h;
    auto r1 = h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("T")}));
    PF_CHECK(r1.status == EditStatus::Applied);
    PF_CHECK(r1.new_revision.value == 1);
    auto r2 = h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("T2")}));
    PF_CHECK(r2.new_revision.value == 2);
    PF_CHECK(h.state.document().version().value == 2);
}

PF_TEST(EditingSystemRejectsStaleRevision) {
    TestHost h;
    h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("T")}));
    EditCommand stale = h.Cmd(SetTitlePayload{InlineFromText("T2")});
    stale.base_revision = ProjectRevision{0};
    auto r = h.editing->Apply(stale);
    PF_CHECK(r.status == EditStatus::Rejected);
    PF_CHECK(r.failure == FailureReason::StaleOperation);
}

PF_TEST(EditingSystemUndoRedoTitle) {
    TestHost h;
    h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("V1")}));
    h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("V2")}));
    PF_CHECK(InlineToPlainText(h.state.document().front_matter().title) == "V2");

    auto undo = h.editing->Undo();
    PF_CHECK(undo.status == EditStatus::Applied);
    PF_CHECK(InlineToPlainText(h.state.document().front_matter().title) == "V1");
    // Undo produced a NEW revision (monotonic).
    PF_CHECK(h.state.revision().value == 3);

    auto redo = h.editing->Redo();
    PF_CHECK(redo.status == EditStatus::Applied);
    PF_CHECK(InlineToPlainText(h.state.document().front_matter().title) == "V2");
    PF_CHECK(h.state.revision().value == 4);
}

PF_TEST(EditingSystemInsertSectionAndEvent) {
    TestHost h;
    auto sec = h.editing->Apply(h.Cmd(InsertSectionPayload{0, InlineFromText("S")}));
    PF_CHECK(sec.status == EditStatus::Applied);
    PF_CHECK(sec.created_node != NodeId());
    {
        const Document& doc = h.state.document();
        PF_CHECK(doc.body().sections.size() == 1);
        PF_CHECK(doc.body().sections[0].id == sec.created_node);
    }
    PF_CHECK(h.events.size() == 1);
    PF_CHECK(h.events[0].new_revision.value == 1);
}

PF_TEST(EditingSystemMovesSubsections) {
    TestHost h;
    auto section =
        h.editing->Apply(h.Cmd(InsertSectionPayload{0, InlineFromText("S")}));
    h.editing->Apply(h.Cmd(
        InsertSubsectionPayload{0, 0, InlineFromText("First")}));
    h.editing->Apply(h.Cmd(
        InsertSubsectionPayload{0, 1, InlineFromText("Second")}));
    auto moved =
        h.editing->Apply(h.Cmd(MoveSubsectionPayload{0, 0, 2}));
    PF_CHECK(moved.status == EditStatus::Applied);
    const auto& subsections =
        h.state.document().body().sections[0].subsections;
    PF_CHECK(InlineToPlainText(subsections[0].title) == "Second");
    PF_CHECK(InlineToPlainText(subsections[1].title) == "First");
    (void)section;
}

PF_TEST(EditingSystemTemplateChangeKeepsDocumentVersion) {
    TestHost h;
    h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("T")}));
    DocumentVersion v_before = h.state.document().version();
    ProjectRevision r_before = h.state.revision();

    ChangeTemplatePayload tpl;
    tpl.template_id = "ieee-conference";
    auto r = h.editing->Apply(h.Cmd(tpl));
    PF_CHECK(r.status == EditStatus::Applied);

    // DocumentVersion unchanged, ProjectRevision +1 (rule 补充 8).
    PF_CHECK(h.state.document().version() == v_before);
    PF_CHECK(h.state.revision().value == r_before.value + 1);
}

PF_TEST(EditingSystemEvents) {
    TestHost h;
    h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("T")}));
    PF_CHECK(h.events.size() == 1);
    PF_CHECK(h.events[0].old_revision.value == 0);
    PF_CHECK(h.events[0].new_revision.value == 1);
    PF_CHECK(h.events[0].origin == EditOrigin::User);
}

PF_TEST(AnchorResolverAfterBlock) {
    TestHost h;
    auto sec = h.editing->Apply(h.Cmd(InsertSectionPayload{0, InlineFromText("S")}));
    NodeId sid = sec.created_node;
    auto p1 = h.editing->Apply(
        h.Cmd(InsertParagraphPayload{sid, std::nullopt, InlineFromText("first")}));

    AnchorResolver resolver;
    StableNodeAnchor anchor;
    anchor.reference_node = p1.created_node;
    anchor.bias = AnchorBias::After;
    auto resolved = resolver.Resolve(h.state.document(), anchor);
    PF_CHECK(resolved.ok());
    PF_CHECK(resolved.value().parent == sid);
    PF_CHECK(resolved.value().index.has_value());
    // Inserted after first => index 1
    PF_CHECK(*resolved.value().index == 1);

    // Missing reference node
    StableNodeAnchor bad;
    bad.reference_node = NodeId("ghost");
    auto failed = resolver.Resolve(h.state.document(), bad);
    PF_CHECK(!failed.ok());
}

PF_TEST(UndoHistoryCanRedo) {
    TestHost h;
    PF_CHECK(!h.editing->history().CanUndo());
    h.editing->Apply(h.Cmd(SetTitlePayload{InlineFromText("T")}));
    PF_CHECK(h.editing->history().CanUndo());
    PF_CHECK(!h.editing->history().CanRedo());
    h.editing->Undo();
    PF_CHECK(h.editing->history().CanRedo());
    h.editing->Redo();
    PF_CHECK(!h.editing->history().CanRedo());
}

PF_TEST(EditingSystemSnapshotUndoRestoresDeletion) {
    TestHost h;
    // Build two sections with paragraphs.
    auto s1 = h.editing->Apply(h.Cmd(InsertSectionPayload{0, InlineFromText("One")}));
    auto p1 = h.editing->Apply(
        h.Cmd(InsertParagraphPayload{s1.created_node, std::nullopt,
                                     InlineFromText("first")}));
    auto s2 = h.editing->Apply(h.Cmd(InsertSectionPayload{1, InlineFromText("Two")}));

    // Delete section 0 (historically non-invertible; snapshot undo fixes it).
    auto del = h.editing->Apply(h.Cmd(DeleteSectionPayload{0}));
    PF_CHECK(del.status == EditStatus::Applied);
    {
        const Document& doc = h.state.document();
        PF_CHECK(doc.body().sections.size() == 1);
        PF_CHECK(doc.body().sections[0].title.size() > 0);
    }

    // Undo restores the deleted section AND its content.
    auto undo = h.editing->Undo();
    PF_CHECK(undo.status == EditStatus::Applied);
    {
        const Document& doc = h.state.document();
        PF_CHECK(doc.body().sections.size() == 2);
        PF_CHECK(InlineToPlainText(doc.body().sections[0].title) == "One");
        // Paragraph content preserved.
        PF_CHECK(doc.body().sections[0].blocks.size() == 1);
        const auto* para = std::get_if<Paragraph>(&doc.body().sections[0].blocks[0]);
        PF_CHECK(para != nullptr);
        PF_CHECK(InlineToPlainText(para->content) == "first");
    }

    // Redo re-deletes.
    auto redo = h.editing->Redo();
    PF_CHECK(redo.status == EditStatus::Applied);
    PF_CHECK(h.state.document().body().sections.size() == 1);

    // NodeId stability across undo: the restored section keeps its id.
    auto undo2 = h.editing->Undo();
    PF_CHECK(undo2.status == EditStatus::Applied);
    PF_CHECK(h.state.document().body().sections[0].id == s1.created_node);
    PF_CHECK(h.state.document().body().sections[0].blocks[0].index() == 0);
    const auto* para =
        std::get_if<Paragraph>(&h.state.document().body().sections[0].blocks[0]);
    PF_CHECK(para != nullptr);
    PF_CHECK(para->id == p1.created_node);  // NodeId stable through undo
}

PF_TEST(EditingSystemEquationEditUndo) {
    TestHost h;
    auto s = h.editing->Apply(h.Cmd(InsertSectionPayload{0, InlineFromText("S")}));
    auto eq = h.editing->Apply(h.Cmd(InsertEquationPayload{
        s.created_node, std::nullopt, "E = mc^2", true}));

    h.editing->Apply(
        h.Cmd(EditEquationPayload{eq.created_node, "F = ma", std::nullopt}));
    {
        const Document& doc = h.state.document();
        const auto& block = doc.body().sections[0].blocks[0];
        PF_CHECK(std::get<EquationBlock>(block).expression.latex == "F = ma");
    }
    h.editing->Undo();
    {
        const Document& doc = h.state.document();
        const auto& block = doc.body().sections[0].blocks[0];
        PF_CHECK(std::get<EquationBlock>(block).expression.latex == "E = mc^2");
        PF_CHECK(std::get<EquationBlock>(block).id == eq.created_node);
    }
}
