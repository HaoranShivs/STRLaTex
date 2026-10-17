#include "build/TemplateVerifier.h"
#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/InlineText.h"
#include <fstream>
#include <sstream>

namespace pf {
Result<VerifiedTemplatePackage> VerifyTemplateArchive(
    const std::filesystem::path& archive, const std::filesystem::path& runtime,
    const std::filesystem::path& workspace, const std::atomic<bool>* cancelled,
    std::function<void(const std::string&)> progress) {
    auto package = InspectTemplateArchive(archive);
    if (!package)
        return Unexpected(package.error());
    VerifiedTemplatePackage verified;
    verified.package = std::move(package.value());
    Document doc;
    DocumentEditor editor(doc);
    editor.SetTitle(
        InlineFromText("Template verification: Structure & Science"));
    auto aff = editor.AddAffiliation("PaperForge Verification Institute");
    Author author;
    author.name = "Alice Example";
    author.email = "alice@example.org";
    author.affiliations = {aff.value()};
    editor.AddAuthor(author);
    editor.SetAbstract(InlineFromText(
        "An abstract used to verify the publisher front matter."));
    editor.SetKeywords({"verification", "templates"});
    auto section = editor.InsertSection(0, InlineFromText("Introduction"));
    Paragraph p;
    p.content = InlineFromText("First citation ");
    Citation cite;
    cite.keys = {"z-first"};
    p.content.push_back(cite);
    p.content.push_back(TextRun{" and narrative ", 0});
    cite.keys = {"a-second"};
    cite.mode = CitationMode::Narrative;
    p.content.push_back(cite);
    p.content.push_back(TextRun{". Bold and italic text.",
                                TextMark::Strong | TextMark::Emphasis});
    editor.InsertBlock(section.value(), std::nullopt, p);
    EquationBlock eq;
    eq.expression.latex = "E=mc^2";
    eq.numbered = true;
    editor.InsertBlock(section.value(), std::nullopt, eq);
    for (const auto& def : verified.package.profiles) {
        if (cancelled && cancelled->load())
            return Unexpected("Template import cancelled");
        if (progress)
            progress("Verifying " + def.name + "…");
        RenderRequest request;
        request.document = &doc;
        request.template_id = def.id;
        request.resolved_template =
            std::make_shared<const TemplateDefinition>(def);
        request.bibliography_bibtex =
            "@article{z-first,author={Zed, Zoe},title={First verification "
            "reference},journal={Test Journal},year={2025}}\n"
            "@article{a-second,author={Alpha, Alice},title={Second "
            "verification reference},journal={Test Journal},year={2026}}\n";
        auto rendered = LatexRenderer().Render(request);
        if (rendered.status != RenderResult::Status::Ok)
            return Unexpected("Cannot plan template " + def.name + ": " +
                              (rendered.diagnostics.empty()
                                   ? "unknown error"
                                   : rendered.diagnostics.front().message));
        CompileRequest compile;
        compile.package = std::move(rendered.package);
        compile.toolchain = compile.package.toolchain;
        compile.workspace =
            workspace / IdGenerator::NewSnapshotId() / def.binding.profile;
        CompilerConfig config;
        config.texlive_root = runtime;
        auto result = TexLiveCompiler(config).Compile(compile, cancelled);
        if (result.status != CompileStatus::Success)
            return Unexpected("Template verification failed for " + def.name +
                              ".\n" + result.log);
        // A PDF alone could be produced by the wrong class. Check recorder
        // evidence.
        std::ifstream recorder(compile.workspace / "main.fls");
        std::string line;
        bool loaded = false;
        while (std::getline(recorder, line)) {
            if (!line.starts_with("INPUT "))
                continue;
            auto input = std::filesystem::path(line.substr(6));
            if (input.string().starts_with("/work/"))
                input = compile.workspace / input.lexically_relative("/work");
            std::error_code ec;
            const auto actual = std::filesystem::weakly_canonical(
                input.is_absolute() ? input : compile.workspace / input, ec);
            const auto expected = std::filesystem::weakly_canonical(
                compile.workspace / (def.document_class + ".cls"), ec);
            if (!ec && actual == expected)
                loaded = true;
        }
        if (!loaded)
            return Unexpected(
                "Template probe did not load the imported class: " +
                def.document_class);
        std::ifstream bbl(compile.workspace / "main.bbl");
        std::ostringstream bibliography;
        bibliography << bbl.rdbuf();
        const auto first = bibliography.str().find("{z-first}");
        const auto second = bibliography.str().find("{a-second}");
        if (first == std::string::npos || second == std::string::npos ||
            first >= second)
            return Unexpected(
                "Template citation numbering disagrees with the editor");
        verified.probes.push_back({def.id, compile.package.plan_digest,
                                   result.pdf_path, std::move(result.log)});
    }
    return verified;
}
} // namespace pf
