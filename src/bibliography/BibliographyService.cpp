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
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 去除字段值外层成对的花括号/引号。
std::string StripBraces(std::string value) {
    value = Trim(std::move(value));
    while (value.size() >= 2 &&
           ((value.front() == '{' && value.back() == '}') ||
            (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2);
        value = Trim(std::move(value));
    }
    // 合并内部空白与换行。
    std::string out;
    bool in_space = false;
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            in_space = true;
        } else {
            if (in_space && !out.empty()) out += ' ';
            in_space = false;
            out += c;
        }
    }
    return out;
}

class BibParser {
public:
    explicit BibParser(const std::string& text) : text_(text) {}

    std::vector<BibEntry> Parse(bool* ok) {
        std::vector<BibEntry> entries;
        size_t pos = 0;
        bool any_error = false;
        while (pos < text_.size()) {
            // 查找下一个 '@'
            size_t at = text_.find('@', pos);
            if (at == std::string::npos) break;
            size_t open = text_.find_first_of("{(", at);
            if (open == std::string::npos || open > at + 64) {
                pos = at + 1;
                continue;
            }
            BibEntry entry;
            entry.entry_type = ToLower(Trim(text_.substr(at + 1, open - at - 1)));
            // 读取 key，直到逗号
            size_t key_end = text_.find(',', open);
            if (key_end == std::string::npos) {
                any_error = true;
                break;
            }
            entry.key = Trim(text_.substr(open + 1, key_end - open - 1));
            // 读取字段，直到匹配的右花括号
            int depth = 1;
            size_t i = key_end + 1;
            char closer = (text_[open] == '{') ? '}' : ')';
            while (i < text_.size() && depth > 0) {
                char c = text_[i];
                if (c == '{') ++depth;
                else if (c == '}') --depth;
                else if (c == closer && depth == 1) --depth;
                ++i;
            }
            std::string body = text_.substr(key_end + 1, i - key_end - 2);
            ParseFields(body, &entry);
            if (!entry.key.empty() && !entry.entry_type.empty()) {
                entries.push_back(std::move(entry));
            }
            pos = i;
        }
        if (ok) *ok = !any_error;
        return entries;
    }

private:
    void ParseFields(const std::string& body, BibEntry* entry) {
        size_t i = 0;
        while (i < body.size()) {
            // 跳过分隔符
            while (i < body.size() && (body[i] == ',' || std::isspace(static_cast<unsigned char>(body[i])))) ++i;
            if (i >= body.size()) break;
            size_t eq = body.find('=', i);
            if (eq == std::string::npos) break;
            std::string name = ToLower(Trim(body.substr(i, eq - i)));
            i = eq + 1;
            // 值：{..}、"..." 或裸值
            std::string value;
            if (i < body.size() && body[i] == '{') {
                int depth = 1;
                size_t start = ++i;
                while (i < body.size() && depth > 0) {
                    if (body[i] == '{') ++depth;
                    else if (body[i] == '}') --depth;
                    if (depth > 0) ++i;
                }
                value = body.substr(start, i - start);
                ++i;
            } else if (i < body.size() && body[i] == '"') {
                size_t start = ++i;
                while (i < body.size() && body[i] != '"') {
                    if (body[i] == '\\' && i + 1 < body.size()) ++i;
                    ++i;
                }
                value = body.substr(start, i - start);
                ++i;
            } else {
                size_t start = i;
                while (i < body.size() && body[i] != ',') ++i;
                value = Trim(body.substr(start, i - start));
            }
            if (!name.empty()) {
                entry->fields[name] = StripBraces(value);
            }
        }
        // 提取已知字段。
        auto f = [&](const char* n) -> std::string {
            auto it = entry->fields.find(n);
            return it == entry->fields.end() ? std::string() : it->second;
        };
        entry->title = f("title");
        entry->year = f("year");
        entry->venue = f("journal");
        if (entry->venue.empty()) entry->venue = f("booktitle");
        entry->authors = SplitBibAuthors(f("author"));
    }

    const std::string& text_;
};

}  // namespace

std::vector<std::string> SplitBibAuthors(const std::string& authors) {
    std::vector<std::string> out;
    std::string current;
    int brace = 0;
    for (char c : authors) {
        if (c == '{') ++brace;
        else if (c == '}') --brace;
        if (c == ',' && brace == 0) {
            // 同一作者内的 "Last, First" 分隔符：保留，不拆分
            current += c;
        } else if ((c == ' ' || c == '\t') && brace == 0) {
            // 前瞻：" and " 用于分隔作者
            if (current.size() >= 3 && current.substr(current.size() - 3) == "and") {
                out.push_back(Trim(current.substr(0, current.size() - 3)));
                current.clear();
            } else {
                current += c;
            }
        } else {
            current += c;
        }
    }
    if (!Trim(current).empty()) out.push_back(Trim(current));
    // 后处理结尾的 "and"
    for (auto& a : out) {
        a = Trim(a);
        if (a.size() >= 3 && a.substr(a.size() - 3) == "and") {
            a = Trim(a.substr(0, a.size() - 3));
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& s) {
                                 return s.empty() || ToLower(s) == "others";
                             }),
              out.end());
    return out;
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
    for (const auto& e : entries_) keys.push_back(e.key);
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
    bool ok = false;
    auto entries = BibParser(bibtex_text).Parse(&ok);
    if (entries.empty()) {
        result.status = BibliographyImportResult::Status::ParseError;
        result.detail = "no valid entries found";
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
        if (std::find(result.duplicate_keys.begin(),
                      result.duplicate_keys.end(),
                      e.key) == result.duplicate_keys.end()) {
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
        for (const auto& a : e.authors) hay += " " + ToLower(a);
        if (hay.find(q) != std::string::npos) {
            result.entries.push_back(e);
        }
    }
    return result;
}

}  // namespace pf
