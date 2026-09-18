// End-to-end tests: ProjectSession workflow + real tectonic build.
#include "TestMain.hpp"
#include "ScopedTempDir.hpp"

#include "asset/AssetManager.h"
#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "persistence/ProjectPersistence.h"
#include <fstream>
#include <set>
#include <string>

#include "project/ProjectSession.h"

using namespace pf;

namespace {

ProjectSession::Config TestConfig() {
  ProjectSession::Config config;
  config.tectonic_path = PF_TECTONIC_BIN;
  // E-08: unique per process; this file and TestCitationNumbering.cpp used to
  // share "pf-e2e-workspaces" and could delete each other's build directories.
  static pf::test::ScopedTempDir workspace("pf-e2e-workspaces");
  config.workspace_root = workspace.path();
  config.debounce = std::chrono::milliseconds{0};
  return config;
}

// Build/save results are applied on the application thread, so a non-Qt driver
// has to be that thread: pump the session's event queue until `done` holds.
bool PumpUntil(ProjectSession &session, const std::function<bool()> &done,
               int timeout_ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    session.WaitForApplicationEvent(std::chrono::milliseconds{20});
  }
  return done();
}

bool WaitForBuild(ProjectSession &session, const std::function<bool()> &done,
                  int timeout_ms = 180000) {
  return PumpUntil(session, done, timeout_ms);
}

// Enqueue + drain: keeps the tests reading one line while the write itself
// happens on the save worker.
SaveResult SaveAndFlush(ProjectSession &session) {
  session.Save();
  return session.FlushSaves();
}

} // namespace

PF_TEST(SessionNewProjectWorkflow) {
  pf::test::ScopedTempDir dir("pf-e2e-new");

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
  PF_CHECK(
      InlineToPlainText(session2.state().document().front_matter().title) ==
      "Workflow Paper");
  PF_CHECK(session2.persistence_state() == PersistenceState::Clean);
}

PF_TEST(SessionUndoRedoAndRevisionMonotonicity) {
  pf::test::ScopedTempDir dir("pf-e2e-undo");
  ProjectSession session(TestConfig());
  session.NewProject(dir);

  std::uint64_t max_rev = 0;
  auto make_title_cmd = [&](const std::string &t) {
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
  PF_CHECK(InlineToPlainText(session.state().document().front_matter().title) ==
           "A");
  // Undo produced a new revision, never went backwards.
  PF_CHECK(session.current_revision().value > max_rev);
  session.Redo();
  PF_CHECK(InlineToPlainText(session.state().document().front_matter().title) ==
           "B");
}

PF_TEST(SessionBibliographyImportAndSearch) {
  pf::test::ScopedTempDir dir("pf-e2e-bib");
  ProjectSession session(TestConfig());
  session.NewProject(dir);

  std::string bib =
      "@article{k1, author={A. Author}, title={Great Paper}, year={2020}}";
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
}

PF_TEST(SessionTemplateChangeUndoable) {
  pf::test::ScopedTempDir dir("pf-e2e-tpl");
  ProjectSession session(TestConfig());
  session.NewProject(dir);

  std::uint64_t doc_version_before = session.state().document_version();
  std::uint64_t rev_before = session.current_revision().value;

  session.ChangeTemplate("ieee-conference");
  PF_CHECK(session.state().template_selection() == "ieee-conference");
  PF_CHECK(session.state().document_version() == doc_version_before);
  PF_CHECK(session.current_revision().value == rev_before + 1);
}

PF_TEST(SessionAssetImportFigure) {
  pf::test::ScopedTempDir dir("pf-e2e-asset");
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
  pf::test::ScopedTempDir png_dir("pf-test-image");
  auto png_path = png_dir.path() / "image.png";
  {
    static const unsigned char png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // signature
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR len+type
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // 1x1
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00,
        0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01,
        0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    std::ofstream out(png_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png), sizeof(png));
  }

  auto figure_result = session.InsertFigureFromSource(png_path, section_id);
  PF_CHECK(figure_result.status == EditStatus::Applied);
  PF_CHECK(session.assets().registry().All().size() == 1);
  {
    const Document &doc = session.state().document();
    PF_CHECK(doc.body().sections[0].blocks.size() == 1);
    const auto *fig = std::get_if<Figure>(&doc.body().sections[0].blocks[0]);
    PF_CHECK(fig != nullptr);
    PF_CHECK(fig->asset_id != AssetId());
  }
  // Asset file was copied into project assets dir.
  PF_CHECK(!session.assets().registry().All().empty());
}

