// 使用 MockCompiler 的 build 流水线测试（不涉及真正的 tectonic）：
// latest-wins、revision 校验、诊断映射、validation。
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
    std::vector<BuildEvent> events;
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
        host.on_build_event = [this](const BuildEvent& e) {
            events.push_back(e);
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
    // 结果可精确归属到某个 snapshot + 某次 build 尝试。
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
    // MockCompiler 输出 "main.tex:1: simulated error" -> 映射后的诊断。
    // 合并顺序为 validator、generator、compiler（Build Diagnostics 方案
    // §18），因此 compiler 诊断按 source 查找，而不是按下标。
    PF_CHECK(!rig.results[0].diagnostics.empty());
    const Diagnostic* compiler_diag = nullptr;
    for (const auto& d : rig.results[0].diagnostics) {
        if (d.source == DiagnosticSource::Compiler) compiler_diag = &d;
    }
    PF_CHECK(compiler_diag != nullptr);
    if (compiler_diag) {
        PF_CHECK(compiler_diag->severity == DiagnosticSeverity::Error);
        PF_CHECK(compiler_diag->code == "LATEX_ERROR");
        // 每条诊断都标记了产生它的 build（§12）。
        PF_CHECK(compiler_diag->build_id == rig.results[0].build_id);
        // 即使没有 node map，生成源中的位置也会保留。
        PF_CHECK(compiler_diag->location.file == "main.tex");
        PF_CHECK(compiler_diag->location.line.value_or(0) == 1);
    }
}

PF_TEST(BuildLatestWinsPending) {
    // 慢速 debounce：提交多个 snapshot；只有最新的那个应当被 build。
    BuildTestRig rig(true, std::chrono::milliseconds{150});
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{1}));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{2}));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    rig.coordinator->RequestBuild(rig.MakeSnapshot(ProjectRevision{3}));
    PF_CHECK(rig.WaitForResult(8000));
    // 留出可能出现的第二轮 build 周期。
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    // Rev 3（最新）必须出现在结果中。
    bool has_rev3 = false;
    for (const auto& r : rig.results) {
        if (r.revision.value == 3 &&
            r.outcome == BuildResult::Outcome::Success) {
            has_rev3 = true;
        }
    }
    PF_CHECK(has_rev3);
    // Rev 1 绝不能完成（它在 debounce 期间已被取代）。
    for (const auto& r : rig.results) {
        if (r.revision.value == 1) {
            PF_CHECK(r.outcome != BuildResult::Outcome::Success);
        }
    }
}

PF_TEST(DiagnosticMapperMapsLineToNode) {
    // 编译结果在已知行上有一个错误；指向该处的 source map
    // 会解析为一个 node id。
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
    msg.line = 132;  // 位于映射范围的临近区域内
    msg.is_error = true;
    msg.text = "Undefined control sequence";
    cres.messages.push_back(msg);

    DiagnosticMapper mapper;
    auto diagnostics = mapper.Map(cres, smap, ProjectRevision{5});
    PF_CHECK(diagnostics.size() == 1);
    PF_CHECK(diagnostics[0].source == DiagnosticSource::Compiler);
    PF_CHECK(diagnostics[0].severity == DiagnosticSeverity::Error);
    PF_CHECK(diagnostics[0].revision.value == 5);
    // 通过 SourceMap 映射到该公式节点。
    PF_CHECK(diagnostics[0].location.kind == DiagnosticLocationKind::Node);
    PF_CHECK(diagnostics[0].location.node == NodeId("eq12"));
}

PF_TEST(DiagnosticMapperClassifiesLatexMessages) {
    // 分类（Build Diagnostics 方案 §13/§16/§19）：code 稳定，
    // warning 始终是 warning（绝不能导致 build 失败，§31），并且
    // latexmk 重跑产生的完全重复项只显示一次。
    SourceMap smap;
    CompileResult cres;
    cres.status = CompileStatus::Failure;
    cres.messages.push_back({"main.tex", 42, true, "Overfull \\hbox (5.2pt too wide)"});
    cres.messages.push_back({"main.tex", 43, false, "LaTeX Warning: Citation `x99' on page 1 undefined"});
    cres.messages.push_back({"main.tex", 44, false, "LaTeX Warning: Reference `sec:a' on page 1 undefined"});
    cres.messages.push_back({"main.tex", 9, true, "! LaTeX Error: Emergency stop."});
    // 同一消息出现两次（重跑会重复输出）：已去重。
    cres.messages.push_back({"main.tex", 9, true, "! LaTeX Error: Emergency stop."});

    DiagnosticMapper mapper;
    const BuildId build("build-77");
    auto diagnostics = mapper.Map(cres, smap, ProjectRevision{5}, build);
    PF_CHECK(diagnostics.size() == 4);
    PF_CHECK(diagnostics[0].code == "LATEX_OVERFULL_HBOX");
    PF_CHECK(diagnostics[0].severity == DiagnosticSeverity::Warning);
    PF_CHECK(diagnostics[1].code == "LATEX_UNDEFINED_CITATION");
    PF_CHECK(diagnostics[1].severity == DiagnosticSeverity::Warning);
    PF_CHECK(diagnostics[2].code == "LATEX_UNDEFINED_REFERENCE");
    PF_CHECK(diagnostics[3].code == "LATEX_EMERGENCY_STOP");
    PF_CHECK(diagnostics[3].severity == DiagnosticSeverity::Error);
    for (const auto& d : diagnostics) {
        PF_CHECK(d.build_id == build);
        PF_CHECK(!d.raw_message.empty());
        // 没有 source-map 条目 -> 没有 node，但生成文件中的位置
        // 仍会保留，因此 Problems 仍能显示 main.tex:LINE（方案 §47）。
        PF_CHECK(d.location.kind == DiagnosticLocationKind::GeneratedFile);
    }
}

