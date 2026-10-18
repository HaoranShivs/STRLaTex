#include "ScopedTempDir.hpp"
#include "TestMain.hpp"
#include "VectorPdfFixture.hpp"

#include "asset/AssetManager.h"
#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "project/ProjectSession.h"
#include <fstream>
#include <iterator>

using namespace pf;

namespace {
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

struct Converter final : IAssetConverter {
    bool succeed = true;
    bool valid_output = true;
    std::filesystem::path last_work;
    std::optional<std::filesystem::path>
    Convert(const std::filesystem::path&, const std::filesystem::path& work,
            std::string* error) override {
        last_work = work;
        if (!succeed) {
            if (error)
                *error = "converter failed deliberately";
            return std::nullopt;
        }
        const auto result = work / (valid_output ? "result.pdf" : "result.svg");
        test::WriteVectorPdf(result);
        return result;
    }
};

ProjectSession::Config Config(const std::filesystem::path& workspace) {
    ProjectSession::Config config;
    config.install_root = PF_INSTALL_ROOT;
    config.workspace_root = workspace;
    config.debounce = std::chrono::hours(1);
    return config;
}

struct Fixture {
    test::ScopedTempDir directory{"pf-wp05-project"};
    test::ScopedTempDir workspace{"pf-wp05-work"};
    ProjectSession session{Config(workspace)};
    NodeId section;
    Fixture() {
        PF_CHECK(session.NewProject(directory));
        Apply(SetTitlePayload{InlineFromText("Vector figures")});
        section = Apply(InsertSectionPayload{0, InlineFromText("Results")})
                      .created_node;
        test::WriteVectorPdf(directory / "original.pdf");
        test::WriteVectorPdf(directory / "replacement.pdf");
    }
    EditResult Apply(FullEditPayload payload) {
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = session.state().id();
        cmd.base_revision = session.current_revision();
        cmd.payload = std::move(payload);
        return session.Execute(cmd);
    }
    Figure* Get(const NodeId& id) {
        auto* block = FindBlock(session.mutable_document(), id);
        return block ? std::get_if<Figure>(block) : nullptr;
    }
};
} // namespace

PF_TEST(WP05PdfImportsPreserveVectorBytesAndSafeDistinctNames) {
    test::ScopedTempDir directory("pf-wp05-assets");
    const auto source = directory / "plot with spaces.PDF";
    test::WriteVectorPdf(source);
    AssetManager manager(directory / "assets");
    const auto imported = manager.Stage({source, ProjectId("p")});
    PF_CHECK(imported.status == AssetImportResult::Status::Ok);
    PF_CHECK(imported.metadata.kind == AssetKind::Pdf &&
             imported.metadata.IsVector());
    PF_CHECK(imported.metadata.media_type == "application/pdf");
    PF_CHECK(imported.metadata.relative_path == "plot_with_spaces.pdf");
    PF_CHECK(imported.metadata.original_name == "plot with spaces.PDF");
    PF_CHECK(imported.metadata.file_size > 0 &&
             !imported.metadata.content_hash.empty());
    PF_CHECK(imported.metadata.width == 0 && imported.metadata.height == 0);
    PF_CHECK(Read(manager.assets_dir() / imported.metadata.relative_path) ==
             Read(source));
    PF_CHECK(manager.registry().All().empty());
    const auto second = manager.Stage({source, ProjectId("p")});
    PF_CHECK(second.status == AssetImportResult::Status::Ok);
    PF_CHECK(second.asset_id != imported.asset_id &&
             second.metadata.relative_path != imported.metadata.relative_path);
    PF_CHECK(Read(manager.assets_dir() / imported.metadata.relative_path) ==
             Read(source));
}

