// Build pipeline tests with MockCompiler (no real tectonic):
// latest-wins, revision checks, diagnostic mapping, validation.
#include "TestMain.hpp"

#include <filesystem>

#include "build/BuildCoordinator.h"
#include "build/Compiler.h"
#include "build/DiagnosticMapper.h"
#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "editing/EditingSystem.h"
#include "editing/EditCommand.h"
#include "document/InlineText.h"
#include "project/ProjectState.h"
#include "render/LatexRenderer.h"
#include "template/TemplateRegistry.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

struct BuildTestRig {
    ProjectState state;
    std::unique_ptr<MockCompiler> compiler;
    std::unique_ptr<BuildCoordinator> coordinator;
    std::vector<BuildResult> results;
    std::atomic<bool> done{false};

    explicit BuildTestRig(bool compiler_succeeds = true,
                          std::chrono::milliseconds debounce =
                              std::chrono::milliseconds{0}) {
        state.SetId(ProjectId("p-build"));
        compiler = std::make_unique<MockCompiler>(compiler_succeeds);
        BuildCoordinator::Host host;
        host.project_id = [this] { return state.id(); };
        host.workspace_root = [] {
            auto p = std::filesystem::temp_directory_path() / "pf-build-test";
            return p.string();
        };
        host.on_build_finished = [this](const BuildResult& r) {
            results.push_back(r);
            done.store(true);
        };
        coordinator = std::make_unique<BuildCoordinator>(std::move(host),
                                                         compiler.get());
        coordinator->set_debounce(debounce);
    }

    BuildSnapshot MakeSnapshot(ProjectRevision rev) {
        auto doc = std::make_shared<Document>();
        DocumentEditor editor(*doc);
        editor.SetTitle(InlineFromText("Build Test"));
        editor.InsertSection(0, InlineFromText("S"));
        BuildSnapshot snap;
        snap.project_id = state.id();
        snap.snapshot_id = IdGenerator::NewSnapshotId();
        snap.build_id = BuildId(IdGenerator::NewBuildId());
        snap.revision = rev;
        snap.document = doc;
        snap.template_id = "generic-article";
        return snap;
    }

    bool WaitForResult(int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done.load();
    }
};

}  // namespace

PF_TEST(BuildSuccessWithMockCompiler) {
    BuildTestRig rig(true);
    auto snapshot = rig.MakeSnapshot(ProjectRevision{1});
    const std::string snapshot_id = snapshot.snapshot_id;
    const BuildId build_id = snapshot.build_id;
    rig.coordinator->RequestBuild(std::move(snapshot), true);
    PF_CHECK(rig.WaitForResult());
    PF_CHECK(rig.results.size() == 1);
    PF_CHECK(rig.results[0].outcome == BuildResult::Outcome::Success);
    PF_CHECK(!rig.results[0].pdf_path.empty());
    PF_CHECK(rig.results[0].revision.value == 1);
    // The result is attributable to the exact snapshot + build attempt.
    PF_CHECK(rig.results[0].snapshot_id == snapshot_id);
    PF_CHECK(rig.results[0].build_id == build_id);
    PF_CHECK(rig.results[0].project_id == rig.state.id());
}

PF_TEST(BuildFailureProducesDiagnostics) {
    BuildTestRig rig(false);
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{1}), true);
    PF_CHECK(rig.WaitForResult());
    PF_CHECK(rig.results.size() == 1);
    PF_CHECK(rig.results[0].outcome == BuildResult::Outcome::Failure);
    // MockCompiler emits "main.tex:1: simulated error" -> mapped diagnostic
    PF_CHECK(!rig.results[0].diagnostics.empty());
    PF_CHECK(rig.results[0].diagnostics[0].source == DiagnosticSource::Compiler);
}

PF_TEST(BuildLatestWinsPending) {
    // Slow debounce: post multiple snapshots; only the latest should build.
    BuildTestRig rig(true, std::chrono::milliseconds{150});
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{1}));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{2}));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{3}));
    PF_CHECK(rig.WaitForResult(8000));
    // Allow a possible second build cycle.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    // Rev 3 (latest) must be among the results.
    bool has_rev3 = false;
    for (const auto& r : rig.results) {
        if (r.revision.value == 3 &&
            r.outcome == BuildResult::Outcome::Success) {
            has_rev3 = true;
        }
    }
    PF_CHECK(has_rev3);
    // Rev 1 must never complete (it was superseded during debounce).
    for (const auto& r : rig.results) {
        if (r.revision.value == 1) {
            PF_CHECK(r.outcome != BuildResult::Outcome::Success);
        }
    }
}

