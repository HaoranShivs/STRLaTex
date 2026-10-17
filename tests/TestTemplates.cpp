#include "ScopedTempDir.hpp"
#include "TestMain.hpp"
#include "build/DiagnosticMapper.h"
#include "build/TemplateVerifier.h"
#include "core/ContentHash.h"
#include "document/DocumentEditor.h"
#include "document/InlineText.h"
#include "project/ProjectSession.h"
#include "render/LatexPlan.h"
#include <fstream>
#include <sstream>
#include <utility>
#include <zlib.h>

using namespace pf;
namespace {
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void Write(const std::filesystem::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
}
// Stored ZIP fixtures exercise the archive parser without depending on external
// tools.
struct ZipFile {
    std::string name, data;
    unsigned mode = 0100644;
};
std::string Zip(const std::vector<ZipFile>& files) {
    std::string out, central;
    const auto u16 = [](std::string& s, unsigned n) {
        s += char(n);
        s += char(n >> 8);
    };
    const auto u32 = [&](std::string& s, unsigned n) {
        u16(s, n);
        u16(s, n >> 16);
    };
    for (const auto& file : files) {
        unsigned offset = out.size(),
                 crc =
                     crc32(0, reinterpret_cast<const Bytef*>(file.data.data()),
                           file.data.size());
        u32(out, 0x04034b50);
        u16(out, 20);
        u16(out, 0);
        u16(out, 0);
        u32(out, 0);
        u32(out, crc);
        u32(out, file.data.size());
        u32(out, file.data.size());
        u16(out, file.name.size());
        u16(out, 0);
        out += file.name;
        out += file.data;
        u32(central, 0x02014b50);
        u16(central, 0x0314);
        u16(central, 20);
        u16(central, 0);
        u16(central, 0);
        u32(central, 0);
        u32(central, crc);
        u32(central, file.data.size());
        u32(central, file.data.size());
        u16(central, file.name.size());
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u32(central, file.mode << 16);
        u32(central, offset);
        central += file.name;
    }
    unsigned offset = out.size();
    out += central;
    u32(out, 0x06054b50);
    u16(out, 0);
    u16(out, 0);
    u16(out, files.size());
    u16(out, files.size());
    u32(out, central.size());
    u32(out, offset);
    u16(out, 0);
    return out;
}
std::string Descriptor(const std::string& adapter = "article-v1") {
    return JsonValue(JsonObject{
                         {"apiVersion", 1},
                         {"version", "1.0"},
                         {"profiles", JsonArray{JsonObject{
                                          {"id", "test-journal"},
                                          {"name", "Test Journal"},
                                          {"class", "testjournal"},
                                          {"frontmatter", adapter},
                                          {"classOptions", JsonArray{"11pt"}},
                                          {"bibliographyStyle", "unsrtnat"}}}}})
        .Dump();
}
std::vector<ZipFile> FixtureFiles() {
    return {{"bundle/paperforge-template.json", Descriptor()},
            {"bundle/testjournal.cls",
             "\\NeedsTeXFormat{LaTeX2e}\n\\ProvidesClass{testjournal}[2026/10/"
             "17 Test]\n\\LoadClass{article}\n\\endinput\n"},
            {"bundle/README", "Fixture license and instructions"}};
}
TemplatePackage Fixture(const std::filesystem::path& temp) {
    Write(temp / "template.zip", Zip(FixtureFiles()));
    auto inspected = InspectTemplateArchive(temp / "template.zip");
    if (!inspected)
        throw std::runtime_error(inspected.error());
    return std::move(inspected.value());
}
Document Paper() {
    Document doc;
    DocumentEditor editor(doc);
    editor.SetTitle(InlineFromText("Research & Structure_1"));
    auto aff = editor.AddAffiliation("Verification Institute");
    Author a;
    a.name = "Alice Example";
    a.affiliations = {aff.value()};
    a.email = "alice@example.org";
    editor.AddAuthor(a);
    editor.SetAbstract(InlineFromText("An abstract with scientific content."));
    editor.SetKeywords({"structure", "science"});
    auto section = editor.InsertSection(0, InlineFromText("Introduction"));
    Paragraph p;
    p.content = InlineFromText("A paragraph with ");
    p.content.push_back(
        TextRun{"bold & italic", TextMark::Strong | TextMark::Emphasis});
    editor.InsertBlock(section.value(), std::nullopt, p);
    return doc;
}
RenderRequest Request(const Document& doc, const std::string& id) {
    RenderRequest r;
    r.document = &doc;
    r.template_id = id;
    r.resolved_template = std::make_shared<const TemplateDefinition>(
        *TemplateRegistry::Instance().Find(id));
    return r;
}
std::filesystem::path Runtime() {
    return std::filesystem::path(PF_INSTALL_ROOT) / "runtime/texlive";
}
std::string PdfText(const std::filesystem::path& pdf) {
    const auto text = pdf.parent_path() / "extracted.txt";
    if (!std::filesystem::exists("/usr/bin/pdftotext"))
        return {};
    const auto command =
        "/usr/bin/pdftotext '" + pdf.string() + "' '" + text.string() + "'";
    PF_CHECK(std::system(command.c_str()) == 0);
    return Read(text);
}
} // namespace

