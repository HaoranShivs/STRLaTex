// Bibliography 测试：BibTeX 解析、搜索。
#include "TestMain.hpp"
#include "ScopedTempDir.hpp"

#include "bibliography/BibliographyService.h"
#include "project/ProjectSession.h"

#include <fstream>
#include <sstream>

using namespace pf;

namespace {

const char* kSampleBib = R"(@article{einstein1905,
  author = {Albert Einstein},
  title = {Ist die Tr{\"a}gheit eines K{\"o}rpers von seinem Energieinhalt abh{\"a}ngig?},
  journal = {Annalen der Physik},
  year = {1905}
}

@inproceedings{knuth1984,
  author = {Donald E. Knuth and Michael F. Plass},
  title = {Breaking Paragraphs into Lines},
  booktitle = {Software Practice and Applications},
  year = {1984}
}

@book{lamport1994,
  author = {Leslie Lamport},
  title = {LaTeX: A Document Preparation System},
  year = {1994}
}
)";

} // namespace

PF_TEST(BibTeXParseEntries) {
    BibliographyDatabase db;
    BibliographyService service(db);
    auto result = service.ImportText(kSampleBib);
    PF_CHECK(result.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(result.entry_count == 3);
    PF_CHECK(db.Entries().size() == 3);

    const BibEntry* e = db.Find("einstein1905");
    PF_CHECK(e != nullptr);
    PF_CHECK(e->year == "1905");
    PF_CHECK(e->authors.size() == 1);
    PF_CHECK(e->authors[0] == "Albert Einstein");
    PF_CHECK(e->venue == "Annalen der Physik");

    const BibEntry* k = db.Find("knuth1984");
    PF_CHECK(k != nullptr);
    PF_CHECK(k->authors.size() == 2);
    PF_CHECK(k->authors[0] == "Donald E. Knuth");
    PF_CHECK(k->authors[1] == "Michael F. Plass");
    PF_CHECK(k->venue == "Software Practice and Applications");
}

PF_TEST(BibTeXAuthorAndSeparation) {
    auto authors = SplitBibAuthors("A. One and B. Two and C. Three");
    PF_CHECK(authors.size() == 3);
    PF_CHECK(authors[0] == "A. One");
    PF_CHECK(authors[2] == "C. Three");

    auto single = SplitBibAuthors("Solo Author");
    PF_CHECK(single.size() == 1);
    PF_CHECK(single[0] == "Solo Author");
}

PF_TEST(BibliographySearch) {
    BibliographyDatabase db;
    BibliographyService service(db);
    service.ImportText(kSampleBib);

    auto all = service.Search(CitationSearchRequest{""});
    PF_CHECK(all.entries.size() == 3);

    auto hit = service.Search(CitationSearchRequest{"einstein"});
    PF_CHECK(hit.entries.size() == 1);
    PF_CHECK(hit.entries[0].key == "einstein1905");

    auto by_title = service.Search(CitationSearchRequest{"paragraphs"});
    PF_CHECK(by_title.entries.size() == 1);
    PF_CHECK(by_title.entries[0].key == "knuth1984");

    auto miss = service.Search(CitationSearchRequest{"quantum chromodynamics"});
    PF_CHECK(miss.entries.empty());
}

PF_TEST(BibliographyImportInvalid) {
    BibliographyDatabase db;
    BibliographyService service(db);
    auto bad = service.ImportText("this is not bibtex at all");
    PF_CHECK(bad.status == BibliographyImportResult::Status::ParseError);
}

PF_TEST(BibliographyReimportUpdates) {
    BibliographyDatabase db;
    BibliographyService service(db);
    service.ImportText(kSampleBib);
    // 相同 key、不同 title：应替换，而不是重复添加。
    std::string updated = "@article{einstein1905, author={Albert Einstein}, title={Updated Title}, year={1905}}";
    auto r = service.ImportText(updated);
    PF_CHECK(r.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(db.Entries().size() == 3);
    PF_CHECK(db.Find("einstein1905")->title == "Updated Title");
}

PF_TEST(BibTeXEntryFormParsesScholarFieldsAndPreservesBraces) {
    const auto parsed = ParseBibTeX(R"bib(@ARTICLE{smith2026,
      title = {A {NASA} study, with {nested {case}}},
      AUTHOR = "Smith, Jane and Armand Brown and {Research and Development Group}",
      journal = {A Journal, Part B},
      year = 2026,
      pages = {12--34},
      url = {https://example.test/paper?id=1&lang=en},
      doi = "10.1234/test",
      abstract = {An extra field, with punctuation.}
    })bib");
    PF_CHECK(parsed.error.empty());
    PF_CHECK(parsed.entries.size() == 1);
    if (parsed.entries.size() != 1)
        return;
    const auto& entry = parsed.entries.front();
    PF_CHECK(entry.entry_type == "article");
    PF_CHECK(entry.title == "A {NASA} study, with {nested {case}}");
    PF_CHECK(entry.authors == std::vector<std::string>({"Smith, Jane", "Armand Brown", "{Research and Development Group}"}));
    PF_CHECK(entry.venue == "A Journal, Part B");
    PF_CHECK(entry.year == "2026");
    PF_CHECK(entry.fields.at("abstract") == "An extra field, with punctuation.");
    PF_CHECK(entry.fields.at("doi") == "10.1234/test");
    const auto serialized = SerializeBibEntry(entry);
    PF_CHECK(serialized.has_value());
    if (serialized) {
        const auto roundtrip = ParseBibTeX(*serialized);
        PF_CHECK(roundtrip.error.empty() && roundtrip.entries.size() == 1);
        if (roundtrip.entries.size() == 1)
            PF_CHECK(roundtrip.entries.front().fields == entry.fields);
    }
}

PF_TEST(BibTeXEntryFormKeepsMissingFieldsEmptyAndSupportsMetadata) {
    const auto incomplete = ParseBibTeX("@article{draft, title = {Only a title}}");
    PF_CHECK(incomplete.error.empty() && incomplete.entries.size() == 1);
    if (incomplete.entries.size() != 1)
        return;
    PF_CHECK(incomplete.entries[0].authors.empty());
    PF_CHECK(incomplete.entries[0].year.empty());
    PF_CHECK(incomplete.entries[0].venue.empty());
    PF_CHECK(incomplete.entries[0].fields.count("author") == 0);

    const auto parsed = ParseBibTeX(R"bib(
      % @article{ignored, title={Comment}}
      @string{venue = "Annual Conference"}
      @comment{Nested comment {containing @article{fake, title={Ignore}}}}
      @preamble{"Ignored preamble"}
      @inproceedings(key2026,
        title = "A {NASA} study, with \"quotes\"",
        author = {王小明 and Müller, Anna},
        booktitle = venue # " on Testing",
        year = 2026
      )
    )bib");
    PF_CHECK(parsed.error.empty() && parsed.entries.size() == 1);
    if (parsed.entries.size() != 1)
        return;
    PF_CHECK(parsed.entries[0].key == "key2026");
    PF_CHECK(parsed.entries[0].venue == "Annual Conference on Testing");
    PF_CHECK(parsed.entries[0].authors == std::vector<std::string>({"王小明", "Müller, Anna"}));
    const auto chinese_key = ParseBibTeX("@article{王2026, title={中文条目}, author={王小明}}");
    PF_CHECK(chinese_key.error.empty() && chinese_key.entries.size() == 1);
    if (chinese_key.entries.size() == 1) {
        PF_CHECK(chinese_key.entries[0].key == "王2026");
        PF_CHECK(SerializeBibEntry(chinese_key.entries[0]).has_value());
    }
}

PF_TEST(BibTeXMalformedInputDoesNotPartiallyImport) {
    BibliographyDatabase db;
    BibliographyService service(db);
    PF_CHECK(service.ImportText("@article{old, title={Keep me}}").status == BibliographyImportResult::Status::Ok);
    for (const auto& text : {"@article{new, title={Unclosed}",
                             "@article{new, title=\"Unclosed}",
                             "@article{new, title={Title} year=2026}",
                             "@article{new, title={Valid}}\n@article{bad, title={Unclosed}",
                             "@article{,title={Missing key}}"}) {
        PF_CHECK(service.ImportText(text).status == BibliographyImportResult::Status::ParseError);
        PF_CHECK(db.Entries().size() == 1);
        PF_CHECK(db.Find("old") && db.Find("old")->title == "Keep me");
    }
}

PF_TEST(BibTeXEntrySerializationRejectsInvalidEdits) {
    BibEntry entry;
    entry.entry_type = "article";
    entry.key = "edited2026";
    for (const auto& value : {"Unclosed {brace", "Extra } brace", "Done}", "Trailing escape\\"}) {
        entry.fields["title"] = value;
        std::string error;
        PF_CHECK(!SerializeBibEntry(entry, &error));
        PF_CHECK(!error.empty());
    }
    entry.fields["title"] = R"(Valid \{literal braces\} and {NASA})";
    PF_CHECK(SerializeBibEntry(entry).has_value());
    entry.key = "invalid,key";
    PF_CHECK(!SerializeBibEntry(entry));
    entry.key = "edited2026";
    entry.fields["Title"] = "Duplicate field";
    PF_CHECK(!SerializeBibEntry(entry));
}

namespace {

std::string ReadBibliography(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class ReferenceCompiler final : public ICompiler {
  public:
    CompileResult Compile(const CompileRequest&, const std::atomic<bool>*) override {
        CompileResult result;
        result.status = CompileStatus::Success;
        return result;
    }
};

ProjectSession::Config ReferenceConfig(const std::filesystem::path& directory) {
    ProjectSession::Config config;
    config.workspace_root = directory / "workspaces";
    config.compiler_factory = []() { return std::make_unique<ReferenceCompiler>(); };
    return config;
}

BibEntry DraftReference(const std::string& key) {
    BibEntry entry;
    entry.key = key;
    entry.entry_type = "article";
    entry.fields = {{"title", "New {NASA} paper"}, {"author", "Jane Smith and Armand Brown"}, {"year", "2026"},
                     {"journal", "Test Journal"}, {"doi", "10.1234/new"}};
    return entry;
}

} // namespace

PF_TEST(BibliographyAddOnePreservesExistingSourceAndRejectsDuplicateKeys) {
    test::ScopedTempDir directory("pf-reference-add");
    const std::string original = "% Keep this comment and formatting\n@article{old, title={Existing paper}}\n";
    {
        ProjectSession session(ReferenceConfig(directory));
        PF_CHECK(session.NewProject(directory));
        PF_CHECK(session.ImportBibliography(original).status == BibliographyImportResult::Status::Ok);
        const auto added = session.AddBibliographyEntry(DraftReference("new2026"));
        PF_CHECK(added.status == BibliographyImportResult::Status::Ok);
        PF_CHECK(added.entry_count == 1);
        PF_CHECK(added.bibliography_revision == 2);
        const auto source = ReadBibliography(directory / "references.bib");
        PF_CHECK(source.starts_with(original));
        PF_CHECK(session.bibliography().Entries().size() == 2);
        PF_CHECK(session.bibliography().Find("new2026")->fields.at("doi") == "10.1234/new");
        const auto revision = session.current_revision();
        PF_CHECK(session.AddBibliographyEntry(DraftReference("old")).status == BibliographyImportResult::Status::DuplicateKey);
        PF_CHECK(session.current_revision() == revision);
        PF_CHECK(ReadBibliography(directory / "references.bib") == source);
        session.Save();
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    }
    ProjectSession reopened(ReferenceConfig(directory));
    PF_CHECK(reopened.OpenProject(directory, nullptr));
    PF_CHECK(reopened.bibliography().Find("old") != nullptr);
    PF_CHECK(reopened.bibliography().Find("new2026") != nullptr);
}

PF_TEST(BibliographyAddOneUsesConfiguredFileAndKeepsDraftOnWriteFailure) {
    test::ScopedTempDir directory("pf-reference-custom-file");
    ProjectSession session(ReferenceConfig(directory));
    PF_CHECK(session.NewProject(directory));
    session.state().mutable_settings().bibliography_path = "bib/custom.bib";
    PF_CHECK(session.AddBibliographyEntry(DraftReference("first")).status == BibliographyImportResult::Status::Ok);
    const auto path = directory / "bib/custom.bib";
    PF_CHECK(std::filesystem::exists(path));
    PF_CHECK(!std::filesystem::exists(directory / "references.bib"));
    const auto source = ReadBibliography(path);
    const auto revision = session.current_revision();
    // A nonempty directory at the temp-file path forces a real atomic-write failure.
    std::filesystem::create_directory(path.string() + ".tmp");
    std::ofstream(path.string() + ".tmp/keep") << "block replacement";
    const auto failure = session.AddBibliographyEntry(DraftReference("second"));
    PF_CHECK(failure.status != BibliographyImportResult::Status::Ok);
    PF_CHECK(session.current_revision() == revision);
    PF_CHECK(session.bibliography().Find("second") == nullptr);
    PF_CHECK(ReadBibliography(path) == source);
    session.state().mutable_settings().bibliography_path = "../escaped.bib";
    PF_CHECK(session.AddBibliographyEntry(DraftReference("escape")).status != BibliographyImportResult::Status::Ok);
    PF_CHECK(session.bibliography().Find("escape") == nullptr);
}