PF_TEST(DiagnosticMapperMapsLineToNode) {
    // Compile result with an error at a known line; source map pointing
    // there resolves to a node id.
    SourceMap smap;
    GeneratedSourceRange range;
    range.file = "main.tex";
    range.begin_line = 120;
    range.end_line = 123;
    smap.AddMapping(range, NodeId("eq12"));

    CompileResult cres;
    cres.status = CompileStatus::Failure;
    CompilerMessage msg;
    msg.file = "main.tex";
    msg.line = 132;  // inside the mapped range neighborhood
    msg.is_error = true;
    msg.text = "Undefined control sequence";
    cres.messages.push_back(msg);

    DiagnosticMapper mapper;
    auto diagnostics = mapper.Map(cres, smap, ProjectRevision{5});
    PF_CHECK(diagnostics.size() == 1);
    PF_CHECK(diagnostics[0].source == DiagnosticSource::Compiler);
    PF_CHECK(diagnostics[0].severity == DiagnosticSeverity::Error);
    PF_CHECK(diagnostics[0].revision.value == 5);
    // Mapped through SourceMap to the equation node.
    PF_CHECK(diagnostics[0].location.kind == DiagnosticLocationKind::Node);
    PF_CHECK(diagnostics[0].location.node == NodeId("eq12"));
}

PF_TEST(ValidatorMissingCitation) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    Citation cit;
    cit.keys = {"unknown_key"};
    p.content.push_back(cit);
    editor.InsertBlock(s.value(), std::nullopt, p);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.bibliography_keys = {"known_key"};
    input.revision = ProjectRevision{9};

    auto result = validator.Validate(input);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E-MISSING-CITATION") found = true;
    }
    PF_CHECK(found);
    PF_CHECK(result.can_render);  // semantic issues do not block rendering
}

PF_TEST(ValidatorDanglingCrossReference) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    CrossReference ref;
    ref.target = NodeId("deleted-node");
    p.content.push_back(ref);
    editor.InsertBlock(s.value(), std::nullopt, p);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.revision = ProjectRevision{11};

    auto result = validator.Validate(input);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E-MISSING-XREF-TARGET") found = true;
    }
    PF_CHECK(found);
}

PF_TEST(ValidatorEmptyTitleWarning) {
    Document doc;
    DocumentEditor editor(doc);
    editor.InsertSection(0, InlineFromText("S"));

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    auto result = validator.Validate(input);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "W-EMPTY-TITLE") found = true;
    }
    PF_CHECK(found);
}

PF_TEST(ValidatorTemplateRequiredFields) {
    // IEEE template requires authors/affiliations/abstract/keywords.
    Document doc;
    DocumentEditor editor(doc);
    (void)editor.SetTitle(InlineFromText("Title Only"));
    (void)editor.SetAbstract(InlineFromText("Has abstract"));
    // No authors, no affiliations, no keywords.

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "ieee-conference";
    input.revision = ProjectRevision{1};

    auto result = validator.Validate(input);
    auto has = [&result](const char* code) {
        for (const auto& d : result.diagnostics) {
            if (d.code == code) return true;
        }
        return false;
    };
    PF_CHECK(has("W-REQ-AUTHORS"));
    PF_CHECK(has("W-REQ-AFFILIATIONS"));
    PF_CHECK(has("W-REQ-KEYWORDS"));
    PF_CHECK(!has("W-REQ-TITLE"));       // title present
    PF_CHECK(!has("W-REQ-ABSTRACT"));    // abstract present

    // generic-article requires authors (default) but not affiliations.
    input.template_id = "generic-article";
    result = validator.Validate(input);
    PF_CHECK(has("W-REQ-AUTHORS"));
    PF_CHECK(!has("W-REQ-AFFILIATIONS"));
    PF_CHECK(!has("W-REQ-KEYWORDS"));
}

namespace {
size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}
}  // namespace