PF_TEST(WP05VectorFormatsRequireConvertersAndRespectRasterOnlyOption) {
    test::ScopedTempDir directory("pf-wp05-no-converter");
    AssetManager manager(directory / "assets");
    for (const auto* extension : {"svg", "eps", "pdf"}) {
        const auto path = directory / (std::string("plot.") + extension);
        std::ofstream(path) << "vector source";
        PF_CHECK(manager.Stage({path, ProjectId("p")}, false).status ==
                 AssetImportResult::Status::UnsupportedType);
        if (std::string(extension) == "pdf")
            continue;
        const auto result = manager.Stage({path, ProjectId("p")});
        PF_CHECK(result.status == AssetImportResult::Status::UnsupportedType);
        PF_CHECK(result.detail.find(std::string(extension) == "svg"
                                        ? "rsvg-convert"
                                        : "epstopdf") != std::string::npos);
    }
    PF_CHECK(std::filesystem::is_empty(manager.assets_dir()));
    PF_CHECK(manager.Stage({directory, ProjectId("p")}).status ==
             AssetImportResult::Status::SourceMissing);
}

PF_TEST(WP05ConvertedAssetsUseFinalTypeAndAlwaysRemoveScratchFiles) {
    test::ScopedTempDir directory("pf-wp05-convert");
    auto converter = std::make_shared<Converter>();
    AssetManager manager(directory / "assets", converter);
    for (const auto* extension : {"svg", "eps"}) {
        const auto source = directory / (std::string("plot.") + extension);
        std::ofstream(source) << "source";
        const auto result = manager.Stage({source, ProjectId("p")});
        PF_CHECK(result.status == AssetImportResult::Status::Ok);
        PF_CHECK(result.metadata.kind == AssetKind::Pdf);
        PF_CHECK(result.metadata.media_type == "application/pdf");
        PF_CHECK(
            std::filesystem::path(result.metadata.relative_path).extension() ==
            ".pdf");
        PF_CHECK(result.metadata.original_name == source.filename().string());
        PF_CHECK(!std::filesystem::exists(converter->last_work));
    }
    const auto files =
        std::distance(std::filesystem::directory_iterator(manager.assets_dir()),
                      std::filesystem::directory_iterator());
    converter->succeed = false;
    const auto failed = manager.Stage({directory / "plot.svg", ProjectId("p")});
    PF_CHECK(failed.status == AssetImportResult::Status::IoError);
    PF_CHECK(failed.detail == "converter failed deliberately");
    PF_CHECK(!std::filesystem::exists(converter->last_work));
    converter->succeed = true;
    converter->valid_output = false;
    PF_CHECK(manager.Stage({directory / "plot.svg", ProjectId("p")}).status ==
             AssetImportResult::Status::IoError);
    PF_CHECK(!std::filesystem::exists(converter->last_work));
    PF_CHECK(
        std::distance(std::filesystem::directory_iterator(manager.assets_dir()),
                      std::filesystem::directory_iterator()) == files);
}

PF_TEST(WP05ReplacingFigurePreservesIdentityRichCaptionLayoutAndUndo) {
    Fixture fixture;
    const NodeId id =
        fixture.session
            .InsertFigureFromSource(fixture.directory / "original.pdf",
                                    fixture.section)
            .created_node;
    PF_CHECK(!id.empty());
    if (!fixture.Get(id))
        return;
    const AssetId original = fixture.Get(id)->asset_id;
    const InlineContent caption{
        TextRun{"Vector ", static_cast<std::uint8_t>(TextMark::Strong)},
        InlineMath{{"x^2"}}};
    fixture.Apply(EditCaptionPayload{id, caption});
    fixture.Apply(EditFigureLabelPayload{id, "fig:vector"});
    fixture.Apply(EditFigureSpanPayload{id, FigureSpan::DoubleColumn});
    fixture.Apply(EditFigureWidthPayload{id, FigureWidth::Percent50});
    PF_CHECK(
        fixture.session
            .ReplaceFigureFromSource(id, fixture.directory / "replacement.pdf")
            .status == EditStatus::Applied);
    const AssetId replacement = fixture.Get(id)->asset_id;
    PF_CHECK(replacement != original);
    PF_CHECK(fixture.Get(id)->caption == caption &&
             fixture.Get(id)->label == "fig:vector");
    PF_CHECK(fixture.Get(id)->width == FigureWidth::Percent50 &&
             fixture.Get(id)->span == FigureSpan::DoubleColumn);
    PF_CHECK(fixture.session.assets().registry().Find(original));
    PF_CHECK(fixture.session.Undo().status == EditStatus::Applied);
    PF_CHECK(fixture.Get(id)->asset_id == original);
    PF_CHECK(fixture.session.Redo().status == EditStatus::Applied);
    PF_CHECK(fixture.Get(id)->asset_id == replacement);
    fixture.session.Save();
    PF_CHECK(fixture.session.FlushSaves().status == SaveResult::Status::Ok);
    fixture.session.CloseProject();
    PF_CHECK(fixture.session.OpenProject(fixture.directory, nullptr));
    PF_CHECK(fixture.Get(id) && fixture.Get(id)->asset_id == replacement);
    PF_CHECK(fixture.Get(id)->caption == caption &&
             fixture.Get(id)->label == "fig:vector");
    PF_CHECK(fixture.Get(id)->width == FigureWidth::Percent50 &&
             fixture.Get(id)->span == FigureSpan::DoubleColumn);
    const auto* metadata =
        fixture.session.assets().registry().Find(replacement);
    PF_CHECK(metadata && metadata->kind == AssetKind::Pdf);
}

