#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>

#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "numbering/CitationNumberResolver.h"
#include "project/ProjectSession.h"
#include "render/LatexRenderer.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

ProjectSession::Config Config(const std::filesystem::path& workspace) {
    ProjectSession::Config config;
    config.install_root = PF_INSTALL_ROOT;
    config.workspace_root = workspace;
    config.debounce = std::chrono::milliseconds{0};
    return config;
}

struct Fixture {
    test::ScopedTempDir directory{"pf-wp04-project"};
    test::ScopedTempDir workspace{"pf-wp04-builds"};
    ProjectSession session{Config(workspace.path())};
    NodeId section;

    Fixture() {
        PF_CHECK(session.NewProject(directory.path()));
        PF_CHECK(
            Apply(SetTitlePayload{InlineFromText("Rich captions and tables")})
                .status == EditStatus::Applied);
        section = Apply(InsertSectionPayload{0, InlineFromText("Results")})
                      .created_node;
    }

    EditResult Apply(FullEditPayload payload) {
        EditCommand command;
        command.operation_id = OperationId(IdGenerator::NewOperationId());
        command.project_id = session.state().id();
        command.base_revision = session.current_revision();
        command.payload = std::move(payload);
        return session.Execute(command);
    }

    NodeId AddTable(size_t rows = 2, size_t columns = 3) {
        InsertTablePayload payload;
        payload.parent = section;
        payload.rows = rows;
        payload.columns.resize(columns);
        payload.has_header_row = true;
        const auto result = Apply(payload);
        PF_CHECK(result.status == EditStatus::Applied);
        return result.created_node;
    }

    Table* GetTable(const NodeId& id) {
        Block* block = FindBlock(session.mutable_document(), id);
        return block ? std::get_if<Table>(block) : nullptr;
    }
};

InlineContent Rich(const NodeId& target) {
    return {TextRun{"Loss ", static_cast<std::uint8_t>(TextMark::Strong)},
            InlineMath{{"\\mathcal{L}"}},
            TextRun{" from ", static_cast<std::uint8_t>(TextMark::Emphasis)},
            Citation{{"smith2020"}, CitationMode::Narrative},
            TextRun{"; see ", 0},
            CrossReference{target}};
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

} // namespace

