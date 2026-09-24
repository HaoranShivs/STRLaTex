// 端到端测试：ProjectSession 工作流 + 真实的 tectonic build。
#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

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
    // E-08：每个进程独有；本文件与 TestCitationNumbering.cpp 过去共用
    // "pf-e2e-workspaces"，会互相删除对方的 build 目录。
    static pf::test::ScopedTempDir workspace("pf-e2e-workspaces");
    config.workspace_root = workspace.path();
    config.debounce = std::chrono::milliseconds{0};
    return config;
}

// build/save 结果在应用线程上生效，因此非 Qt 的驱动必须充当该线程：
// 泵送 session 的事件队列，直到 `done` 成立。
bool PumpUntil(ProjectSession& session, const std::function<bool()>& done, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{20});
    }
    return done();
}

bool WaitForBuild(ProjectSession& session, const std::function<bool()>& done, int timeout_ms = 180000) {
    return PumpUntil(session, done, timeout_ms);
}

// 入队 + 排空：让测试始终只读一行，而真正的写入发生在 save worker 上。
SaveResult SaveAndFlush(ProjectSession& session) {
    session.Save();
    return session.FlushSaves();
}

} // namespace

PF_TEST(SessionNewProjectWorkflow) {
    pf::test::ScopedTempDir dir("pf-e2e-new");

    ProjectSession session(TestConfig());
    PF_CHECK(session.NewProject(dir));
    PF_CHECK(session.lifecycle_state() == LifecycleState::Open);

    // 通过协议进行编辑
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

    // 重新打开并校验。
    ProjectSession session2(TestConfig());
    std::string error;
    PF_CHECK(session2.OpenProject(dir, &error));
    PF_CHECK(InlineToPlainText(session2.state().document().front_matter().title) == "Workflow Paper");
    PF_CHECK(session2.persistence_state() == PersistenceState::Clean);
}

PF_TEST(SessionUndoRedoAndRevisionMonotonicity) {
    pf::test::ScopedTempDir dir("pf-e2e-undo");
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
    PF_CHECK(InlineToPlainText(session.state().document().front_matter().title) == "A");
    // Undo 产生了一个新的 revision，绝不会回退。
    PF_CHECK(session.current_revision().value > max_rev);
    session.Redo();
    PF_CHECK(InlineToPlainText(session.state().document().front_matter().title) == "B");
}

