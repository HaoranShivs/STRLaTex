// P0-04 regression tests: project-relative path trust boundary at the I/O
// points. The deserialization-time validation lives in
// TestDeserializationSafety.cpp; this file covers the second checkpoint -
// resolution immediately before a real filesystem access, including the
// symlink-escape case the review called out.
#include "TestMain.hpp"

#include <fstream>
#include <string>

#include "core/ProjectPath.h"
#include "project/SnapshotFactory.h"
#include "project/ProjectSession.h"

using namespace pf;

namespace {

std::filesystem::path TempDir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

class NullCompiler final : public ICompiler {
public:
    CompileResult Compile(const CompileRequest&,
                          const std::atomic<bool>*) override {
        CompileResult result;
        result.status = CompileStatus::Success;
        return result;
    }
};

} // namespace

PF_TEST(ResolveRejectsTraversalThatEscapesRoot) {
    auto root = TempDir("pf-boundary-root");
    std::filesystem::create_directories(root / "assets");

    // A sibling directory whose name shares the root's prefix must not be
    // reachable: containment is component-wise, not a string prefix test.
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

// A symlink inside the project that points outside must not let a relative
// path read the target: weakly_canonical resolves the link, the component
// walk then sees the escape.
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
    std::filesystem::create_directory_symlink(outside, root / "assets" / "link",
                                              ec);
    if (ec) {
        // Filesystem does not support symlinks here: skip explicitly rather
        // than reporting a pass for a check that never ran.
        std::cout << "  [SKIP] symlinks unsupported: " << ec.message() << "\n";
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        return;
    }

    auto escaped = ResolveUntrustedProjectPath(root, "assets/link/secret.txt");
    PF_CHECK(!escaped.ok());
    if (!escaped.ok())
        PF_CHECK(escaped.error() == PathError::OutsideProjectRoot);

    // The link itself resolves outside the root and must be rejected too.
    auto link_itself = ResolveUntrustedProjectPath(root, "assets/link");
    PF_CHECK(!link_itself.ok());
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

// The compiler snapshot must not hand the build an asset source outside the
// project, even when the registry holds a hostile relative path.
PF_TEST(BuildSnapshotDropsAssetsThatEscapeTheProject) {
    auto dir = TempDir("pf-boundary-snapshot");
    NullCompiler compiler;
    ProjectSession::Config config;
    config.compiler_factory = [&compiler]() -> std::unique_ptr<ICompiler> {
        return std::make_unique<NullCompiler>();
    };
    config.debounce = std::chrono::milliseconds{0};
    config.workspace_root = std::filesystem::temp_directory_path() /
                            "pf-boundary-workspaces";
    std::filesystem::create_directories(config.workspace_root);
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));

    // Register an asset whose stored path tries to leave the assets dir.
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
    const auto snapshot = factory.CreateBuildSnapshot(
        session.state(), std::string{});
    // The hostile asset is dropped entirely ...
    PF_CHECK(snapshot.asset_files.count("a-evil") == 0);
    // ... and every surviving source stays inside the assets directory.
    const std::string assets_root =
        std::filesystem::weakly_canonical(session.paths().assets_dir).string();
    for (const auto& [name, source] : snapshot.asset_sources) {
        const std::string resolved =
            std::filesystem::weakly_canonical(source).string();
        PF_CHECK(resolved.compare(0, assets_root.size(), assets_root) == 0);
    }
    PF_CHECK(snapshot.asset_files.count("a-good") == 1);
    std::filesystem::remove_all(dir);
}

// Save must refuse to write references.bib through a traversal path; the
// project directory must not gain a sibling file, and the persistence state
// must not claim the project was fully persisted.
PF_TEST(SaveRefusesBibliographyPathEscape) {
    auto dir = TempDir("pf-boundary-save");
    NullCompiler compiler;
    ProjectSession::Config config;
    config.compiler_factory = [&compiler]() -> std::unique_ptr<ICompiler> {
        return std::make_unique<NullCompiler>();
    };
    config.debounce = std::chrono::milliseconds{0};
    config.workspace_root = std::filesystem::temp_directory_path() /
                            "pf-boundary-workspaces2";
    std::filesystem::create_directories(config.workspace_root);
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));

    // Force a traversal path into the settings, as a hostile project.paper
    // could have done before the deserialization validator existed, and give
    // the session a non-empty bibliography so the side-car write is attempted.
    session.state().mutable_settings().bibliography_path = "../../escaped.bib";
    session.ForceBibliographyForTest("% escaped\n");

    session.Save();
    session.FlushSaves();

    auto escaped = std::filesystem::temp_directory_path() / "escaped.bib";
    PF_CHECK(!std::filesystem::exists(escaped));
    // The unsafe side-car is skipped, so the project must not report Clean.
    PF_CHECK(session.persistence_state() != PersistenceState::Clean);
    std::filesystem::remove(escaped);
    std::filesystem::remove_all(dir);
}
