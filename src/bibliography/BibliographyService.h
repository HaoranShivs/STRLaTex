#pragma once

#include <cstdint>
// BibTeX 解析与参考文献服务（架构 18）。

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pf {

struct BibEntry {
    std::string key;
    std::string entry_type;  // article、inproceedings 等
    std::string title;
    std::vector<std::string> authors;  // 由 "author" 字段解析而来
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
    // 在导入文件内出现多次的键（引用方案 §9）。最后一条定义生效——
    // 与 BibTeX 的行为一致——但导入方必须暴露该冲突，而不是静默合并。
    std::vector<std::string> duplicate_keys;
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

    // 把 .bib 文件解析进数据库。
    BibliographyImportResult ImportFile(const std::string& path);
    BibliographyImportResult ImportText(const std::string& bibtex_text);

    CitationSearchResult Search(const CitationSearchRequest& request) const;

    BibliographyDatabase& db() noexcept { return db_; }

private:
    BibliographyDatabase& db_;
    std::uint64_t revision_ = 0;
};

// 把 BibTeX 的 author 字段拆分为各个作者名。
std::vector<std::string> SplitBibAuthors(const std::string& authors);

}  // namespace pf
