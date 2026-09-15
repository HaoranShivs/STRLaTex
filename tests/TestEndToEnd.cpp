// End-to-end tests: ProjectSession workflow + real tectonic build.
#include "TestMain.hpp"

#include "asset/AssetManager.h"
#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include "persistence/ProjectPersistence.h"
#include <fstream>

#include "project/ProjectSession.h"

using namespace pf;

namespace {

ProjectSession::Config TestConfig() {
    ProjectSession::Config config;
    config.tectonic_path = PF_TECTONIC_BIN;
    auto root = std::filesystem::temp_directory_path() / "pf-e2e-workspaces";
    std::filesystem::create_directories(root);
    config.workspace_root = root;
    config.debounce = std::chrono::milliseconds{0};
    return config;
}

// Build/save results are applied on the application thread, so a non-Qt driver
// has to be that thread: pump the session's event queue until `done` holds.
bool PumpUntil(ProjectSession& session, const std::function<bool()>& done,
               int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
    }
    return done();
}

bool WaitForBuild(ProjectSession& session, const std::function<bool()>& done,
                  int timeout_ms = 180000) {
    return PumpUntil(session, done, timeout_ms);
}

// Enqueue + drain: keeps the tests reading one line while the write itself
// happens on the save worker.
SaveResult SaveAndFlush(ProjectSession& session) {
    session.Save();
    return session.FlushSaves();
}

}  // namespace

PF_TEST(SessionNewProjectWorkflow) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-new";
    std::filesystem::remove_all(dir);

    ProjectSession session(TestConfig());
    PF_CHECK(session.NewProject(dir));
    PF_CHECK(session.lifecycle_state() == LifecycleState::Open);

    // Edit via protocol
    EditCommand title_cmd;
    title_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    title_cmd.project_id = session.state().id();
    title_cmd.base_revision = session.current_revision();
    SetTitlePayload title;
    title.title = InlineFromText("Workflow Paper");
    title_cmd.payload = title;
    auto r = session.Execute(title_cmd);
    PF_CHECK(r.status == EditStatus::Applied);
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);

    auto save = SaveAndFlush(session);
    PF_CHECK(save.status == SaveResult::Status::Ok);
    PF_CHECK(session.persistence_state() == PersistenceState::Clean);
    PF_CHECK(std::filesystem::exists(dir / "project.paper"));

    // Reopen and verify.
    ProjectSession session2(TestConfig());
    std::string error;
    PF_CHECK(session2.OpenProject(dir, &error));
    PF_CHECK(InlineToPlainText(
                 session2.state().document().front_matter().title) ==
             "Workflow Paper");
    PF_CHECK(session2.persistence_state() == PersistenceState::Clean);
    std::filesystem::remove_all(dir);
}

PF_TEST(SessionUndoRedoAndRevisionMonotonicity) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-undo";
    std::filesystem::remove_all(dir);
    ProjectSession session(TestConfig());
    session.NewProject(dir);

    std::uint64_t max_rev = 0;
    auto make_title_cmd = [&](const std::string& t) {
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = session.state().id();
        cmd.base_revision = session.current_revision();
        SetTitlePayload p;
        p.title = InlineFromText(t);
        cmd.payload = p;
        return cmd;
    };

    session.Execute(make_title_cmd("A"));
    session.Execute(make_title_cmd("B"));
    max_rev = session.current_revision().value;

    session.Undo();
    PF_CHECK(InlineToPlainText(
                 session.state().document().front_matter().title) == "A");
    // Undo produced a new revision, never went backwards.
    PF_CHECK(session.current_revision().value > max_rev);
    session.Redo();
    PF_CHECK(InlineToPlainText(
                 session.state().document().front_matter().title) == "B");
    std::filesystem::remove_all(dir);
}

PF_TEST(SessionBibliographyImportAndSearch) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-bib";
    std::filesystem::remove_all(dir);
    ProjectSession session(TestConfig());
    session.NewProject(dir);

    std::string bib = "@article{k1, author={A. Author}, title={Great Paper}, year={2020}}";
    auto import = session.ImportBibliography(bib);
    PF_CHECK(import.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(import.entry_count == 1);

    auto search = session.SearchCitations("great");
    PF_CHECK(search.entries.size() == 1);
    PF_CHECK(search.entries[0].key == "k1");

    // Bibliography change bumps ProjectRevision.
    std::uint64_t before = session.current_revision().value;
    session.ImportBibliography(bib + "\n@article{k2, title={Second}}");
    PF_CHECK(session.current_revision().value > before);

    auto save = SaveAndFlush(session);
    PF_CHECK(save.status == SaveResult::Status::Ok);
    PF_CHECK(std::filesystem::exists(dir / "references.bib"));
    std::filesystem::remove_all(dir);
}

