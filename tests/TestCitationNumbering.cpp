// Citation feature plan regression matrix (domain + numbering + validation +
// persistence layers; GUI-level coverage lives in paperforge-inline-editor-test):
//
//   * CitationNumberResolver: first-citation order, stable numbers on repeat
//     citations, renumbering after the first citation is removed, unknown key
//     -> [?], multi-citation sorting/compression.
//   * Validator: E-CITATION-UNKNOWN-KEY, E-CITATION-NO-BIBLIOGRAPHY.
//   * Template/rendering: GUI numbering policy matches the LaTeX style
//     (generic-article -> unsrtnat, ieee -> IEEEtran), LaTeX stays semantic
//     (\citep{key}, never a hardcoded [1]).
//   * Bibliography: duplicate keys surfaced on import; references.bib is a
//     project resource that survives save/reload with the citation keys.
#include "TestMain.hpp"
#include "ScopedTempDir.hpp"

#include <chrono>
#include <filesystem>

#include "bibliography/BibliographyService.h"
#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "editing/EditCommand.h"
#include "numbering/CitationNumberResolver.h"
#include "project/ProjectSession.h"
#include "render/LatexRenderer.h"
#include "template/TemplateRegistry.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

ProjectSession::Config TestConfig() {
    ProjectSession::Config config;
    config.tectonic_path = PF_TECTONIC_BIN;
    // E-08: unique per process (see TestEndToEnd.cpp for the shared-path bug).
    static pf::test::ScopedTempDir workspace("pf-cite-workspaces");
    config.workspace_root = workspace.path();
    config.debounce = std::chrono::milliseconds{0};
    return config;
}

// A document whose paragraphs cite `chains` in order (one paragraph per
// chain, each chain one multi-key citation).
Document MakeCitingDoc(const std::vector<std::vector<std::string>>& chains,
                       std::vector<NodeId>* paragraphs = nullptr) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    for (const auto& chain : chains) {
        Paragraph p;
        p.content.push_back(TextRun{"Claim from ", 0});
        Citation cit;
        cit.keys = chain;
        p.content.push_back(std::move(cit));
        editor.InsertBlock(s.value(), std::nullopt, p);
        if (paragraphs) paragraphs->push_back(p.id);
    }
    return doc;
}

BibliographyDatabase MakeDb(const std::vector<std::string>& keys) {
    std::string bib;
    for (const auto& key : keys) {
        bib += "@article{" + key + ", author={A. Author}, title={T}, year={2020}}\n";
    }
    BibliographyDatabase db;
    BibliographyService service(db);
    service.ImportText(bib);
    return db;
}