PF_TEST(TemplatePlanResolvesNativeFrontmatterAndImmutableEmission) {
    auto doc = Paper();
    auto ieee = Request(doc, "ieee-conference");
    auto planned = LatexPlanner().Plan(ieee, *ieee.resolved_template);
    PF_CHECK(planned.plan != nullptr);
    if (!planned.plan)
        return;
    auto before = LatexEmitter().Emit(*planned.plan);
    const auto& tex = before.package.files.front().content;
    PF_CHECK(tex.find("\\IEEEauthorblockN{") != std::string::npos);
    PF_CHECK(tex.find("\\IEEEauthorblockA{") != std::string::npos);
    PF_CHECK(tex.find("\\thanks{") == std::string::npos);
    PF_CHECK(tex.find("\\begin{IEEEkeywords}") != std::string::npos);
    PF_CHECK(tex.find("Research \\& Structure\\_1") != std::string::npos);
    PF_CHECK(tex.find("\\textbf{\\emph{bold \\& italic}}") !=
             std::string::npos);
    DocumentEditor(doc).SetTitle(InlineFromText("Changed after planning"));
    PF_CHECK_EQ(
        tex, LatexEmitter().Emit(*planned.plan).package.files.front().content);
    auto generic = LatexRenderer().Render(Request(doc, "generic-article"));
    PF_CHECK(generic.package.files.front().content.find("\\thanks{") !=
             std::string::npos);
    PF_CHECK(generic.package.files.front().content.find("\\IEEEauthorblock") ==
             std::string::npos);
}
PF_TEST(TemplatePlanIdentityExcludesAttemptsAndIncludesTemplateResources) {
    auto doc = Paper();
    auto request = Request(doc, "generic-article");
    auto a = LatexRenderer().Render(request);
    request.build_id = "another-attempt";
    request.snapshot_id = "another-snapshot";
    request.revision = {500};
    auto b = LatexRenderer().Render(request);
    PF_CHECK_EQ(a.package.plan_digest, b.package.plan_digest);
    auto def = *request.resolved_template;
    def.resources["journal.sty"] = "% changed";
    request.resolved_template = std::make_shared<const TemplateDefinition>(def);
    auto c = LatexRenderer().Render(request);
    PF_CHECK(a.package.plan_digest != c.package.plan_digest);
    def.frontmatter = "unknown-v1";
    request.resolved_template = std::make_shared<const TemplateDefinition>(def);
    PF_CHECK(LatexRenderer().Render(request).status ==
             RenderResult::Status::Failed);
    def.frontmatter = "article-v1";
    def.packages.push_back({"geometry", "margin=5cm"});
    request.resolved_template = std::make_shared<const TemplateDefinition>(def);
    PF_CHECK(LatexRenderer().Render(request).status ==
             RenderResult::Status::Failed);
}
PF_TEST(TemplateSourceMapUsesActualRangesAndDoesNotMapClassErrorsToParagraphs) {
    auto doc = Paper();
    auto rendered = LatexRenderer().Render(Request(doc, "generic-article"));
    auto pos = rendered.package.files.front().content.find("A paragraph");
    const auto& tex = rendered.package.files.front().content;
    auto line = 1 + std::count(tex.begin(), tex.begin() + pos, '\n');
    PF_CHECK(rendered.source_map.Resolve(line).has_value());
    CompileResult compiled;
    compiled.messages.push_back(
        {"broken.cls", static_cast<unsigned>(line), true, "Class failed"});
    auto diagnostics =
        DiagnosticMapper().Map(compiled, rendered.source_map, {});
    PF_CHECK(diagnostics.front().location.kind != DiagnosticLocationKind::Node);
    SourceMap map;
    map.AddMapping(2, NodeId("paragraph"));
    PF_CHECK(!map.Resolve(3));
}
PF_TEST(TemplateArchiveRejectsUnsafeAndAmbiguousEntries) {
    test::ScopedTempDir temp("pf-template-zip");
    const std::vector<std::vector<ZipFile>> cases = {
        {{"../outside.cls", "bad"}},
        {{"/absolute.cls", "bad"}},
        {{"C:/drive.cls", "bad"}},
        {{"a\\b.cls", "bad"}},
        {{"a/./b.cls", "bad"}},
        {{"duplicate.cls", "a"}, {"duplicate.cls", "b"}},
        {{"link.cls", "/etc/passwd", 0120777}},
        {{"a", "one"}, {"a/b.cls", "two"}}};
    for (const auto& files : cases) {
        Write(temp / "unsafe.zip", Zip(files));
        auto inspected = InspectTemplateArchive(temp / "unsafe.zip");
        PF_CHECK(!inspected);
        if (!inspected)
            PF_CHECK(inspected.error().find("NeedsAdapter") ==
                     std::string::npos);
    }
    auto invalid = Zip(FixtureFiles());
    invalid[30] = 'x';
    Write(temp / "mismatch.zip", invalid);
    PF_CHECK(!InspectTemplateArchive(temp / "mismatch.zip"));
    invalid = Zip(FixtureFiles());
    invalid[14] ^= 1;
    Write(temp / "checksum.zip", invalid);
    PF_CHECK(!InspectTemplateArchive(temp / "checksum.zip"));
    Write(temp / "truncated.zip", invalid.substr(0, invalid.size() - 4));
    PF_CHECK(!InspectTemplateArchive(temp / "truncated.zip"));
}
PF_TEST(TemplateManifestRejectsUnsupportedComponentsAndCodeOptions) {
    test::ScopedTempDir temp("pf-template-manifest");
    auto files = FixtureFiles();
    files[0].data = Descriptor("native-script-plugin");
    Write(temp / "unknown.zip", Zip(files));
    PF_CHECK(!InspectTemplateArchive(temp / "unknown.zip"));
    files = FixtureFiles();
    auto json = JsonParse(files[0].data);
    json->set("buildCommand", "execute arbitrary command");
    files[0].data = json->Dump();
    Write(temp / "script.zip", Zip(files));
    PF_CHECK(!InspectTemplateArchive(temp / "script.zip"));
    files = FixtureFiles();
    json = JsonParse(files[0].data);
    auto profiles = json->find("profiles")->as_array();
    profiles.front().set("id", "..");
    json->set("profiles", profiles);
    files[0].data = json->Dump();
    Write(temp / "profile-path.zip", Zip(files));
    PF_CHECK(!InspectTemplateArchive(temp / "profile-path.zip"));
    Write(temp / "resources.zip",
          Zip({{"unknown.cls", "% unrecognized publisher"}}));
    auto unsupported = InspectTemplateArchive(temp / "resources.zip");
    PF_CHECK(!unsupported);
    if (!unsupported)
        PF_CHECK(unsupported.error().starts_with("NeedsAdapter:"));
}
PF_TEST(TemplateManifestBoundsResourceExpansionAcrossProfiles) {
    test::ScopedTempDir temp("pf-template-profile-budget");
    auto files = FixtureFiles();
    auto descriptor = JsonParse(files[0].data);
    auto prototype = descriptor->find("profiles")->as_array().front();
    JsonArray profiles;
    for (int i = 0; i < 16; ++i) {
        auto profile = prototype;
        profile.set("id", "profile-" + std::to_string(i));
        profiles.push_back(profile);
    }
    descriptor->set("profiles", profiles);
    files[0].data = descriptor->Dump();
    files.push_back({"bundle/large.sty", std::string(5 * 1024 * 1024, '%')});
    Write(temp / "expanded.zip", Zip(files));
    auto result = InspectTemplateArchive(temp / "expanded.zip");
    PF_CHECK(!result);
    if (!result)
        PF_CHECK(result.error().find("memory limit") != std::string::npos);
}
PF_TEST(TemplatePackageInstallIsPinnedPortableAndDetectsTampering) {
    test::ScopedTempDir temp("pf-template-storage");
    auto package = Fixture(temp.path());
    auto installed = InstallTemplatePackage(temp.path(), package);
    PF_CHECK(installed.ok());
    PF_CHECK(InstallTemplatePackage(temp.path(), package).ok());
    auto loaded = LoadProjectTemplates(temp.path());
    PF_CHECK(loaded.ok());
    if (!loaded || loaded.value().empty())
        return;
    PF_CHECK_EQ(loaded.value().front().binding,
                package.profiles.front().binding);
    PF_CHECK_EQ(loaded.value().front().resources,
                package.profiles.front().resources);
    PF_CHECK(
        VerifyTemplateBinding(temp.path(), package.profiles.front().binding)
            .ok());
    const auto stored = temp / ("template-resources/" + package.digest +
                                "/files/testjournal.cls");
    Write(stored, "% tampered");
    PF_CHECK(
        !VerifyTemplateBinding(temp.path(), package.profiles.front().binding));
    PF_CHECK(!InstallTemplatePackage(temp.path(), package));
    PF_CHECK_EQ(Read(stored),
                "% tampered"); // No silent overwrite of an immutable identity.
}
PF_TEST(TemplateBindingPersistsAndTemplateUndoRestoresTheActualSelection) {
    test::ScopedTempDir temp("pf-template-session");
    ProjectSession::Config config;
    config.workspace_root = temp / "build";
    config.compiler_factory = [] { return std::make_unique<MockCompiler>(); };
    ProjectSession session(config);
    PF_CHECK(session.NewProject(temp / "project"));
    const auto doc_version = session.state().document_version();
    session.ChangeTemplate("ieee-conference");
    const auto ieee = session.state().template_binding();
    PF_CHECK(session.Undo().status == EditStatus::Applied);
    PF_CHECK_EQ(session.state().template_selection(), "generic-article");
    PF_CHECK(session.Redo().status == EditStatus::Applied);
    PF_CHECK_EQ(session.state().template_binding(), ieee);
    PF_CHECK_EQ(session.state().document_version(), doc_version);
    session.Save();
    PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    PF_CHECK(std::filesystem::exists(temp / "project/template.lock.json"));
    session.CloseProject();
    PF_CHECK(session.OpenProject(temp / "project", nullptr));
    PF_CHECK_EQ(session.state().template_binding(), ieee);
    auto raw = JsonParse(Read(temp / "project/project.paper"));
    auto binding = *raw->find("templateBinding");
    binding.set("digest", "broken");
    raw->set("templateBinding", binding);
    PF_CHECK(!ProjectSerializer::Deserialize(raw->Dump()));
}
PF_TEST(TemplateBuildCapturesOutputDirectoryBeforeEnteringTheWorker) {
    test::ScopedTempDir temp("pf-template-build-directory");
    const auto owner = std::this_thread::get_id();
    std::atomic<bool> foreign_callback{false}, done{false}, success{false};
    BuildCoordinator::Host host;
    host.workspace_root = [&] { return (temp / "workspaces").string(); };
    host.debug_dump_dir = [&]() -> std::optional<std::string> {
        if (std::this_thread::get_id() != owner) {
            foreign_callback.store(true);
            return (temp / "wrong-project").string();
        }
        return (temp / "captured-project").string();
    };
    host.on_build_finished = [&](const BuildResult& result) {
        success.store(result.outcome == BuildResult::Outcome::Success);
        done.store(true);
    };
    MockCompiler compiler;
    BuildCoordinator coordinator(std::move(host), &compiler);
    BuildSnapshot snapshot;
    snapshot.project_id = ProjectId("template-fixture");
    snapshot.snapshot_id = "snapshot";
    snapshot.template_id = "generic-article";
    snapshot.document = std::make_shared<const Document>(Paper());
    coordinator.RequestBuild(std::move(snapshot), true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    PF_CHECK(done.load());
    PF_CHECK(success.load());
    PF_CHECK(!foreign_callback.load());
    PF_CHECK(std::filesystem::exists(temp / "captured-project/main.tex"));
    PF_CHECK(!std::filesystem::exists(temp / "wrong-project"));
}
PF_TEST(TemplateTamperingBlocksTheNextBuildWithoutFallingBackToArticle) {
    test::ScopedTempDir temp("pf-template-block-tampered-build");
    auto package = Fixture(temp.path());
    std::filesystem::create_directories(temp / "project");
    PF_CHECK(InstallTemplatePackage(temp / "project", package).ok());
    SerializedProject project;
    project.project_id = "template-project";
    project.template_id = package.profiles.front().id;
    project.template_binding = package.profiles.front().binding;
    project.document = Paper();
    SaveRequest save;
    save.snapshot = project;
    save.destination = temp / "project/project.paper";
    save.save_id = "initial";
    PF_CHECK(ProjectPersistence::Save(save).status == SaveResult::Status::Ok);
    ProjectSession::Config config;
    config.workspace_root = temp / "build";
    config.compiler_factory = [] { return std::make_unique<MockCompiler>(); };
    ProjectSession session(config);
    PF_CHECK(session.OpenProject(temp / "project", nullptr));
    Write(temp / ("project/template-resources/" + package.digest +
                  "/files/testjournal.cls"),
          "% altered after open");
    std::optional<BuildResult> result;
    session.SetBuildResultHandler(
        [&](const BuildResult& value) { result = value; });
    session.RequestBuild(true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!result && std::chrono::steady_clock::now() < deadline)
        session.WaitForApplicationEvent(std::chrono::milliseconds(20));
    PF_CHECK(result.has_value());
    if (result)
        PF_CHECK(result->outcome == BuildResult::Outcome::Failure);
    PF_CHECK_EQ(session.state().template_selection(),
                package.profiles.front().id);
}
PF_TEST(TemplateBuildsUseGenuineBuiltinClassesAndPrintAffiliations) {
    if (!std::filesystem::exists(Runtime() / "bin")) {
        std::cout << "  SKIP: TeX runtime unavailable\n";
        return;
    }
    test::ScopedTempDir temp("pf-template-native");
    auto doc = Paper();
    auto paragraph = std::get<Paragraph>(
        std::as_const(doc).body().sections.front().blocks.front());
    paragraph.content.push_back(TextRun{" According to ", 0});
    Citation narrative;
    narrative.keys = {"native"};
    narrative.mode = CitationMode::Narrative;
    paragraph.content.push_back(narrative);
    DocumentEditor(doc).SetParagraphContent(paragraph.id, paragraph.content);
    for (const auto& id : {"generic-article", "ieee-conference"}) {
        auto render_request = Request(doc, id);
        render_request.bibliography_bibtex =
            "@article{native,author={Example, Alice},title={Native "
            "bibliography},journal={Verification Journal},year={2026}}";
        auto rendered = LatexRenderer().Render(render_request);
        CompileRequest request;
        request.package = std::move(rendered.package);
        request.toolchain = request.package.toolchain;
        request.workspace = temp / id;
        CompilerConfig config;
        config.texlive_root = Runtime();
        auto compiled = TexLiveCompiler(config).Compile(request);
        PF_CHECK(compiled.status == CompileStatus::Success);
        if (compiled.status != CompileStatus::Success) {
            std::cout << compiled.log;
            continue;
        }
        auto text = PdfText(compiled.pdf_path);
        if (!text.empty()) {
            PF_CHECK(text.find("Verification Institute") != std::string::npos);
            PF_CHECK(text.find("structure") != std::string::npos);
            PF_CHECK(text.find("Example [1]") != std::string::npos);
        }
        for (const auto& message : compiled.messages)
            PF_CHECK(!message.is_error);
        if (std::string(id) == "ieee-conference") {
            PF_CHECK(Read(request.workspace / "main.fls")
                         .find("ieeetran/IEEEtran.cls") != std::string::npos);
            PF_CHECK(compiled.log.find("thanks is locked out") ==
                     std::string::npos);
        }
    }
}
PF_TEST(
    TemplateCompilerReportsFinalPassDiagnosticsAndIgnoresStatusExclamations) {
    if (!std::filesystem::exists(Runtime() / "bin"))
        return;
    test::ScopedTempDir temp("pf-template-diagnostics");
    auto doc = Paper();
    auto paragraph = std::get<Paragraph>(
        std::as_const(doc).body().sections.front().blocks.front());
    Citation cite;
    cite.keys = {"resolved"};
    paragraph.content.push_back(cite);
    DocumentEditor(doc).SetParagraphContent(paragraph.id, paragraph.content);
    auto request = Request(doc, "generic-article");
    auto def = *request.resolved_template;
    def.packages.push_back({"hookfixture", ""});
    def.resources["hookfixture.sty"] =
        "\\ProvidesPackage{hookfixture}\n\\typeout{new hooks "
        "used!!}\n\\endinput\n";
    request.resolved_template = std::make_shared<const TemplateDefinition>(def);
    request.bibliography_bibtex =
        "@article{resolved,author={Example, Alice},title={Complete "
        "reference},journal={Test},year={2026}}";
    auto rendered = LatexRenderer().Render(request);
    CompileRequest compile;
    compile.package = std::move(rendered.package);
    compile.toolchain = compile.package.toolchain;
    compile.workspace = temp / "build";
    CompilerConfig config;
    config.texlive_root = Runtime();
    auto result = TexLiveCompiler(config).Compile(compile);
    PF_CHECK(result.status == CompileStatus::Success);
    PF_CHECK(result.log.find("new hooks used!!") != std::string::npos);
    PF_CHECK(result.log.find("undefined") !=
             std::string::npos); // An early pass really needed resolving.
    for (const auto& message : result.messages) {
        PF_CHECK(!message.is_error);
        PF_CHECK(message.text.find("new hooks used!!") == std::string::npos);
        PF_CHECK(message.text.find("undefined") == std::string::npos);
    }
}
PF_TEST(TemplateIsolationHidesHostFilesAndDisablesShellEscape) {
    if (!std::filesystem::exists(Runtime() / "bin"))
        return;
    test::ScopedTempDir temp("pf-template-isolation");
    Write(temp / "private.txt", "Host-only fixture data");
    CompileRequest request;
    request.workspace = temp / "build";
    request.package.requires_isolation = true;
    request.package.files.push_back(
        {"main.tex",
         "\\documentclass{article}\n\\begin{document}\n"
         "\\IfFileExists{" +
             (temp / "private.txt").string() +
             "}{\\errmessage{Host file leaked}}{\\typeout{HOST-FILE-HIDDEN}}\n"
             "\\ifnum\\pdfshellescape=0 "
             "\\typeout{SHELL-ESCAPE-DISABLED}\\else\\errmessage{Shell escape "
             "enabled}\\fi\n"
             "Isolated template fixture.\\end{document}\n",
         false});
    CompilerConfig config;
    config.texlive_root = Runtime();
    auto result = TexLiveCompiler(config).Compile(request);
    PF_CHECK(result.status == CompileStatus::Success);
    if (result.status != CompileStatus::Success)
        std::cout << result.log;
    PF_CHECK(result.log.find("HOST-FILE-HIDDEN") != std::string::npos);
    PF_CHECK(result.log.find("SHELL-ESCAPE-DISABLED") != std::string::npos);
    PF_CHECK_EQ(Read(temp / "private.txt"), "Host-only fixture data");
}
PF_TEST(TemplateCasPublisherZipCompilesBothProfilesAndSurvivesReopen) {
    const auto archive = std::filesystem::path(PF_INSTALL_ROOT) /
                         "tools/templates/els-cas-templates.zip";
    if (!std::filesystem::exists(archive)) {
        std::cout << "  SKIP: optional CAS fixture ZIP unavailable\n";
        return;
    }
    test::ScopedTempDir temp("pf-template-cas");
    auto verified = VerifyTemplateArchive(archive, Runtime(), temp / "probe");
    PF_CHECK(verified.ok());
    if (!verified) {
        std::cout << verified.error() << '\n';
        return;
    }
    PF_CHECK_EQ(verified.value().package.profiles.size(), std::size_t{2});
    for (const auto& probe : verified.value().probes) {
        auto text = PdfText(probe.pdf_path);
        if (!text.empty()) {
            PF_CHECK(text.find("PaperForge Verification Institute") !=
                     std::string::npos);
            PF_CHECK(text.find("Alice Example") != std::string::npos);
            PF_CHECK(text.find("Keywords") != std::string::npos);
        }
        auto tex = Read(probe.pdf_path.parent_path() / "main.tex");
        PF_CHECK(tex.find("\\begin{abstract}") < tex.find("\\maketitle"));
        PF_CHECK(tex.find("\\affiliation[1]{organization={") !=
                 std::string::npos);
        PF_CHECK(tex.find("\\title[mode=title]{") != std::string::npos);
        PF_CHECK(tex.find("\\usepackage[margin=1in]{geometry}") ==
                 std::string::npos);
    }
    ProjectSession::Config config;
    config.workspace_root = temp / "build";
    config.compiler_factory = [] { return std::make_unique<MockCompiler>(); };
    ProjectSession session(config);
    PF_CHECK(session.NewProject(temp / "project"));
    auto id = verified.value().package.profiles.back().id;
    PF_CHECK(session.InstallVerifiedTemplate(verified.value(), id).ok());
    auto binding = session.state().template_binding();
    PF_CHECK_EQ(session.AvailableTemplates().size(), std::size_t{4});
    session.Save();
    PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    session.CloseProject();
    PF_CHECK(session.OpenProject(temp / "project", nullptr));
    PF_CHECK_EQ(session.state().template_binding(), binding);
    PF_CHECK(session.ResolveTemplate(id) != nullptr);
    PF_CHECK(session.Undo().status !=
             EditStatus::Applied); // History is session-scoped.
}