// Real tectonic build (network may be needed on first run for bundles).
#ifndef PF_SKIP_TECTONIC_TESTS
PF_TEST(EndToEndTectonicBuild) {
  pf::test::ScopedTempDir dir("pf-e2e-tectonic");

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
  para.content =
      InlineFromText("This paper was built by PaperForge end to end.");
  session.Execute(make_cmd(para));

  InsertEquationPayload eq;
  eq.parent = sec_result.created_node;
  eq.latex = "E = mc^{2}";
  session.Execute(make_cmd(eq));

  session.ImportBibliography(
      "@article{ref1, author={Jane Doe}, title={Something}, year={2021}}");
  (void)SaveAndFlush(session);

  bool completed = false;
  std::optional<BuildResult> last_result;
  session.SetBuildResultHandler([&](const BuildResult &r) {
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
    for (const auto &d : last_result->diagnostics) {
      std::cout << "    " << d.Summary() << "\n";
    }
  }
}
#endif

PF_TEST(SessionAutosaveAndCrashRecovery) {
  pf::test::ScopedTempDir dir("pf-e2e-recovery");

  std::optional<BuildResult> unused;
  {
    ProjectSession session(TestConfig());
    session.NewProject(dir);
    PF_CHECK(!session.HasRecoverySnapshot());

    auto make_title_cmd = [&](const std::string &t) {
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
    PF_CHECK(
        InlineToPlainText(session.state().document().front_matter().title) ==
        "Unsaved Title");
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
  }
  // Reopen WITHOUT recovery: shows the last user save.
  {
    ProjectSession session(TestConfig());
    std::string error;
    PF_CHECK(session.OpenProject(dir, &error));
    PF_CHECK(
        InlineToPlainText(session.state().document().front_matter().title) ==
        "Saved Title");
  }
}

PF_TEST(SessionAutosaveTimer) {
  pf::test::ScopedTempDir dir("pf-e2e-autotimer");
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
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{900};
  while (std::chrono::steady_clock::now() < deadline) {
    session.WaitForApplicationEvent(std::chrono::milliseconds{50});
  }
  session.FlushSaves();
  session.StopAutosaveTimer();

  PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" /
                                   "autosave.paper"));
  // Dirty state unchanged by autosave (architecture 32).
  PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
}

