#include "bibliography/BibliographyService.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace pf {

// ---------------- BibTeX 解析器 ----------------

namespace {

std::string Trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(begin, end - begin);
}

std::string ToLower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<std::string> SplitBibAuthors(const std::string& authors) {
    std::vector<std::string> result;
    size_t start = 0;
    int depth = 0;
    auto append = [&](size_t end) {
        auto author = Trim(authors.substr(start, end - start));
        if (!author.empty() && ToLower(author) != "others")
            result.push_back(std::move(author));
    };
    for (size_t i = 0; i < authors.size(); ++i) {
        if (authors[i] == '\\' && i + 1 < authors.size()) {
            ++i;
        } else if (authors[i] == '{') {
            ++depth;
        } else if (authors[i] == '}') {
            --depth;
        } else if (depth == 0 && i + 3 <= authors.size() && ToLower(authors.substr(i, 3)) == "and" &&
                   (i == 0 || std::isspace(static_cast<unsigned char>(authors[i - 1]))) &&
                   (i + 3 == authors.size() || std::isspace(static_cast<unsigned char>(authors[i + 3])))) {
            append(i);
            start = i + 3;
            i += 2;
        }
    }
    append(authors.size());
    return result;
}

// ---------------- 数据库 / 服务 ----------------

void BibliographyDatabase::Clear() {
    entries_.clear();
    key_index_.clear();
}

void BibliographyDatabase::AddEntry(BibEntry entry) {
    auto it = key_index_.find(entry.key);
    if (it != key_index_.end()) {
        entries_[it->second] = std::move(entry);
        return;
    }
    key_index_[entry.key] = entries_.size();
    entries_.push_back(std::move(entry));
}

const BibEntry* BibliographyDatabase::Find(const std::string& key) const {
    auto it = key_index_.find(key);
    return it == key_index_.end() ? nullptr : &entries_[it->second];
}

std::vector<std::string> BibliographyDatabase::Keys() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& e : entries_)
        keys.push_back(e.key);
    return keys;
}

BibliographyImportResult BibliographyService::ImportFile(const std::string& path) {
    BibliographyImportResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.status = BibliographyImportResult::Status::FileMissing;
        result.detail = "cannot open " + path;
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ImportText(ss.str());
}

BibliographyImportResult BibliographyService::ImportText(const std::string& bibtex_text) {
    BibliographyImportResult result;
    auto parsed = ParseBibTeX(bibtex_text);
    auto& entries = parsed.entries;
    if (!parsed.error.empty() || entries.empty() ||
        std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.key.empty(); })) {
        result.status = BibliographyImportResult::Status::ParseError;
        result.detail = parsed.error.empty() ? "no valid entries found, or a citation key is missing" : parsed.error;
        return result;
    }
    // *在本文件内*重复的键属于数据问题：保留最后一条定义（BibTeX 自身的
    // 行为），但要报告每一个重复键，以便导入方发出警告（引用方案 §9）。
    // 覆盖已有键的重新导入属于更新而非重复，保持静默。
    std::vector<std::string> seen;
    for (const auto& e : entries) {
        if (std::find(seen.begin(), seen.end(), e.key) == seen.end()) {
            seen.push_back(e.key);
            continue;
        }
        if (std::find(result.duplicate_keys.begin(), result.duplicate_keys.end(), e.key) ==
            result.duplicate_keys.end()) {
            result.duplicate_keys.push_back(e.key);
        }
    }
    for (auto& e : entries) {
        db_.AddEntry(std::move(e));
    }
    ++revision_;
    result.status = BibliographyImportResult::Status::Ok;
    result.entry_count = entries.size();
    result.bibliography_revision = revision_;
    return result;
}

CitationSearchResult BibliographyService::Search(const CitationSearchRequest& request) const {
    CitationSearchResult result;
    std::string q = ToLower(Trim(request.query));
    for (const auto& e : db_.Entries()) {
        if (q.empty()) {
            result.entries.push_back(e);
            continue;
        }
        std::string hay = ToLower(e.key + " " + e.title + " " + e.year + " " + e.venue);
        for (const auto& a : e.authors)
            hay += " " + ToLower(a);
        if (hay.find(q) != std::string::npos) {
            result.entries.push_back(e);
        }
    }
    return result;
}

} // namespace pf
