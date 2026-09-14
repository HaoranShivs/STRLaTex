// Bibliography tests: BibTeX parsing, search.
#include "TestMain.hpp"

#include "bibliography/BibliographyService.h"

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

}  // namespace

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
    // Same key, different title: should replace, not duplicate.
    std::string updated =
        "@article{einstein1905, author={Albert Einstein}, title={Updated Title}, year={1905}}";
    auto r = service.ImportText(updated);
    PF_CHECK(r.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(db.Entries().size() == 3);
    PF_CHECK(db.Find("einstein1905")->title == "Updated Title");
}
