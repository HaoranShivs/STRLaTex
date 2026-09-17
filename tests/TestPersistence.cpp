// Persistence tests: serialize/load round-trip, atomic save, stale-save guard.
#include "TestMain.hpp"

#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include <cstdint>
#include <fstream>
#include <set>
#include <string>

#include "document/DocumentEditor.h"
#include "persistence/ProjectPersistence.h"

using namespace pf;

namespace {

SerializedProject MakeProject() {
  SerializedProject sp;
  sp.project_id = "p-test";
  sp.revision = ProjectRevision{42};
  sp.template_id = "generic-article";

  DocumentEditor editor(sp.document);
  editor.SetTitle(InlineFromText("Persisted Paper"));
  auto s = editor.InsertSection(0, InlineFromText("Section A"));
  auto sub = editor.InsertSubsection(0, 0, InlineFromText("Sub A1"));

  Paragraph p;
  p.content = InlineFromText("content paragraph");
  editor.InsertBlock(s.value(), std::nullopt, p);

  EquationBlock eq;
  eq.expression.latex = "y = f(x)";
  editor.InsertBlock(sub.value(), std::nullopt, eq);

  auto cols = std::vector<TableColumn>{{ColumnAlignment::Left},
                                       {ColumnAlignment::Center}};
  Table table = DocumentEditor::MakeTable(cols, 2, true).value();
  editor.InsertBlock(s.value(), std::nullopt, table);

  AssetMetadata meta;
  meta.id = AssetId("a1");
  meta.relative_path = "figure_001.png";
  meta.media_type = "image/png";
  meta.original_name = "figure_001.png";
  meta.file_size = 1234;
  meta.content_hash = "deadbeef";
  meta.width = 800;
  meta.height = 600;
  sp.assets.push_back(meta);
  return sp;
}

} // namespace

PF_TEST(SerializeDeserializeRoundTrip) {
  auto sp = MakeProject();
  std::string json = ProjectSerializer::Serialize(sp);
  PF_CHECK(!json.empty());

  auto parsed = ProjectSerializer::Deserialize(json);
  PF_CHECK(parsed.ok());
  const auto &back = parsed.value();
  PF_CHECK(back.project_id == "p-test");
  PF_CHECK(back.revision.value == 42);
  PF_CHECK(back.template_id == "generic-article");
  PF_CHECK(InlineToPlainText(back.document.front_matter().title) ==
           "Persisted Paper");
  PF_CHECK(back.document.body().sections.size() == 1);
  PF_CHECK(back.document.body().sections[0].blocks.size() == 2);
  PF_CHECK(back.document.body().sections[0].subsections.size() == 1);
  PF_CHECK(back.document.body().sections[0].subsections[0].blocks.size() == 1);
  PF_CHECK(back.assets.size() == 1);
  PF_CHECK(back.assets[0].content_hash == "deadbeef");

  // Table integrity
  const Document &cdoc = back.document;
  const auto &blocks = cdoc.body().sections[0].blocks;
  const auto *table = std::get_if<Table>(&blocks[1]);
  PF_CHECK(table != nullptr);
  PF_CHECK(table->IsRectangular());
  PF_CHECK(table->cells.size() == 2);
  PF_CHECK(table->columns.size() == 2);
  PF_CHECK(table->has_header_row);
}

PF_TEST(SaveIsAtomicAndReloadable) {
  auto tmp = std::filesystem::temp_directory_path() / "pf-save-test";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  auto file = tmp / "project.paper";

  SaveRequest req;
  req.save_id = IdGenerator::NewSaveId();
  req.project_id = ProjectId("p1");
  req.revision = ProjectRevision{10};
  req.destination = file;
  auto snapshot = MakeProject();
  snapshot.revision = ProjectRevision{10}; // saved revision must match
  req.snapshot = std::move(snapshot);

  auto save = ProjectPersistence::Save(req);
  PF_CHECK(save.status == SaveResult::Status::Ok);
  PF_CHECK(std::filesystem::exists(file));
  // No temp residue
  PF_CHECK(!std::filesystem::exists(file.string() + ".tmp-" + req.save_id));

  LoadRequest load;
  load.project_file = file;
  auto result = ProjectPersistence::Load(load);
  PF_CHECK(result.status == LoadResult::Status::Ok);
  PF_CHECK(result.project->revision.value == 10);
  {
    const Document &cdoc = result.project->document;
    PF_CHECK(cdoc.body().sections.size() == 1);
  }

  std::filesystem::remove_all(tmp);
}

PF_TEST(LoadMissingFileReportsError) {
  LoadRequest load;
  load.project_file = "/nonexistent/project.paper";
  auto result = ProjectPersistence::Load(load);
  PF_CHECK(result.status == LoadResult::Status::FileMissing);
}