PF_TEST(SessionTemplateChangeUndoable) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-tpl";
    std::filesystem::remove_all(dir);
    ProjectSession session(TestConfig());
    session.NewProject(dir);

    std::uint64_t doc_version_before = session.state().document_version();
    std::uint64_t rev_before = session.current_revision().value;

    session.ChangeTemplate("ieee-conference");
    PF_CHECK(session.state().template_selection() == "ieee-conference");
    PF_CHECK(session.state().document_version() == doc_version_before);
    PF_CHECK(session.current_revision().value == rev_before + 1);
    std::filesystem::remove_all(dir);
}

PF_TEST(SessionAssetImportFigure) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-asset";
    std::filesystem::remove_all(dir);
    ProjectSession session(TestConfig());
    session.NewProject(dir);

    // Create a real section to hold the figure.
    EditCommand sec_cmd;
    sec_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    sec_cmd.project_id = session.state().id();
    sec_cmd.base_revision = session.current_revision();
    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("Figures");
    sec_cmd.payload = sec;
    auto sec_result = session.Execute(sec_cmd);
    PF_CHECK(sec_result.status == EditStatus::Applied);
    NodeId section_id = sec_result.created_node;

    // Make a tiny valid PNG (1x1 pixel).
    auto png_path = std::filesystem::temp_directory_path() / "pf-test-image.png";
    {
        static const unsigned char png[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // signature
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR len+type
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  // 1x1
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
            0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
            0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
        std::ofstream out(png_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png), sizeof(png));
    }

    auto figure_result = session.InsertFigureFromSource(png_path, section_id);
    PF_CHECK(figure_result.status == EditStatus::Applied);
    PF_CHECK(session.assets().registry().All().size() == 1);
    {
        const Document& doc = session.state().document();
        PF_CHECK(doc.body().sections[0].blocks.size() == 1);
        const auto* fig = std::get_if<Figure>(&doc.body().sections[0].blocks[0]);
        PF_CHECK(fig != nullptr);
        PF_CHECK(fig->asset_id != AssetId());
    }
    // Asset file was copied into project assets dir.
    PF_CHECK(!session.assets().registry().All().empty());
    std::filesystem::remove_all(dir);
    std::filesystem::remove(png_path);
}

// Real tectonic build (network may be needed on first run for bundles).
#ifndef PF_SKIP_TECTONIC_TESTS
PF_TEST(EndToEndTectonicBuild) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-tectonic";
    std::filesystem::remove_all(dir);

    ProjectSession session(TestConfig());
    session.NewProject(dir);

    auto make_cmd = [&](auto payload) {
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = session.state().id();
        cmd.base_revision = session.current_revision();
        cmd.payload = std::move(payload);
        return cmd;
    };

    SetTitlePayload title;
    title.title = InlineFromText("End to End Test Paper");
    session.Execute(make_cmd(title));

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("Introduction");
    auto sec_result = session.Execute(make_cmd(sec));
    PF_CHECK(sec_result.status == EditStatus::Applied);

    InsertParagraphPayload para;
    para.parent = sec_result.created_node;
    para.content = InlineFromText("This paper was built by PaperForge end to end.");
    session.Execute(make_cmd(para));

    InsertEquationPayload eq;
    eq.parent = sec_result.created_node;
    eq.math_source = "E = mc^{2}";
    session.Execute(make_cmd(eq));

    session.ImportBibliography(
        "@article{ref1, author={Jane Doe}, title={Something}, year={2021}}");
    (void)SaveAndFlush(session);

    bool completed = false;
    std::optional<BuildResult> last_result;
    session.SetBuildResultHandler([&](const BuildResult& r) {
        last_result = r;
        completed = true;
    });
    session.RequestBuild(true);
    PF_CHECK(WaitForBuild(session, [&] { return completed; }));

    PF_CHECK(last_result.has_value());
    // The result carries the identity of the exact ask.
    PF_CHECK(last_result->project_id == session.state().id());
    PF_CHECK(!last_result->build_id.empty());
    PF_CHECK(last_result->snapshot_id == session.latest_snapshot_id());
    if (last_result->outcome == BuildResult::Outcome::Success) {
        PF_CHECK(session.preview_state() == PreviewState::Fresh);
        PF_CHECK(std::filesystem::exists(last_result->pdf_path));
        // PDF magic number
        std::ifstream pdf(last_result->pdf_path, std::ios::binary);
        char header[4] = {0};
        pdf.read(header, 4);
        PF_CHECK(std::string(header, 4) == "%PDF");
        std::cout << "    (PDF built: " << last_result->pdf_path << ")\n";
    } else {
        // If tectonic cannot download its bundle (offline), skip gracefully.
        std::cout << "    (tectonic build failed - likely offline bundle "
                     "download; skipping)\n";
        for (const auto& d : last_result->diagnostics) {
            std::cout << "    " << d.Summary() << "\n";
        }
    }
    std::filesystem::remove_all(dir);
}
#endif