PF_TEST(AuthorsAffiliationsRenderWithThanks) {
    Document doc;
    DocumentEditor editor(doc);
    (void)editor.SetTitle(InlineFromText("Authored Paper"));

    auto aff0 = editor.AddAffiliation("University One");
    auto aff1 = editor.AddAffiliation("Institute Two");
    PF_CHECK(aff0.ok());
    PF_CHECK(aff1.ok());

    Author alice;
    alice.name = "Alice";
    alice.affiliations = {aff0.value()};
    PF_CHECK(editor.AddAuthor(alice).ok());
    Author bob;
    bob.name = "Bob";
    bob.affiliations = {aff1.value()};
    PF_CHECK(editor.AddAuthor(bob).ok());
    (void)editor.SetKeywords({"latex", "structure"});

    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{3};
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    PF_CHECK(!rendered.package.files.empty());
    const std::string& tex = rendered.package.files[0].content;

    // Authors carry the number of their institution.
    PF_CHECK(tex.find("\\author{Alice\\textsuperscript{1} \\and "
                      "Bob\\textsuperscript{2}") != std::string::npos);
    // Both institutions are listed once, numbered, inside a single \thanks.
    PF_CHECK(tex.find("\\thanks{\\textsuperscript{1} University One \\\\ "
                      "\\textsuperscript{2} Institute Two}") !=
             std::string::npos);
    // ... and never repeated per author.
    PF_CHECK(CountOccurrences(tex, "University One") == 1);
    PF_CHECK(CountOccurrences(tex, "Institute Two") == 1);
    PF_CHECK(tex.find("Keywords:") != std::string::npos);
}

PF_TEST(SetAffiliationsPayloadEditing) {
    Document doc;
    ProjectState state;
    state.SetId(ProjectId("p"));
    EditingSystem::Host host;
    host.project_id = [&] { return state.id(); };
    host.revision = [&] { return state.revision(); };
    host.bump_revision = [&] { return state.BumpRevision(); };
    host.document = [&]() -> Document& { return state.mutable_document(); };
    EditingSystem editing(host);

    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = state.id();
    cmd.base_revision = state.revision();
    SetAffiliationsPayload aff_p;
    Affiliation a1;
    a1.id = AffiliationId("aff0");
    a1.name = "University One";
    aff_p.affiliations = {a1};
    cmd.payload = aff_p;
    auto result = editing.Apply(cmd);
    PF_CHECK(result.status == EditStatus::Applied);
    PF_CHECK(state.document().front_matter().affiliations.size() == 1);
    PF_CHECK(state.document().front_matter().affiliations[0].name ==
             "University One");

    // A second institution can be added, and authors pointing at the first one
    // keep a resolving link.
    // The front matter is only mutable through the editing system, so the
    // author goes in the same way the GUI adds one.
    {
        Author author;
        author.name = "Alice";
        author.affiliations = {AffiliationId("aff0")};
        EditCommand author_cmd;
        author_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        author_cmd.project_id = state.id();
        author_cmd.base_revision = state.revision();
        AddAuthorPayload add_author;
        add_author.author = author;
        author_cmd.payload = add_author;
        PF_CHECK(editing.Apply(author_cmd).status == EditStatus::Applied);
    }
    Affiliation a2;
    a2.id = AffiliationId("aff1");
    a2.name = "Institute Two";
    EditCommand add_cmd;
    add_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    add_cmd.project_id = state.id();
    add_cmd.base_revision = state.revision();
    SetAffiliationsPayload two;
    two.affiliations = {a1, a2};
    add_cmd.payload = two;
    PF_CHECK(editing.Apply(add_cmd).status == EditStatus::Applied);
    PF_CHECK(state.document().front_matter().affiliations.size() == 2);
    PF_CHECK(state.document().front_matter().authors.size() == 1);
    PF_CHECK(state.document().front_matter().authors[0].affiliations.size() == 1);
    PF_CHECK(state.document().front_matter().authors[0].affiliations[0] ==
             AffiliationId("aff0"));

    // Shortening the list drops the link instead of leaving a dangling id.
    EditCommand drop_cmd;
    drop_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    drop_cmd.project_id = state.id();
    drop_cmd.base_revision = state.revision();
    SetAffiliationsPayload only_second;
    only_second.affiliations = {a2};
    drop_cmd.payload = only_second;
    PF_CHECK(editing.Apply(drop_cmd).status == EditStatus::Applied);
    PF_CHECK(state.document().front_matter().authors[0].affiliations.empty());
}