PF_TEST(LoadBadJsonReportsError) {
  auto tmp = std::filesystem::temp_directory_path() / "pf-bad-test";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);
  auto file = tmp / "project.paper";
  {
    std::ofstream out(file);
    out << "{ this is not json";
  }

  LoadRequest load;
  load.project_file = file;
  auto result = ProjectPersistence::Load(load);
  PF_CHECK(result.status == LoadResult::Status::ParseError);
  std::filesystem::remove_all(tmp);
}

// Regression: a project written while the id generator was not load-aware can
// contain duplicate node ids - the counter restarted at zero after every open,
// so the first inserted block reused an id already on disk (two nodes both
// "n1"). Duplicate ids made an edit hit the wrong block and scrambled insert
// order. Loading must (a) heal the duplicates and (b) lift the generator past
// the stored ids so the next insert cannot collide either.
PF_TEST(LoadHealsDuplicateNodeIdsAndAdvancesGenerator) {
  // Deliberately far above anything this process has allocated, so the
  // assertions below hold no matter which tests ran first.
  constexpr std::uint64_t kHigh = 700000;
  // The figure and the paragraph share "n700000"; the section is "n699999".
  const std::string json = R"({
      "schemaVersion": "3",
      "projectId": "p-dup",
      "revision": 5,
      "template": "generic-article",
      "body": {
        "sections": [
          {
            "id": "n699999",
            "title": [{"type": "text", "text": "Intro"}],
            "blocks": [
              {"type": "figure", "id": "n700000", "assetId": "a1",
               "caption": [], "width": "100"},
              {"type": "paragraph", "id": "n700000",
               "content": [{"type": "text", "text": "hello"}]}
            ],
            "subsections": []
          }
        ]
      }
    })";

  auto project = ProjectSerializer::Deserialize(json);
  PF_CHECK(project.ok());
  if (!project.ok())
    return;

  // (a) The duplicate must be healed: every node id unique.
  const auto ids = project.value().document.CollectNodeIds();
  std::set<std::string> unique;
  for (const auto &id : ids)
    unique.insert(id.value());
  PF_CHECK_EQ(unique.size(), ids.size());
  // The first occurrence kept its id (figure is stored first).
  PF_CHECK(unique.count("n700000") == 1);

  // (b) The generator must now be past every stored id, so a freshly minted id
  // cannot clash with one already in the document.
  const std::string fresh = IdGenerator::NewNodeId();
  for (const auto &id : ids)
    PF_CHECK(id.value() != fresh);
  PF_CHECK(std::stoull(fresh.substr(1)) > kHigh);
}

// The figure's single-/double-column attribute must survive a save/load cycle,
// and a file written before the attribute existed must read as single-column
// (the old behaviour), never as an uninitialised value.
PF_TEST(FigureSpanPersistsAndDefaultsToSingleColumn) {
  const std::string json = R"({
      "schemaVersion": "3",
      "projectId": "p-span",
      "revision": 1,
      "template": "generic-article",
      "body": {
        "sections": [
          {
            "id": "n10",
            "title": [{"type": "text", "text": "Intro"}],
            "blocks": [
              {"type": "figure", "id": "n11", "assetId": "a1",
               "caption": [], "width": "100", "span": "double"},
              {"type": "figure", "id": "n12", "assetId": "a2",
               "caption": [], "width": "50"}
            ],
            "subsections": []
          }
        ]
      }
    })";

  auto project = ProjectSerializer::Deserialize(json);
  PF_CHECK(project.ok());
  if (!project.ok())
    return;
  // A const Document: the non-const body() is deliberately editor-only.
  const Document &loaded = project.value().document;
  const auto &blocks = loaded.body().sections[0].blocks;
  PF_CHECK_EQ(blocks.size(), std::size_t{2});
  if (blocks.size() != 2)
    return;
  const auto *wide = std::get_if<Figure>(&blocks[0]);
  const auto *legacy = std::get_if<Figure>(&blocks[1]);
  PF_CHECK(wide && wide->span == FigureSpan::DoubleColumn);
  // No "span" key at all: the pre-attribute default.
  PF_CHECK(legacy && legacy->span == FigureSpan::SingleColumn);
  PF_CHECK(legacy && legacy->width == FigureWidth::Percent50);

  // Serialize + reparse: the choice is written explicitly, so it comes back.
  const std::string out = ProjectSerializer::Serialize(project.value());
  auto again = ProjectSerializer::Deserialize(out);
  PF_CHECK(again.ok());
  if (!again.ok())
    return;
  const Document &reloaded = again.value().document;
  const auto &back = reloaded.body().sections[0].blocks;
  PF_CHECK_EQ(back.size(), std::size_t{2});
  if (back.size() != 2)
    return;
  const auto *wide_again = std::get_if<Figure>(&back[0]);
  const auto *single_again = std::get_if<Figure>(&back[1]);
  PF_CHECK(wide_again && wide_again->span == FigureSpan::DoubleColumn);
  PF_CHECK(single_again && single_again->span == FigureSpan::SingleColumn);
}