int CountDiagnostics(const ValidationResult& r, const char* code) {
    int n = 0;
    for (const auto& d : r.diagnostics) {
        if (d.code == code) ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Numbering policy
// ---------------------------------------------------------------------------

PF_TEST(CitationNumberingFollowsFirstCitationOrder) {
    auto db = MakeDb({"a", "b", "c"});
    // Cite order: A, then B, then A again, then C.
    auto doc = MakeCitingDoc({{"a"}, {"b"}, {"a"}, {"c"}});
    auto resolver = CitationNumberResolver::Build(doc, db);

    PF_CHECK(resolver.Find("a")->number == 1);
    PF_CHECK(resolver.Find("b")->number == 2);
    PF_CHECK(resolver.Find("c")->number == 3);
    // Re-citing A keeps its number: "[1]" in both places.
    PF_CHECK(resolver.Find("a")->display == "[1]");
    PF_CHECK(resolver.Find("a")->resolved);
}

PF_TEST(CitationNumberingRenumbersAfterCitationRemoved) {
    auto db = MakeDb({"a", "b"});
    {
        auto doc = MakeCitingDoc({{"a"}, {"b"}});
        auto resolver = CitationNumberResolver::Build(doc, db);
        PF_CHECK(resolver.Find("a")->number == 1);
        PF_CHECK(resolver.Find("b")->number == 2);
    }
    // Delete *all* citations of A: B becomes [1] (citation-order re-flow).
    auto doc = MakeCitingDoc({{"b"}});
    auto resolver = CitationNumberResolver::Build(doc, db);
    PF_CHECK(resolver.Find("b")->number == 1);
    PF_CHECK(!resolver.Find("a"));
}

PF_TEST(CitationPillSortsDedupesAndCompresses) {
    auto db = MakeDb({"a", "b", "c", "d"});
    auto doc = MakeCitingDoc({{"a"}, {"b"}, {"c"}, {"d"}, {"d", "a", "b"}});
    auto resolver = CitationNumberResolver::Build(doc, db);

    // Multi-citation is sorted by number regardless of key order, and the
    // [1,2,3] run compresses like natbib sort&compress.
    PF_CHECK(resolver.FormatPill({"b", "a"}) == "[1, 2]");
    PF_CHECK(resolver.FormatPill({"c", "a", "b"}) == "[1-3]");
    PF_CHECK(resolver.FormatPill({"d", "a", "b"}) == "[1, 2, 4]");
    // Duplicate keys inside one citation collapse to one number.
    PF_CHECK(resolver.FormatPill({"a", "a"}) == "[1]");
}

PF_TEST(CitationUnknownKeyShowsQuestionMarkWithoutConsumingNumber) {
    auto db = MakeDb({"a", "b"});
    auto doc = MakeCitingDoc({{"ghost"}, {"a"}, {"a", "ghost"}});
    auto resolver = CitationNumberResolver::Build(doc, db);

    // Unknown key: [?], and it must not shift the real numbering.
    PF_CHECK(resolver.Find("ghost")->display == "[?]");
    PF_CHECK(!resolver.Find("ghost")->resolved);
    PF_CHECK(resolver.Find("a")->number == 1);
    PF_CHECK(resolver.Find("b") == std::nullopt);  // never cited, but known
    PF_CHECK(resolver.FormatPill({"a", "ghost"}) == "[1, ?]");
}

// ---------------------------------------------------------------------------
// Validation (before every build; citation plan §9)
// ---------------------------------------------------------------------------

PF_TEST(ValidatorReportsUnknownCitationKey) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    Citation cit;
    cit.keys = {"ghost2024"};
    p.content.push_back(std::move(cit));
    editor.InsertBlock(s.value(), std::nullopt, p);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.has_bibliography = true;
    input.bibliography_keys = {"real2024"};
    auto result = validator.Validate(input);
    PF_CHECK(CountDiagnostics(result, "E-CITATION-UNKNOWN-KEY") == 1);
}

PF_TEST(ValidatorReportsCitationsWithoutBibliography) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    Citation cit;
    cit.keys = {"anything"};
    p.content.push_back(std::move(cit));
    Citation cit2;
    cit2.keys = {"other"};
    p.content.push_back(std::move(cit2));
    editor.InsertBlock(s.value(), std::nullopt, p);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.has_bibliography = false;  // empty bibliography
    auto result = validator.Validate(input);
    // One readable project-level error, never one noise error per key.
    PF_CHECK(CountDiagnostics(result, "E-CITATION-NO-BIBLIOGRAPHY") == 1);
    PF_CHECK(CountDiagnostics(result, "E-CITATION-UNKNOWN-KEY") == 0);
}

// ---------------------------------------------------------------------------
// LaTeX layer: semantic only, numbering policy aligned
// ---------------------------------------------------------------------------

PF_TEST(GenericArticleUsesCitationOrderBibliographyStyle) {
    const auto* def = TemplateRegistry::Instance().Find("generic-article");
    PF_CHECK(def != nullptr);
    if (def) PF_CHECK(def->bibliography_style == "unsrtnat");
    // IEEE keeps IEEEtran - also citation-order numeric.
    const auto* ieee = TemplateRegistry::Instance().Find("ieee-conference");
    if (ieee) PF_CHECK(ieee->bibliography_style == "IEEEtran");
}