PF_TEST(WP05RejectedFigureImportsDoNotChangeRevisionRegistryOrFiles) {
    Fixture fixture;
    const auto revision = fixture.session.current_revision();
    const auto version = fixture.session.state().document().version();
    PF_CHECK(fixture.session
                 .ReplaceFigureFromSource(fixture.section,
                                          fixture.directory / "original.pdf")
                 .status == EditStatus::Rejected);
    PF_CHECK(fixture.session
                 .InsertFigureFromSource(fixture.directory / "original.pdf",
                                         NodeId("missing"))
                 .status == EditStatus::Rejected);
    PF_CHECK(fixture.session
                 .InsertFigureFromSource(fixture.directory / "missing.png",
                                         fixture.section)
                 .status == EditStatus::Rejected);
    PF_CHECK(fixture.session.current_revision() == revision);
    PF_CHECK(fixture.session.state().document().version() == version);
    PF_CHECK(fixture.session.assets().registry().All().empty());
    PF_CHECK(std::filesystem::is_empty(fixture.session.paths().assets_dir));
    // Failed imports must not add an undo entry.
    PF_CHECK(fixture.session.Undo().status == EditStatus::Applied);
    PF_CHECK(fixture.session.state().document().body().sections.empty());
    fixture.session.CloseProject();
    PF_CHECK(fixture.session
                 .InsertFigureFromSource(fixture.directory / "original.pdf",
                                         fixture.section)
                 .status == EditStatus::Rejected);
    PF_CHECK(std::filesystem::is_empty(fixture.directory / "assets"));
}

PF_TEST(WP05VectorFigureBuildsWithRealGraphicxAndCopiesOriginalPdf) {
    Fixture fixture;
    const auto imported = fixture.session.InsertFigureFromSource(
        fixture.directory / "original.pdf", fixture.section);
    PF_CHECK(imported.status == EditStatus::Applied);
    if (!fixture.Get(imported.created_node))
        return;
    const AssetId asset = fixture.Get(imported.created_node)->asset_id;
    const auto metadata = *fixture.session.assets().registry().Find(asset);
    fixture.Apply(EditCaptionPayload{imported.created_node,
                                     InlineFromText("Vector test")});
    fixture.Apply(
        EditFigureWidthPayload{imported.created_node, FigureWidth::Percent50});
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
        std::cout << result->log << '\n';
    PF_CHECK(result->outcome == BuildResult::Outcome::Success);
    const auto workspace = result->pdf_path.parent_path();
    PF_CHECK(Read(workspace / "main.tex")
                 .find("width=0.5\\linewidth]{assets/" +
                       metadata.relative_path + "}") != std::string::npos);
    PF_CHECK(Read(workspace / "assets" / metadata.relative_path) ==
             Read(fixture.directory / "original.pdf"));
    PF_CHECK(Read(result->pdf_path).find("/Subtype /Form") !=
             std::string::npos);
}