// Regression: inserting several figures into a project that was loaded from
// disk used to make the earlier figure show the newest image, and to scatter
// the insert order. Cause: the id generator started at zero in every process,
// so after opening a project whose nodes were "n1..nN" the first inserted
// block reused an id already on disk. Two nodes sharing an id make
// LocateNode/ResolveInsertionPoint (which return the *first* match) point at
// the wrong block. This drives the real user path - open, insert image, insert
// a second image - and checks every inserted figure keeps its own node id,
// its own staged asset file, and its own place in the document.
PF_TEST(SessionInsertingMultipleFiguresKeepsIdsAssetsAndOrder) {
  pf::test::ScopedTempDir dir("pf-e2e-multi-image");

  // Ids far above anything this process allocates, so a collision can only
  // be avoided by the load-time observe/heal path.
  {
    std::ofstream out(dir / "project.paper", std::ios::binary);
    out << R"({
          "schemaVersion": "3",
          "projectId": "p-multi-image",
          "revision": 1,
          "template": "generic-article",
          "body": {
            "sections": [
              {
                "id": "n800000",
                "title": [{"type": "text", "text": "Intro"}],
                "blocks": [
                  {"type": "paragraph", "id": "n800001",
                   "content": [{"type": "text", "text": "body"}]}
                ],
                "subsections": []
              }
            ]
          }
        })";
  }

  // Two distinct 1x1 PNGs, so the staged files can be told apart.
  static const unsigned char png_a[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
      0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
      0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  auto src_a = dir / "first.png";
  auto src_b = dir / "second.png";
  {
    std::ofstream out(src_a, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png_a), sizeof(png_a));
  }
  {
    std::ofstream out(src_b, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png_a), sizeof(png_a));
    out.put('\n'); // makes the two files differ
  }

  ProjectSession session(TestConfig());
  std::string error;
  PF_CHECK(session.OpenProject(dir, &error));

  const NodeId section("n800000");
  auto first = session.InsertFigureFromSource(src_a, section);
  PF_CHECK(first.status == EditStatus::Applied);
  auto second = session.InsertFigureFromSource(src_b, section);
  PF_CHECK(second.status == EditStatus::Applied);
  if (first.status != EditStatus::Applied ||
      second.status != EditStatus::Applied) {
    return;
  }

  // Each insert minted its own id - the old bug made them collide.
  PF_CHECK(first.created_node != second.created_node);
  // And the ids sit *above* everything the file already used: loading pushed
  // the generator past the stored ids, which is exactly what stops a new
  // insert from reusing an id that is already on disk.
  PF_CHECK(std::stoull(first.created_node.value().substr(1)) > 800001);
  PF_CHECK(std::stoull(second.created_node.value().substr(1)) > 800001);

  // Walk the document: both figures present, in insertion order, each with a
  // distinct asset. The first one must still be the first one.
  std::vector<std::string> figure_nodes;
  std::vector<AssetId> figure_assets;
  VisitBlocks(session.state().document(),
              [&](const Block &block, const NodeAddress &) {
                const auto *fig = std::get_if<Figure>(&block);
                if (!fig)
                  return;
                figure_nodes.push_back(fig->id.value());
                figure_assets.push_back(fig->asset_id);
              });
  PF_CHECK_EQ(figure_nodes.size(), std::size_t{2});
  if (figure_nodes.size() != 2) {
    return;
  }
  PF_CHECK_EQ(figure_nodes[0], first.created_node.value());
  PF_CHECK_EQ(figure_nodes[1], second.created_node.value());
  PF_CHECK(figure_assets[0] != figure_assets[1]);

  // Both images were copied into the project-local assets directory and are
  // reachable through a relative path (issue: path must point at the file).
  for (const auto &asset : figure_assets) {
    const auto *meta = session.assets().registry().Find(asset);
    PF_CHECK(meta != nullptr);
    if (!meta)
      continue;
    PF_CHECK(!meta->relative_path.empty());
    PF_CHECK(std::filesystem::exists(dir / "assets" / meta->relative_path));
  }

  // No duplicate node ids anywhere, so later edits cannot hit the wrong block.
  std::set<std::string> unique;
  for (const auto &id : CollectAllNodeIds(session.state().document()))
    unique.insert(id.value());
  PF_CHECK_EQ(unique.size(),
              CollectAllNodeIds(session.state().document()).size());

  // Round-trip: the healed ids and both assets survive a save + reopen.
  PF_CHECK(SaveAndFlush(session).status == SaveResult::Status::Ok);
  ProjectSession reopened(TestConfig());
  PF_CHECK(reopened.OpenProject(dir, &error));
  std::size_t reopened_figures = 0;
  VisitBlocks(reopened.state().document(),
              [&](const Block &block, const NodeAddress &) {
                if (std::get_if<Figure>(&block))
                  ++reopened_figures;
              });
  PF_CHECK_EQ(reopened_figures, std::size_t{2});
}