PF_TEST(BuildEmitsLifecycleEvents) {
    // 一次成功的 build 会产生完整的事件序列，全部标记为
    // 同一个 build id（Build Diagnostics 方案 §3-§5，§60 "正常 Build"）。
    BuildTestRig rig(true);
    auto snapshot = rig.MakeSnapshot(ProjectRevision{1});
    const BuildId build_id = snapshot.build_id;
    rig.coordinator->RequestBuild(std::move(snapshot), true);
    PF_CHECK(rig.WaitForResult());
    PF_CHECK(!rig.events.empty());

    std::vector<BuildEventType> order;
    for (const auto& e : rig.events) {
        PF_CHECK(e.build_id == build_id);
        PF_CHECK(e.timestamp_ms > 0);
        order.push_back(e.type);
    }
    auto has = [&](BuildEventType t) {
        return std::find(order.begin(), order.end(), t) != order.end();
    };
    PF_CHECK(has(BuildEventType::BuildStarted));
    PF_CHECK(has(BuildEventType::GenerationStarted));
    PF_CHECK(has(BuildEventType::GenerationFinished));
    PF_CHECK(has(BuildEventType::ProcessStarted));
    PF_CHECK(has(BuildEventType::ProcessFinished));
    PF_CHECK(has(BuildEventType::StdOut));  // mock 日志实时流式输出
    PF_CHECK(has(BuildEventType::BuildSucceeded));
    // 第一个事件是开始，最后一个是终止事件（§5 状态流转）。
    PF_CHECK(order.front() == BuildEventType::BuildStarted);
    PF_CHECK(order.back() == BuildEventType::BuildSucceeded);
    // Build Log 底栏所用的 session 记账信息（§3）。
    PF_CHECK(rig.results[0].finished_ms >= rig.results[0].started_ms);
    PF_CHECK(rig.results[0].exit_code == 0);
}

PF_TEST(BuildFailureEmitsFailedEvent) {
    BuildTestRig rig(false);
    auto snapshot = rig.MakeSnapshot(ProjectRevision{1});
    const BuildId build_id = snapshot.build_id;
    rig.coordinator->RequestBuild(std::move(snapshot), true);
    PF_CHECK(rig.WaitForResult());
    bool saw_failed = false;
    bool saw_succeeded = false;
    for (const auto& e : rig.events) {
        if (e.build_id != build_id) continue;
        if (e.type == BuildEventType::BuildFailed) saw_failed = true;
        if (e.type == BuildEventType::BuildSucceeded) saw_succeeded = true;
    }
    PF_CHECK(saw_failed);
    PF_CHECK(!saw_succeeded);  // Failed 之后绝不能出现 Success（§5）
    PF_CHECK(rig.results[0].exit_code == 1);
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
        if (d.code == "E-CITATION-UNKNOWN-KEY") found = true;
    }
    PF_CHECK(found);
    PF_CHECK(result.can_render);  // 语义问题不阻塞渲染
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
    // IEEE 模板要求 authors/affiliations/abstract/keywords。
    Document doc;
    DocumentEditor editor(doc);
    (void)editor.SetTitle(InlineFromText("Title Only"));
    (void)editor.SetAbstract(InlineFromText("Has abstract"));
    // 没有 authors、affiliations 和 keywords。

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
    PF_CHECK(!has("W-REQ-TITLE"));       // title 存在
    PF_CHECK(!has("W-REQ-ABSTRACT"));    // abstract 存在

    // generic-article 要求 authors（默认）但不要求 affiliations。
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

    // 作者带上其机构的编号。
    PF_CHECK(tex.find("\\author{Alice\\textsuperscript{1} \\and "
                      "Bob\\textsuperscript{2}") != std::string::npos);
    // 两个机构都只列出一次并编号，放在单个 \thanks 内。
    PF_CHECK(tex.find("\\thanks{\\textsuperscript{1} University One \\\\ "
                      "\\textsuperscript{2} Institute Two}") !=
             std::string::npos);
    // ... 且绝不按作者重复。
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

    // 可以添加第二个机构，而指向第一个机构的作者
    // 仍保持可解析的链接。
    // front matter 只能通过 editing system 修改，因此
    // 作者的添加方式与 GUI 添加作者的方式一致。
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

    // 缩短列表会移除链接，而不是留下悬空的 id。
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