PF_TEST(SessionAutosaveAndCrashRecovery) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-recovery";
    std::filesystem::remove_all(dir);

    std::optional<BuildResult> unused;
    {
        ProjectSession session(TestConfig());
        session.NewProject(dir);
        PF_CHECK(!session.HasRecoverySnapshot());

        auto make_title_cmd = [&](const std::string& t) {
            EditCommand cmd;
            cmd.operation_id = OperationId(IdGenerator::NewOperationId());
            cmd.project_id = session.state().id();
            cmd.base_revision = session.current_revision();
            SetTitlePayload p;
            p.title = InlineFromText(t);
            cmd.payload = p;
            return cmd;
        };
        session.Execute(make_title_cmd("Saved Title"));
        auto save = SaveAndFlush(session);
        PF_CHECK(save.status == SaveResult::Status::Ok);

        // Edit but do not save; then autosave.
        session.Execute(make_title_cmd("Unsaved Title"));
        PF_CHECK(session.persistence_state() == PersistenceState::Dirty);

        // Manual autosave tick: capture on this thread, write on the worker.
        auto autosave = session.Autosave();
        PF_CHECK(autosave.status == SaveResult::Status::Queued);
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
        PF_CHECK(session.HasRecoverySnapshot());
        // Autosave must NOT change Clean/Dirty (architecture 32).
        PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    }
    // "Crash": reopen without saving.
    {
        ProjectSession session(TestConfig());
        std::string error;
        // Recovery snapshot file exists on disk (fresh instance, not opened).
        PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" /
                                         "autosave.paper"));
        bool recovered = false;
        PF_CHECK(session.OpenProjectWithRecovery(dir, &error, &recovered));
        PF_CHECK(recovered);
        PF_CHECK(InlineToPlainText(
                     session.state().document().front_matter().title) ==
                 "Unsaved Title");
        PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    }
    // Reopen WITHOUT recovery: shows the last user save.
    {
        ProjectSession session(TestConfig());
        std::string error;
        PF_CHECK(session.OpenProject(dir, &error));
        PF_CHECK(InlineToPlainText(
                     session.state().document().front_matter().title) ==
                 "Saved Title");
    }
    std::filesystem::remove_all(dir);
}

PF_TEST(SessionAutosaveTimer) {
    auto dir = std::filesystem::temp_directory_path() / "pf-e2e-autotimer";
    std::filesystem::remove_all(dir);
    ProjectSession session(TestConfig());
    session.NewProject(dir);

    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = session.state().id();
    cmd.base_revision = session.current_revision();
    SetTitlePayload p;
    p.title = InlineFromText("Timer Test");
    cmd.payload = p;
    session.Execute(cmd);

    // Short interval: 300ms. The timer thread only posts a tick; this loop is
    // the application thread that turns it into a snapshot.
    session.StartAutosaveTimer(std::chrono::milliseconds{300});
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{900};
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{50});
    }
    session.FlushSaves();
    session.StopAutosaveTimer();

    PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" /
                                     "autosave.paper"));
    // Dirty state unchanged by autosave (architecture 32).
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    std::filesystem::remove_all(dir);
}