PF_TEST(RendererEmitsSemanticCitesNeverNumbers) {
    auto doc = MakeCitingDoc({{"a"}, {"a", "b"}});
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    req.bibliography_bibtex =
        "@article{a, title={A}}\n@article{b, title={B}}";
    auto result = renderer.Render(req);
    std::string tex;
    for (const auto& f : result.package.files) {
        if (f.path == "main.tex") tex = f.content;
    }
    PF_CHECK(tex.find("\\citep{a}") != std::string::npos);
    PF_CHECK(tex.find("\\citep{a,b}") != std::string::npos);
    // The renderer must not hardcode display numbers...
    PF_CHECK(tex.find("[1]") == std::string::npos);
    // ...and the style must agree with the GUI's citation-order numbering.
    PF_CHECK(tex.find("\\bibliographystyle{unsrtnat}") != std::string::npos);
    PF_CHECK(tex.find("natbib") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Bibliography lifecycle
// ---------------------------------------------------------------------------

PF_TEST(BibliographyDuplicateKeysSurfacedOnImport) {
    BibliographyDatabase db;
    BibliographyService service(db);
    auto r = service.ImportText(
        "@article{x, title={First}}\n"
        "@article{y, title={Unique}}\n"
        "@article{x, title={Second}}\n");
    PF_CHECK(r.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(r.duplicate_keys.size() == 1);
    if (!r.duplicate_keys.empty()) {
        PF_CHECK(r.duplicate_keys[0] == "x");
    }
    // Last definition wins (BibTeX behaviour), no silent doubling.
    PF_CHECK(db.Entries().size() == 2);
    PF_CHECK(db.Find("x")->title == "Second");
}

PF_TEST(BibliographyImportedAtomicallyAndReloaded) {
    auto dir = std::filesystem::temp_directory_path() / "pf-citation-bib";
    std::filesystem::remove_all(dir);

    {
        ProjectSession session(TestConfig());
        PF_CHECK(session.NewProject(dir));

        // Citation plan §7: a *successful* import persists references.bib
        // immediately - not only at save time.
        auto ok = session.ImportBibliography(
            "@article{smith2024, author={J. Smith}, title={Seminal}, year={2024}}\n");
        PF_CHECK(ok.status == BibliographyImportResult::Status::Ok);
        PF_CHECK(std::filesystem::exists(dir / "references.bib"));
        PF_CHECK(session.state().settings().bibliography_path ==
                 "references.bib");
        // No .tmp litter from the atomic rename.
        PF_CHECK(!std::filesystem::exists(dir / "references.bib.tmp"));

        // A failed import must not clobber the stored bibliography.
        auto bad = session.ImportBibliography("not bibtex at all");
        PF_CHECK(bad.status == BibliographyImportResult::Status::ParseError);
        PF_CHECK(std::filesystem::exists(dir / "references.bib"));
        PF_CHECK(session.bibliography().Find("smith2024") != nullptr);

        session.Save();
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    }

    // Reopen: bibliography reloads from the project resource (citation §7).
    ProjectSession reopened(TestConfig());
    std::string error;
    PF_CHECK(reopened.OpenProject(dir, &error));
    PF_CHECK(reopened.bibliography().Find("smith2024") != nullptr);
    std::filesystem::remove_all(dir);
}

PF_TEST(CitationKeysSurviveSaveAndReload) {
    auto dir = std::filesystem::temp_directory_path() / "pf-citation-roundtrip";
    std::filesystem::remove_all(dir);

    NodeId paragraph_id;
    {
        ProjectSession session(TestConfig());
        PF_CHECK(session.NewProject(dir));
        session.ImportBibliography(
            "@article{smith2024, title={S}}\n@article{jones2020, title={J}}\n");

        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = session.state().id();
        cmd.base_revision = session.current_revision();
        InsertSectionPayload sec;
        sec.index = 0;
        sec.title = InlineFromText("Intro");
        cmd.payload = sec;
        auto sec_r = session.Execute(cmd);
        PF_CHECK(sec_r.status == EditStatus::Applied);

        InsertParagraphPayload para;
        para.parent = sec_r.created_node;
        para.content = InlineFromText("This method was introduced by ");
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.base_revision = session.current_revision();
        cmd.payload = para;
        auto para_r = session.Execute(cmd);
        PF_CHECK(para_r.status == EditStatus::Applied);
        paragraph_id = para_r.created_node;

        // The exact content InlineEditor produces after the picker commits.
        Citation cit;
        cit.keys = {"smith2024"};
        InlineContent content;
        content.push_back(TextRun{"This method was introduced by ", 0});
        content.push_back(std::move(cit));
        content.push_back(TextRun{" previously.", 0});
        EditParagraphPayload edit;
        edit.paragraph = paragraph_id;
        edit.content = content;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.base_revision = session.current_revision();
        cmd.payload = edit;
        PF_CHECK(session.Execute(cmd).status == EditStatus::Applied);

        session.Save();
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    }

    ProjectSession reopened(TestConfig());
    std::string error;
    PF_CHECK(reopened.OpenProject(dir, &error));

    const Document& doc = reopened.state().document();
    bool found_citation = false;
    VisitNodes(doc, [&](const NodeAddress& address) {
        if (const auto* block = FindBlock(
                const_cast<Document&>(doc), address.node)) {
            if (const auto* para = std::get_if<Paragraph>(block)) {
                for (const auto& node : para->content) {
                    if (const auto* cit = std::get_if<Citation>(&node)) {
                        found_citation =
                            cit->keys.size() == 1 &&
                            cit->keys[0] == "smith2024";
                    }
                }
            }
        }
    });
    PF_CHECK(found_citation);
    // The numbering recomputes identically after reopen: keys are stored,
    // numbers are derived (citation plan §10).
    auto resolver =
        CitationNumberResolver::Build(doc, reopened.bibliography());
    PF_CHECK(resolver.Find("smith2024")->number == 1);
    std::filesystem::remove_all(dir);
}
