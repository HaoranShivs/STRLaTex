// P0-04 回归测试：I/O 处的项目相对路径信任边界。反序列化阶段的校验位于
// TestDeserializationSafety.cpp；本文件覆盖第二道检查点——紧接真正文件系统
// 访问之前的解析，包括评审特别指出的符号链接逃逸场景。
#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <fstream>
#include <string>

#include "core/ProjectPath.h"
#include "project/ProjectSession.h"
#include "project/SnapshotFactory.h"

using namespace pf;

namespace {

std::filesystem::path TempDir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

class NullCompiler final : public ICompiler {
  public:
    CompileResult Compile(const CompileRequest&, const std::atomic<bool>*) override {
        CompileResult result;
        result.status = CompileStatus::Success;
        return result;
    }
};

} // namespace

PF_TEST(ResolveRejectsTraversalThatEscapesRoot) {
    auto root = TempDir("pf-boundary-root");
    std::filesystem::create_directories(root / "assets");

    // 名称与根目录共享前缀的同级目录绝不可达：包含性按路径分量判断，
    // 而非字符串前缀比较。
    auto sibling = root.parent_path() / (root.filename().string() + "2");
    std::filesystem::create_directories(sibling);

    auto escape = ProjectRelativePath::Parse("assets/../../outside.txt");
    PF_CHECK(!escape.ok());

    auto prefix_lookalike = ResolveUntrustedProjectPath(root, "assets/x.png");
    PF_CHECK(prefix_lookalike.ok());
    if (prefix_lookalike.ok()) {
        const std::string resolved = prefix_lookalike.value().string();
        const std::string sibling_prefix = sibling.string();
        PF_CHECK(resolved.compare(0, sibling_prefix.size(), sibling_prefix) != 0);
    }
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(sibling);
}

// 项目内指向外部的符号链接，不得让相对路径读取到目标：weakly_canonical
// 解析该链接，随后的分量遍历即可发现逃逸。
PF_TEST(ResolveRejectsSymlinkEscape) {
    auto root = TempDir("pf-boundary-symlink");
    std::filesystem::create_directories(root / "assets");

    auto outside = TempDir("pf-boundary-outside");
    std::filesystem::create_directories(outside);
    {
        std::ofstream out(outside / "secret.txt");
        out << "secret";
    }
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside, root / "assets" / "link", ec);
    if (ec) {
        // 此处的文件系统不支持符号链接：显式跳过，而不是
        // 为一项从未执行的检查报告通过。
        std::cout << "  [SKIP] symlinks unsupported: " << ec.message() << "\n";
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        return;
    }

    auto escaped = ResolveUntrustedProjectPath(root, "assets/link/secret.txt");
    PF_CHECK(!escaped.ok());
    if (!escaped.ok())
        PF_CHECK(escaped.error() == PathError::OutsideProjectRoot);

    // 链接本身解析到根目录之外，同样必须被拒绝。
    auto link_itself = ResolveUntrustedProjectPath(root, "assets/link");
    PF_CHECK(!link_itself.ok());
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

// 即使注册表中存有恶意的相对路径，编译器 snapshot 也不得把项目之外的
// 资源源文件交给 build。
PF_TEST(BuildSnapshotDropsAssetsThatEscapeTheProject) {
    auto dir = TempDir("pf-boundary-snapshot");
    NullCompiler compiler;
    ProjectSession::Config config;
    config.compiler_factory = [&compiler]() -> std::unique_ptr<ICompiler> { return std::make_unique<NullCompiler>(); };
    config.debounce = std::chrono::milliseconds{0};
    static pf::test::ScopedTempDir workspace("pf-boundary-workspaces");
    config.workspace_root = workspace.path();
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));

    // 注册一个存储路径试图离开 assets 目录的资源。
    AssetMetadata hostile;
    hostile.id = AssetId("a-evil");
    hostile.relative_path = "../../etc/passwd";
    hostile.media_type = "image/png";
    session.assets().registry().Register(hostile);

    AssetMetadata good;
    good.id = AssetId("a-good");
    good.relative_path = "figure_001.png";
    good.media_type = "image/png";
    session.assets().registry().Register(good);

    SnapshotFactory factory(&session.assets());
    const auto snapshot = factory.CreateBuildSnapshot(session.state(), std::string{});
    // 恶意资源被完全丢弃……
    PF_CHECK(snapshot.asset_files.count("a-evil") == 0);
    // ……且每个保留下来的源文件都位于 assets 目录内。
    const std::string assets_root = std::filesystem::weakly_canonical(session.paths().assets_dir).string();
    for (const auto& [name, source] : snapshot.asset_sources) {
        const std::string resolved = std::filesystem::weakly_canonical(source).string();
        PF_CHECK(resolved.compare(0, assets_root.size(), assets_root) == 0);
    }
    PF_CHECK(snapshot.asset_files.count("a-good") == 1);
    std::filesystem::remove_all(dir);
}

// Save 必须拒绝通过遍历路径写入 references.bib；项目目录不得多出
// 同级文件，持久化状态也不得声称项目已完全持久化。
PF_TEST(SaveRefusesBibliographyPathEscape) {
    auto dir = TempDir("pf-boundary-save");
    NullCompiler compiler;
    ProjectSession::Config config;
    config.compiler_factory = [&compiler]() -> std::unique_ptr<ICompiler> { return std::make_unique<NullCompiler>(); };
    config.debounce = std::chrono::milliseconds{0};
    static pf::test::ScopedTempDir workspace("pf-boundary-workspaces2");
    config.workspace_root = workspace.path();
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));

    // 强行把遍历路径写入设置，模拟反序列化校验器出现之前恶意 project.paper
    // 可能造成的情况，并给 session 设置非空参考文献，以触发 side-car 写入。
    session.state().mutable_settings().bibliography_path = "../../escaped.bib";
    session.ForceBibliographyForTest("% escaped\n");

    session.Save();
    session.FlushSaves();

    auto escaped = std::filesystem::temp_directory_path() / "escaped.bib";
    PF_CHECK(!std::filesystem::exists(escaped));
    // 不安全的 side-car 被跳过，因此项目不得报告为 Clean。
    PF_CHECK(session.persistence_state() != PersistenceState::Clean);
    std::filesystem::remove(escaped);
    std::filesystem::remove_all(dir);
}
