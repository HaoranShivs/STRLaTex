#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <fstream>
#include <iterator>

#include "persistence/ProjectPdf.h"
#include "project/ProjectSession.h"

using namespace pf;

namespace {

ProjectSession::Config PreviewConfig() {
    ProjectSession::Config config;
    config.compiler_factory = [] { return std::make_unique<MockCompiler>(); };
    return config;
}

void WritePdf(const std::filesystem::path& path, const std::string& text = "saved preview") {
    std::ofstream out(path, std::ios::binary);
    out << "%PDF-1.4\n" << text << "\n";
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

BuildResult SuccessfulBuild(const ProjectSession& session, const std::filesystem::path& pdf) {
    BuildResult result;
    result.outcome = BuildResult::Outcome::Success;
    result.project_id = session.state().id();
    result.revision = session.current_revision();
    result.build_id = BuildId("preview-build");
    result.pdf_path = pdf;
    return result;
}

} // namespace

PF_TEST(ProjectPdfSurvivesRestartAndWorkspaceRemoval) {
    test::ScopedTempDir project("pf-preview-reopen");
    test::ScopedTempDir workspace("pf-preview-source");
    const auto source = workspace / "main.pdf";
    WritePdf(source);
    const auto expected = ReadFile(source);
    {
        ProjectSession session(PreviewConfig());
        PF_CHECK(session.NewProject(project));
        session.Save();
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
        PF_CHECK(session.AcceptBuildResult(SuccessfulBuild(session, source)));
        PF_CHECK(session.preview_pdf().path == project / "project.pdf");
        PF_CHECK(ReadFile(project / "project.pdf") == expected);
        PF_CHECK(session.preview_state() == PreviewState::Fresh);
    }
    std::filesystem::remove(source);
    ProjectSession reopened(PreviewConfig());
    std::string error;
    PF_CHECK(reopened.OpenProject(project, &error));
    PF_CHECK(reopened.preview_pdf().valid());
    PF_CHECK(ReadFile(reopened.preview_pdf().path) == expected);
    PF_CHECK(reopened.preview_state() == PreviewState::Stale);
    PF_CHECK(reopened.preview_pdf().build_id.empty());
    PF_CHECK(reopened.latest_build_id().empty());
    reopened.CloseProject();
    PF_CHECK(!reopened.preview_pdf().valid());
}

PF_TEST(ProjectPdfChangesOnlyForAnAcceptedSuccessfulBuild) {
    test::ScopedTempDir project("pf-preview-gate");
    test::ScopedTempDir workspace("pf-preview-gate-source");
    const auto source = workspace / "main.pdf";
    WritePdf(source, "first build");
    ProjectSession session(PreviewConfig());
    PF_CHECK(session.NewProject(project));
    auto result = SuccessfulBuild(session, source);
    PF_CHECK(session.AcceptBuildResult(result));
    const auto first = ReadFile(project / "project.pdf");
    WritePdf(source, "second build");
    for (auto outcome : {BuildResult::Outcome::Failure, BuildResult::Outcome::Cancelled}) {
        result.outcome = outcome;
        PF_CHECK(session.AcceptBuildResult(result));
        PF_CHECK(ReadFile(project / "project.pdf") == first);
    }
    result = SuccessfulBuild(session, source);
    result.revision.value += 1;
    PF_CHECK(!session.AcceptBuildResult(result));
    PF_CHECK(ReadFile(project / "project.pdf") == first);
    result = SuccessfulBuild(session, source);
    result.project_id = ProjectId("another-project");
    PF_CHECK(!session.AcceptBuildResult(result));
    PF_CHECK(ReadFile(project / "project.pdf") == first);
    PF_CHECK(session.AcceptBuildResult(SuccessfulBuild(session, source)));
    PF_CHECK(ReadFile(project / "project.pdf") == ReadFile(source));
    for (const auto& entry : std::filesystem::directory_iterator(project.path()))
        PF_CHECK(!entry.path().filename().string().starts_with("project.pdf.tmp-"));
}

PF_TEST(FailedProjectPdfCopyKeepsTheLastSuccessfulPdf) {
    test::ScopedTempDir project("pf-preview-copy-failure");
    WritePdf(project / "project.pdf");
    const auto expected = ReadFile(project / "project.pdf");
    PF_CHECK(!ProjectPdf::Save(project, project / "missing.pdf").ok());
    PF_CHECK(ReadFile(project / "project.pdf") == expected);
    std::ofstream(project / "invalid.pdf") << "not a PDF";
    PF_CHECK(!ProjectPdf::Save(project, project / "invalid.pdf").ok());
    PF_CHECK(ReadFile(project / "project.pdf") == expected);
}

PF_TEST(ProjectPdfWriteFailureStillPreviewsTheBuildAndReportsAWarning) {
    test::ScopedTempDir project("pf-preview-warning");
    test::ScopedTempDir workspace("pf-preview-warning-source");
    const auto source = workspace / "main.pdf";
    WritePdf(source);
    ProjectSession session(PreviewConfig());
    PF_CHECK(session.NewProject(project));
    std::filesystem::create_directory(project / "project.pdf");
    std::optional<BuildResult> reported;
    session.SetBuildResultHandler([&](const BuildResult& result) { reported = result; });
    PF_CHECK(session.AcceptBuildResult(SuccessfulBuild(session, source)));
    PF_CHECK(session.preview_pdf().path == source);
    PF_CHECK(reported.has_value());
    if (reported) {
        PF_CHECK(reported->outcome == BuildResult::Outcome::Success);
        PF_CHECK(!reported->diagnostics.empty());
        if (!reported->diagnostics.empty()) {
            PF_CHECK(reported->diagnostics.back().code == "PDF_SAVE_FAILED");
            PF_CHECK(reported->diagnostics.back().severity == DiagnosticSeverity::Warning);
        }
    }
}

PF_TEST(ProjectPdfLoadsForAnAutosaveOnlyProject) {
    test::ScopedTempDir project("pf-preview-autosave");
    ProjectSession session(PreviewConfig());
    PF_CHECK(session.NewProject(project));
    session.Autosave();
    session.FlushSaves();
    WritePdf(project / "project.pdf");
    session.CloseProject();
    bool recovered = false;
    std::string error;
    PF_CHECK(session.OpenProjectWithRecovery(project, &error, &recovered));
    PF_CHECK(recovered);
    PF_CHECK(!std::filesystem::exists(project / "project.paper"));
    PF_CHECK(session.preview_pdf().valid());
    PF_CHECK(session.preview_state() == PreviewState::Stale);
}

PF_TEST(ProjectPdfSupportsLegacyPreviewsAndPrefersTheRootPdf) {
    test::ScopedTempDir project("pf-preview-legacy");
    const auto legacy = project / ".paperforge/build/main.pdf";
    std::filesystem::create_directories(legacy.parent_path());
    WritePdf(legacy, "legacy build");
    PF_CHECK(ProjectPdf::Find(project) == std::optional<std::filesystem::path>(legacy));
    WritePdf(project / "project.pdf", "accepted build");
    PF_CHECK(ProjectPdf::Find(project) == std::optional<std::filesystem::path>(project / "project.pdf"));
}

PF_TEST(ProjectPdfRejectsBrokenFilesAndPathsOutsideTheProject) {
    test::ScopedTempDir project("pf-preview-boundary");
    test::ScopedTempDir outside("pf-preview-outside");
    std::ofstream(project / "project.pdf") << "not a PDF";
    PF_CHECK(!ProjectPdf::Find(project));
    std::filesystem::remove(project / "project.pdf");
    WritePdf(outside / "outside.pdf");
    const auto expected = ReadFile(outside / "outside.pdf");
    std::error_code ec;
    std::filesystem::create_symlink(outside / "outside.pdf", project / "project.pdf", ec);
    if (!ec) {
        PF_CHECK(!ProjectPdf::Find(project));
        PF_CHECK(!ProjectPdf::Save(project, outside / "outside.pdf").ok());
        PF_CHECK(ReadFile(outside / "outside.pdf") == expected);
    }
}