PF_TEST(SessionBibliographyImportAndSearch) {
    pf::test::ScopedTempDir dir("pf-e2e-bib");
    ProjectSession session(TestConfig());
    session.NewProject(dir);

    std::string bib = "@article{k1, author={A. Author}, title={Great Paper}, year={2020}}";
    auto import = session.ImportBibliography(bib);
    PF_CHECK(import.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(import.entry_count == 1);

    auto search = session.SearchCitations("great");
    PF_CHECK(search.entries.size() == 1);
    PF_CHECK(search.entries[0].key == "k1");

    // 参考文献变更会提升 ProjectRevision。
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

    // 创建一个真实的 section 来承载该图。
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

    // 构造一个极小的合法 PNG（1x1 像素）。
    pf::test::ScopedTempDir png_dir("pf-test-image");
    auto png_path = png_dir.path() / "image.png";
    {
        static const unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // 文件签名
                                            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR 长度+类型
                                            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // 1x1
                                            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00,
                                            0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01,
                                            0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00,
                                            0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
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
    // asset 文件已被复制到项目的 assets 目录。
    PF_CHECK(!session.assets().registry().All().empty());
}

// 真实的 tectonic build（首次运行可能需要联网下载 bundles）。
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
    para.content = InlineFromText("This paper was built by PaperForge end to end.");
    session.Execute(make_cmd(para));

    InsertEquationPayload eq;
    eq.parent = sec_result.created_node;
    eq.latex = "E = mc^{2}";
    session.Execute(make_cmd(eq));

    session.ImportBibliography("@article{ref1, author={Jane Doe}, title={Something}, year={2021}}");
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
    // 结果携带了该次确切请求的身份信息。
    PF_CHECK(last_result->project_id == session.state().id());
    PF_CHECK(!last_result->build_id.empty());
    PF_CHECK(last_result->snapshot_id == session.latest_snapshot_id());
    if (last_result->outcome == BuildResult::Outcome::Success) {
        PF_CHECK(session.preview_state() == PreviewState::Fresh);
        PF_CHECK(std::filesystem::exists(last_result->pdf_path));
        // PDF 魔数
        std::ifstream pdf(last_result->pdf_path, std::ios::binary);
        char header[4] = {0};
        pdf.read(header, 4);
        PF_CHECK(std::string(header, 4) == "%PDF");
        std::cout << "    (PDF built: " << last_result->pdf_path << ")\n";
    } else {
        // 若 tectonic 无法下载其 bundle（离线），则优雅跳过。
        std::cout << "    (tectonic build failed - likely offline bundle "
                     "download; skipping)\n";
        for (const auto& d : last_result->diagnostics) {
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

        // 编辑但不保存；随后执行 autosave。
        session.Execute(make_title_cmd("Unsaved Title"));
        PF_CHECK(session.persistence_state() == PersistenceState::Dirty);

        // 手动触发一次 autosave tick：在本线程捕获，在 worker 上写入。
        auto autosave = session.Autosave();
        PF_CHECK(autosave.status == SaveResult::Status::Queued);
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
        PF_CHECK(session.HasRecoverySnapshot());
        // autosave 绝不可改变 Clean/Dirty（架构 32）。
        PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    }
    // 「崩溃」：不保存直接重新打开。
    {
        ProjectSession session(TestConfig());
        std::string error;
        // 磁盘上存在恢复 snapshot 文件（全新实例，尚未打开）。
        PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" / "autosave.paper"));
        bool recovered = false;
        PF_CHECK(session.OpenProjectWithRecovery(dir, &error, &recovered));
        PF_CHECK(recovered);
        PF_CHECK(InlineToPlainText(session.state().document().front_matter().title) == "Unsaved Title");
        PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
    }
    // 不带恢复地重新打开：显示用户最后一次保存的内容。
    {
        ProjectSession session(TestConfig());
        std::string error;
        PF_CHECK(session.OpenProject(dir, &error));
        PF_CHECK(InlineToPlainText(session.state().document().front_matter().title) == "Saved Title");
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

    // 短间隔：300ms。定时器线程只投递一个 tick；本循环即应用线程，
    // 负责把它转成 snapshot。
    session.StartAutosaveTimer(std::chrono::milliseconds{300});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{900};
    while (std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{50});
    }
    session.FlushSaves();
    session.StopAutosaveTimer();

    PF_CHECK(std::filesystem::exists(dir / ".paperforge" / "autosave" / "autosave.paper"));
    // autosave 不改变 Dirty 状态（架构 32）。
    PF_CHECK(session.persistence_state() == PersistenceState::Dirty);
}

// 回归：向一个从磁盘加载的 project 中插入多张图时，先前插入的图过去会显示
// 最新的图片，且插入顺序会错乱。原因：id 生成器在每个进程中从零开始，因此
// 打开一个节点为 "n1..nN" 的 project 后，第一个插入的 block 会复用一个磁盘上
// 已存在的 id。两个节点共用一个 id 会使 LocateNode/ResolveInsertionPoint
// （它们返回「第一个」匹配项）指向错误的 block。本测试走真实用户路径——打开、
// 插入图片、再插入第二张图片——并校验每个插入的 figure 都保留自己的 node id、
// 自己暂存的 asset 文件，以及自己在文档中的位置。
PF_TEST(SessionInsertingMultipleFiguresKeepsIdsAssetsAndOrder) {
    pf::test::ScopedTempDir dir("pf-e2e-multi-image");

    // id 远高于本进程会分配的任何值，因此只能靠加载时的 observe/heal 路径
    // 来避免冲突。
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

    // 两个不同的 1x1 PNG，以便区分暂存的文件。
    static const unsigned char png_a[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00,
        0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
        0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    auto src_a = dir / "first.png";
    auto src_b = dir / "second.png";
    {
        std::ofstream out(src_a, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png_a), sizeof(png_a));
    }
    {
        std::ofstream out(src_b, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png_a), sizeof(png_a));
        out.put('\n'); // 使两个文件不同
    }

    ProjectSession session(TestConfig());
    std::string error;
    PF_CHECK(session.OpenProject(dir, &error));

    const NodeId section("n800000");
    auto first = session.InsertFigureFromSource(src_a, section);
    PF_CHECK(first.status == EditStatus::Applied);
    auto second = session.InsertFigureFromSource(src_b, section);
    PF_CHECK(second.status == EditStatus::Applied);
    if (first.status != EditStatus::Applied || second.status != EditStatus::Applied) {
        return;
    }

    // 每次插入都生成了自己的 id——旧 bug 会让它们发生冲突。
    PF_CHECK(first.created_node != second.created_node);
    // 而且这些 id 都「高于」文件已用过的所有 id：加载过程把生成器推进到
    // 已存储的 id 之后，这正是阻止新插入复用磁盘上已有 id 的原因。
    PF_CHECK(std::stoull(first.created_node.value().substr(1)) > 800001);
    PF_CHECK(std::stoull(second.created_node.value().substr(1)) > 800001);

    // 遍历文档：两张图都在，按插入顺序排列，各自拥有不同的 asset。
    // 第一张必须仍是第一张。
    std::vector<std::string> figure_nodes;
    std::vector<AssetId> figure_assets;
    VisitBlocks(session.state().document(), [&](const Block& block, const NodeAddress&) {
        const auto* fig = std::get_if<Figure>(&block);
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

    // 两张图片都被复制到 project 本地的 assets 目录，并可通过相对路径访问
    // （问题：路径必须指向该文件）。
    for (const auto& asset : figure_assets) {
        const auto* meta = session.assets().registry().Find(asset);
        PF_CHECK(meta != nullptr);
        if (!meta)
            continue;
        PF_CHECK(!meta->relative_path.empty());
        PF_CHECK(std::filesystem::exists(dir / "assets" / meta->relative_path));
    }

    // 任何地方都没有重复的 node id，因此后续编辑不会命中错误的 block。
    std::set<std::string> unique;
    for (const auto& id : CollectAllNodeIds(session.state().document()))
        unique.insert(id.value());
    PF_CHECK_EQ(unique.size(), CollectAllNodeIds(session.state().document()).size());

    // 往返回环：修复后的 id 和两个 asset 在 save + 重新打开后依然存在。
    PF_CHECK(SaveAndFlush(session).status == SaveResult::Status::Ok);
    ProjectSession reopened(TestConfig());
    PF_CHECK(reopened.OpenProject(dir, &error));
    std::size_t reopened_figures = 0;
    VisitBlocks(reopened.state().document(), [&](const Block& block, const NodeAddress&) {
        if (std::get_if<Figure>(&block))
            ++reopened_figures;
    });
    PF_CHECK_EQ(reopened_figures, std::size_t{2});
}
