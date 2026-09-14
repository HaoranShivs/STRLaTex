// Persistence tests: serialize/load round-trip, atomic save, stale-save guard.
#include "TestMain.hpp"

#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include <fstream>

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

    DisplayEquation eq;
    eq.math_source = "y = f(x)";
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

}  // namespace

PF_TEST(SerializeDeserializeRoundTrip) {
    auto sp = MakeProject();
    std::string json = ProjectSerializer::Serialize(sp);
    PF_CHECK(!json.empty());

    auto parsed = ProjectSerializer::Deserialize(json);
    PF_CHECK(parsed.ok());
    const auto& back = parsed.value();
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
    const Document& cdoc = back.document;
    const auto& blocks = cdoc.body().sections[0].blocks;
    const auto* table = std::get_if<Table>(&blocks[1]);
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
    snapshot.revision = ProjectRevision{10};  // saved revision must match
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
        const Document& cdoc = result.project->document;
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
    { std::ofstream out(file); out << "{ this is not json"; }

    LoadRequest load;
    load.project_file = file;
    auto result = ProjectPersistence::Load(load);
    PF_CHECK(result.status == LoadResult::Status::ParseError);
    std::filesystem::remove_all(tmp);
}
