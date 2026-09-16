#include "cli/Commands.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include "project/ProjectSession.h"

namespace pf::cli {

namespace {

ProjectSession::Config DefaultConfig() {
    ProjectSession::Config config;
    config.tectonic_path = PF_TECTONIC;
    config.workspace_root = "/tmp/paperforge-builds";
    config.debounce = std::chrono::milliseconds{0};  // CLI: build immediately
    return config;
}

void PrintDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) {
        std::cout << "  " << d.Summary() << "\n";
    }
}

}  // namespace

int CmdNew(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "usage: paperforge new <project-dir>\n";
        return 2;
    }
    ProjectSession session(DefaultConfig());
    if (!session.NewProject(args[0])) {
        std::cerr << "failed to create project\n";
        return 1;
    }
    // Seed with a starter document via the editing protocol.
    ProjectRevision rev = session.current_revision();
    auto make_cmd = [&](auto payload) {
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = session.state().id();
        cmd.base_revision = session.current_revision();
        cmd.origin = EditOrigin::System;
        cmd.payload = std::move(payload);
        return cmd;
    };

    SetTitlePayload title;
    title.title = InlineFromText("My Research Paper");
    session.Execute(make_cmd(title));

    SetAbstractPayload abstract_payload;
    abstract_payload.abstract_text = InlineFromText(
        "This paper presents a study conducted with PaperForge.");
    session.Execute(make_cmd(abstract_payload));

    InsertSectionPayload section;
    section.index = 0;
    section.title = InlineFromText("Introduction");
    auto intro_result = session.Execute(make_cmd(section));

    InsertSectionPayload section2;
    section2.index = 1;
    section2.title = InlineFromText("Methodology");
    session.Execute(make_cmd(section2));

    if (intro_result.status == EditStatus::Applied) {
        InsertParagraphPayload para;
        para.parent = intro_result.created_node;
        para.content = InlineFromText(
            "This is the first paragraph of the introduction.");
        session.Execute(make_cmd(para));
    }

    // Save is asynchronous (immutable snapshot -> save worker); block until
    // the snapshot has actually reached disk before reporting success.
    session.Save();
    auto save = session.FlushSaves();
    if (save.status != SaveResult::Status::Ok) {
        std::cerr << "save failed: " << save.detail << "\n";
        return 1;
    }
    std::cout << "created project at " << args[0] << "\n";
    return 0;
}

int CmdBuild(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "usage: paperforge build <project-dir>\n";
        return 2;
    }
    ProjectSession session(DefaultConfig());
    std::string error;
    if (!session.OpenProject(args[0], &error)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }

    std::atomic<bool> done{false};
    session.SetBuildResultHandler([&](const BuildResult& result) {
        if (result.outcome == BuildResult::Outcome::Success) {
            std::cout << "build OK (rev " << result.revision.value << ") -> "
                      << result.pdf_path << "\n";
        } else if (result.outcome == BuildResult::Outcome::Failure) {
            std::cout << "build FAILED (rev " << result.revision.value << ")\n";
        } else {
            std::cout << "build outcome: " << static_cast<int>(result.outcome) << "\n";
        }
        if (!result.diagnostics.empty()) {
            std::cout << "diagnostics:\n";
            PrintDiagnostics(result.diagnostics);
        }
        done.store(true);
    });

    session.RequestBuild(true);
    // The build runs on a worker; its result is delivered as an application
    // event, so this loop must pump the application-thread queue.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{50});
    }
    if (!done.load()) {
        std::cerr << "build timed out\n";
        return 1;
    }
    return 0;
}

int CmdInfo(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "usage: paperforge info <project-dir>\n";
        return 2;
    }
    ProjectSession session(DefaultConfig());
    std::string error;
    if (!session.OpenProject(args[0], &error)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }
    const auto& doc = session.state().document();
    std::cout << "project:  " << args[0] << "\n";
    std::cout << "id:       " << session.state().id().value() << "\n";
    std::cout << "revision: " << session.state().revision().value << "\n";
    std::cout << "template: " << session.state().template_selection() << "\n";
    std::cout << "title:    " << InlineToPlainText(doc.front_matter().title) << "\n";
    std::cout << "authors:  " << doc.front_matter().authors.size() << "\n";
    std::cout << "sections: " << doc.body().sections.size() << "\n";
    std::cout << "assets:   " << session.assets().registry().All().size() << "\n";
    std::cout << "bib entries: " << session.bibliography().Entries().size() << "\n";
    return 0;
}

int CmdDemo(const std::vector<std::string>& args) {
    // End-to-end demo: create project, edit, build PDF.
    std::string dir = args.empty() ? "/tmp/paperforge-demo" : args[0];
    std::filesystem::remove_all(dir);
    if (CmdNew({dir}) != 0) return 1;

    ProjectSession session(DefaultConfig());
    std::string error;
    if (!session.OpenProject(dir, &error)) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }

    // Add an equation + citation to the introduction section.
    const auto& doc = session.state().document();
    if (!doc.body().sections.empty()) {
        NodeId intro = doc.body().sections[0].id;
        EditCommand eq_cmd;
        eq_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        eq_cmd.project_id = session.state().id();
        eq_cmd.base_revision = session.current_revision();
        InsertEquationPayload eq;
        eq.parent = intro;
        eq.latex = "E = mc^2";
        eq_cmd.payload = eq;
        session.Execute(eq_cmd);

        // Import bibliography and cite.
        std::string bib = R"(@article{einstein1905,
  author = {Albert Einstein},
  title = {Ist die Tr\"agheit eines K\"orpers von seinem Energieinhalt abh\"angig?},
  journal = {Annalen der Physik},
  year = {1905}
}
)";
        session.ImportBibliography(bib);

        if (!doc.body().sections.empty()) {
            // find paragraph
            for (const auto& block : doc.body().sections[0].blocks) {
                if (const auto* para = std::get_if<Paragraph>(&block)) {
                    EditCommand cit_cmd;
                    cit_cmd.operation_id = OperationId(IdGenerator::NewOperationId());
                    cit_cmd.project_id = session.state().id();
                    cit_cmd.base_revision = session.current_revision();
                    InsertCitationPayload cit;
                    cit.paragraph = para->id;
                    cit.keys = {"einstein1905"};
                    cit_cmd.payload = cit;
                    session.Execute(cit_cmd);
                    break;
                }
            }
        }
    }

    session.Save();
    session.FlushSaves();

    std::atomic<bool> done{false};
    session.SetBuildResultHandler([&](const BuildResult& result) {
        if (result.outcome == BuildResult::Outcome::Success) {
            std::cout << "demo build OK -> " << result.pdf_path << "\n";
        } else {
            std::cout << "demo build failed\n";
            PrintDiagnostics(result.diagnostics);
        }
        done.store(true);
    });
    session.RequestBuild(true);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{180};
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{50});
    }
    return done.load() ? 0 : 1;
}

}  // namespace pf::cli