PF_TEST(WP04TableCommandsPreserveRichCellsAndUndo) {
    Fixture fixture;
    const NodeId id = fixture.AddTable();
    const InlineContent content = Rich(fixture.section);
    PF_CHECK(fixture.Apply(SetTableCellPayload{id, 1, 2, content}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture
                 .Apply(SetTableColumnAlignmentPayload{id, 2,
                                                       ColumnAlignment::Right})
                 .status == EditStatus::Applied);
    PF_CHECK(fixture.Apply(SetTableHeaderRowPayload{id, false}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture.Apply(EditTableLabelPayload{id, "tab:results"}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture.Apply(InsertTableRowPayload{id, 1}).status ==
             EditStatus::Applied);
    PF_CHECK(
        fixture.Apply(InsertTableColumnPayload{id, 1, ColumnAlignment::Center})
            .status == EditStatus::Applied);
    PF_CHECK(fixture.GetTable(id)->cells[2][3].content == content);
    PF_CHECK(fixture.GetTable(id)->cells[1][1].content.empty());
    PF_CHECK(fixture.Apply(DeleteTableRowPayload{id, 0}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture.Apply(DeleteTableColumnPayload{id, 0}).status ==
             EditStatus::Applied);

    auto* table = fixture.GetTable(id);
    PF_CHECK(table && table->IsRectangular());
    PF_CHECK(table->RowCount() == 2 && table->ColumnCount() == 3);
    PF_CHECK(table->cells[1][2].content == content);
    PF_CHECK(table->columns[0].alignment == ColumnAlignment::Center);
    PF_CHECK(table->columns[2].alignment == ColumnAlignment::Right);
    PF_CHECK(!table->has_header_row && table->label == "tab:results");
    const auto revision = fixture.session.current_revision();
    PF_CHECK(fixture.session.Undo().status == EditStatus::Applied);
    PF_CHECK(fixture.GetTable(id)->ColumnCount() == 4);
    PF_CHECK(fixture.GetTable(id)->cells[1][3].content == content);
    PF_CHECK(fixture.session.current_revision().value > revision.value);
    PF_CHECK(fixture.session.Redo().status == EditStatus::Applied);
    PF_CHECK(fixture.GetTable(id)->cells[1][2].content == content);

    fixture.session.Save();
    const auto saved = fixture.session.FlushSaves();
    if (saved.status != SaveResult::Status::Ok)
        std::cout << saved.detail << "\n";
    PF_CHECK(saved.status == SaveResult::Status::Ok);
    if (saved.status != SaveResult::Status::Ok)
        return;
    fixture.session.CloseProject();
    PF_CHECK(fixture.session.OpenProject(fixture.directory.path(), nullptr));
    table = fixture.GetTable(id);
    PF_CHECK(table && table->IsRectangular());
    if (!table)
        return;
    PF_CHECK(table->cells[1][2].content == content);
    PF_CHECK(table->columns[0].alignment == ColumnAlignment::Center);
    PF_CHECK(table->columns[2].alignment == ColumnAlignment::Right);
    PF_CHECK(!table->has_header_row && table->label == "tab:results");
}

PF_TEST(WP04RejectedTableCommandsLeaveDocumentAndHistoryUntouched) {
    Fixture fixture;
    const NodeId id = fixture.AddTable(1, 1);
    const auto revision = fixture.session.current_revision();
    const auto version = fixture.session.state().document().version();
    PF_CHECK(fixture.Apply(DeleteTableColumnPayload{id, 0}).status ==
             EditStatus::Rejected);
    PF_CHECK(
        fixture.Apply(SetTableCellPayload{id, 1, 0, InlineFromText("outside")})
            .status == EditStatus::Rejected);
    PF_CHECK(fixture.Apply(InsertTableRowPayload{id, 2}).status ==
             EditStatus::Rejected);
    PF_CHECK(fixture.Apply(DeleteTableRowPayload{id, 1}).status ==
             EditStatus::Rejected);
    PF_CHECK(fixture.Apply(InsertTableColumnPayload{id, 2}).status ==
             EditStatus::Rejected);
    PF_CHECK(
        fixture
            .Apply(SetTableColumnAlignmentPayload{id, 1, ColumnAlignment::Left})
            .status == EditStatus::Rejected);
    PF_CHECK(fixture
                 .Apply(SetTableColumnAlignmentPayload{
                     id, 0, static_cast<ColumnAlignment>(42)})
                 .status == EditStatus::Rejected);
    PF_CHECK(
        fixture.Apply(SetTableCellPayload{fixture.section, 0, 0, {}}).status ==
        EditStatus::Rejected);
    PF_CHECK(fixture.Apply(EditFigureWidthPayload{id, FigureWidth::Percent50})
                 .status == EditStatus::Rejected);
    PF_CHECK(fixture.session.current_revision() == revision);
    PF_CHECK(fixture.session.state().document().version() == version);
    PF_CHECK(fixture.GetTable(id)->ColumnCount() == 1);
    PF_CHECK(fixture.session.Undo().status == EditStatus::Applied);
    PF_CHECK(fixture.GetTable(id) == nullptr); // 拒绝的命令没有占用撤销历史。
}

PF_TEST(WP04RichCaptionsSurviveSaveReopenAndUndo) {
    Fixture fixture;
    const NodeId table_id = fixture.AddTable();
    InsertFigurePayload figure;
    figure.parent = fixture.section;
    figure.asset_id = AssetId("caption-fixture-image");
    const NodeId figure_id = fixture.Apply(figure).created_node;
    const InlineContent content = Rich(table_id);
    for (const NodeId& id : {table_id, figure_id})
        PF_CHECK(fixture.Apply(EditCaptionPayload{id, content}).status ==
                 EditStatus::Applied);
    InlineContent changed = content;
    changed.push_back(TextRun{" updated", 0});
    PF_CHECK(fixture.Apply(EditCaptionPayload{figure_id, changed}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture.session.Undo().status == EditStatus::Applied);
    PF_CHECK(std::get<Figure>(
                 *FindBlock(fixture.session.mutable_document(), figure_id))
                 .caption == content);
    PF_CHECK(fixture.session.Redo().status == EditStatus::Applied);
    PF_CHECK(
        fixture.Apply(EditFigureWidthPayload{figure_id, FigureWidth::Percent50})
            .status == EditStatus::Applied);
    fixture.session.Save();
    PF_CHECK(fixture.session.FlushSaves().status == SaveResult::Status::Ok);
    fixture.session.CloseProject();
    PF_CHECK(fixture.session.OpenProject(fixture.directory.path(), nullptr));
    PF_CHECK(fixture.GetTable(table_id)->caption == content);
    const auto& stored = std::get<Figure>(
        *FindBlock(fixture.session.mutable_document(), figure_id));
    PF_CHECK(stored.caption == changed);
    PF_CHECK(stored.width == FigureWidth::Percent50);
}

PF_TEST(WP04TableSizeLimitsAreEnforcedBeforeMutation) {
    PF_CHECK(!DocumentEditor::MakeTable(std::vector<TableColumn>(1),
                                        Table::kMaxRows + 1, false));
    PF_CHECK(!DocumentEditor::MakeTable(
        std::vector<TableColumn>(Table::kMaxColumns + 1), 1, false));
    Fixture fixture;
    const NodeId tall = fixture.AddTable(Table::kMaxRows, 1);
    const NodeId wide = fixture.AddTable(1, Table::kMaxColumns);
    const auto revision = fixture.session.current_revision();
    PF_CHECK(fixture.Apply(InsertTableRowPayload{tall, 0}).status ==
             EditStatus::Rejected);
    PF_CHECK(fixture.Apply(InsertTableColumnPayload{wide, 0}).status ==
             EditStatus::Rejected);
    PF_CHECK(fixture.session.current_revision() == revision);
    PF_CHECK(fixture.GetTable(tall)->RowCount() == Table::kMaxRows);
    PF_CHECK(fixture.GetTable(wide)->ColumnCount() == Table::kMaxColumns);
}

PF_TEST(WP04NestedTableReferencesArePersistedAndValidated) {
    Fixture fixture;
    const NodeId sub =
        fixture
            .Apply(InsertSubsectionPayload{0, 0, InlineFromText("Subsection")})
            .created_node;
    const NodeId subsub = fixture
                              .Apply(InsertSubsubsectionPayload{
                                  0, 0, 0, InlineFromText("Details")})
                              .created_node;
    InsertTablePayload payload;
    payload.parent = subsub;
    payload.rows = 1;
    payload.columns.resize(2);
    const NodeId id = fixture.Apply(payload).created_node;
    PF_CHECK(fixture
                 .Apply(EditCaptionPayload{
                     id, InlineContent{CrossReference{subsub}}})
                 .status == EditStatus::Applied);
    PF_CHECK(fixture
                 .Apply(SetTableCellPayload{
                     id, 0, 0, InlineContent{CrossReference{fixture.section}}})
                 .status == EditStatus::Applied);
    PF_CHECK(fixture
                 .Apply(SetTableCellPayload{id, 0, 1,
                                            InlineContent{CrossReference{sub}}})
                 .status == EditStatus::Applied);
    fixture.session.Save();
    PF_CHECK(fixture.session.FlushSaves().status == SaveResult::Status::Ok);
    auto saved = ProjectSerializer::Deserialize(
        Read(fixture.directory / "project.paper"));
    PF_CHECK(saved.ok());
    if (!saved)
        return;
    DocumentEditor editor(saved.value().document);
    PF_CHECK(editor
                 .SetTableCell(
                     id, 0, 0,
                     InlineContent{CrossReference{NodeId("missing-heading")}})
                 .ok());
    const auto invalid = ProjectSerializer::Deserialize(
        ProjectSerializer::Serialize(saved.value()));
    PF_CHECK(!invalid.ok());
    if (!invalid)
        PF_CHECK(invalid.error().find("cross-reference") != std::string::npos);
}

PF_TEST(WP04CellCitationsParticipateInPdfReadingOrder) {
    Fixture fixture;
    const auto imported = fixture.session.ImportBibliography(
        "@article{heading, title={Heading}}\n@article{caption, "
        "title={Caption}}\n"
        "@article{cell, title={Cell}}\n@article{subheading, "
        "title={Subheading}}");
    PF_CHECK(imported.status == BibliographyImportResult::Status::Ok);
    const auto cite = [](const std::string& key) {
        return InlineContent{Citation{{key}, CitationMode::Parenthetical}};
    };
    PF_CHECK(
        fixture.Apply(RenameSectionPayload{fixture.section, cite("heading")})
            .status == EditStatus::Applied);
    const NodeId id = fixture.AddTable(1, 1);
    PF_CHECK(fixture.Apply(EditCaptionPayload{id, cite("caption")}).status ==
             EditStatus::Applied);
    PF_CHECK(
        fixture.Apply(SetTableCellPayload{id, 0, 0, cite("cell")}).status ==
        EditStatus::Applied);
    PF_CHECK(fixture.Apply(InsertSubsectionPayload{0, 0, cite("subheading")})
                 .status == EditStatus::Applied);
    const auto numbers = CitationNumberResolver::Build(
        fixture.session.state().document(), fixture.session.bibliography());
    PF_CHECK(numbers.FormatPill({"heading"}) == "[1]");
    PF_CHECK(numbers.FormatPill({"caption"}) == "[2]");
    PF_CHECK(numbers.FormatPill({"cell"}) == "[3]");
    PF_CHECK(numbers.FormatPill({"subheading"}) == "[4]");
}

PF_TEST(WP04RichCellMathAndCitationsAreValidated) {
    Fixture fixture;
    const NodeId id = fixture.AddTable();
    InlineContent content{InlineMath{{"\\mathscr{F}"}},
                          Citation{{"missing"}, CitationMode::Parenthetical}};
    PF_CHECK(fixture.Apply(SetTableCellPayload{id, 0, 0, content}).status ==
             EditStatus::Applied);
    ValidationInput input;
    input.document = &fixture.session.state().document();
    input.template_id = "generic-article";
    input.bibliography_keys = {"present"};
    const auto validation = Validator().Validate(input);
    bool missing_package = false, missing_citation = false;
    for (const auto& diagnostic : validation.diagnostics) {
        missing_package |= diagnostic.code == "E-MATH-MISSING-PACKAGE";
        missing_citation |= diagnostic.code == "E-CITATION-UNKNOWN-KEY";
    }
    PF_CHECK(missing_package);
    PF_CHECK(missing_citation);
}

PF_TEST(WP04RichCaptionsAndTableReferencesReachTheBuiltPdf) {
    Fixture fixture;
    PF_CHECK(fixture.session
                 .ImportBibliography(
                     "@article{smith2020, author={Smith, Jane}, title={An "
                     "example}, journal={Journal}, year={2020}}")
                 .status == BibliographyImportResult::Status::Ok);
    const NodeId id = fixture.AddTable(2, 2);
    const InlineContent content = Rich(fixture.section);
    PF_CHECK(fixture.Apply(EditCaptionPayload{id, content}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture.Apply(EditTableLabelPayload{id, "tab:rich"}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture
                 .Apply(SetTableCellPayload{
                     id, 0, 0,
                     InlineFromText("Metric", static_cast<std::uint8_t>(
                                                  TextMark::Strong))})
                 .status == EditStatus::Applied);
    PF_CHECK(fixture.Apply(SetTableCellPayload{id, 1, 0, content}).status ==
             EditStatus::Applied);
    PF_CHECK(fixture
                 .Apply(SetTableColumnAlignmentPayload{id, 1,
                                                       ColumnAlignment::Right})
                 .status == EditStatus::Applied);
    InsertParagraphPayload paragraph;
    paragraph.parent = fixture.section;
    paragraph.content = {TextRun{"See Table ", 0}, CrossReference{id}};
    PF_CHECK(fixture.Apply(paragraph).status == EditStatus::Applied);

    std::optional<BuildResult> result;
    fixture.session.SetBuildResultHandler(
        [&](const BuildResult& value) { result = value; });
    fixture.session.RequestBuild(true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!result && std::chrono::steady_clock::now() < deadline)
        fixture.session.WaitForApplicationEvent(std::chrono::milliseconds(20));
    PF_CHECK(result.has_value());
    if (!result)
        return;
    if (result->outcome != BuildResult::Outcome::Success)
        std::cout << result->log << "\n";
    PF_CHECK(result->outcome == BuildResult::Outcome::Success);
    PF_CHECK(std::filesystem::is_regular_file(result->pdf_path));
    if (result->outcome != BuildResult::Outcome::Success)
        return;
    const auto workspace = result->pdf_path.parent_path();
    const std::string tex = Read(workspace / "main.tex");
    PF_CHECK(tex.find("\\caption{\\textbf{Loss }") != std::string::npos);
    PF_CHECK(tex.find("\\mathcal{L}") != std::string::npos);
    PF_CHECK(tex.find("\\citet{smith2020}") != std::string::npos);
    PF_CHECK(tex.find("\\begin{tabular}{lr}") != std::string::npos);
    PF_CHECK(tex.find("\\midrule") != std::string::npos);
    PF_CHECK(tex.find("\\label{tab:rich}") != std::string::npos);
    PF_CHECK(tex.find("\\ref{tab:rich}") != std::string::npos);
    PF_CHECK(Read(workspace / "main.aux").find("\\newlabel{tab:rich}") !=
             std::string::npos);
}
