#pragma once

#include <cstdint>
// BibTeX parsing + bibliography service (architecture section 18).

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pf {

struct BibEntry {
    std::string key;
    std::string entry_type;  // article, inproceedings, ...
    std::string title;
    std::vector<std::string> authors;  // parsed from "author" field
    std::string year;
    std::string venue;  // journal / booktitle
    std::map<std::string, std::string> fields;
};

struct CitationSearchRequest {
    std::string query;
};

struct CitationSearchResult {
    std::vector<BibEntry> entries;
};

struct BibliographyImportResult {
    enum class Status { Ok, FileMissing, ParseError };
    Status status = Status::Ok;
    std::string detail;
    size_t entry_count = 0;
    std::uint64_t bibliography_revision = 0;
};

class BibliographyDatabase {
public:
    void Clear();
    void AddEntry(BibEntry entry);
    const BibEntry* Find(const std::string& key) const;
    const std::vector<BibEntry>& Entries() const noexcept { return entries_; }
    std::vector<std::string> Keys() const;

private:
    std::vector<BibEntry> entries_;
    std::map<std::string, size_t> key_index_;
};

class BibliographyService {
public:
    explicit BibliographyService(BibliographyDatabase& db) : db_(db) {}

    // Parse a .bib file into the database.
    BibliographyImportResult ImportFile(const std::string& path);
    BibliographyImportResult ImportText(const std::string& bibtex_text);

    CitationSearchResult Search(const CitationSearchRequest& request) const;

    BibliographyDatabase& db() noexcept { return db_; }

private:
    BibliographyDatabase& db_;
    std::uint64_t revision_ = 0;
};

// Split BibTeX author field into individual author names.
std::vector<std::string> SplitBibAuthors(const std::string& authors);

}  // namespace pf
