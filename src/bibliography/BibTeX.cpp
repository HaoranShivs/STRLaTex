#include "bibliography/BibliographyService.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace pf {
namespace {

std::string Lower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    return first == std::string::npos
               ? std::string()
               : value.substr(first,
                              value.find_last_not_of(" \r\n\t") - first + 1);
}

std::string NormalizeSpace(const std::string& value) {
    std::string result;
    bool space = false;
    for (unsigned char c : value) {
        if (std::isspace(c)) {
            space = !result.empty();
        } else {
            if (space)
                result += ' ';
            result += static_cast<char>(c);
            space = false;
        }
    }
    return result;
}

bool NameChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-';
}

bool ValidName(const std::string& name) {
    return !name.empty() &&
           std::isalpha(static_cast<unsigned char>(name.front())) &&
           std::all_of(name.begin(), name.end(),
                       [](unsigned char c) { return NameChar(c); });
}

bool ValidKey(const std::string& key) {
    return !key.empty() &&
           std::all_of(key.begin(), key.end(), [](unsigned char c) {
               return c >= 0x80 || std::isalnum(c) ||
                      std::string_view("_:.+-/").find(static_cast<char>(c)) !=
                          std::string_view::npos;
           });
}

bool BalancedValue(const std::string& value) {
    int depth = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\') {
            if (++i == value.size())
                return false;
        } else if (value[i] == '{') {
            ++depth;
        } else if (value[i] == '}' && --depth < 0) {
            return false;
        }
    }
    return depth == 0;
}

class Parser {
  public:
    explicit Parser(const std::string& text) : text_(text) {}

    BibTeXParseResult Parse() {
        BibTeXParseResult result;
        while (pos_ < text_.size()) {
            SkipSpace();
            if (pos_ == text_.size())
                break;
            if (text_[pos_] != '@') {
                ++pos_;
                continue;
            }
            ++pos_;
            const std::string type = ReadName();
            SkipSpace();
            if (!ValidName(type) || pos_ == text_.size() ||
                (text_[pos_] != '{' && text_[pos_] != '(')) {
                Fail("Expected an entry type followed by '{' or '('.");
                break;
            }
            const char closer = text_[pos_++] == '{' ? '}' : ')';
            if (type == "comment" || type == "preamble") {
                SkipBody(closer);
            } else if (type == "string") {
                SkipSpace();
                const std::string name = ReadName();
                SkipSpace();
                if (name.empty() || !Take('=')) {
                    Fail("Invalid @string definition.");
                    break;
                }
                macros_[name] = ReadValue(closer);
                SkipSpace();
                Take(',');
                SkipSpace();
                if (!Take(closer))
                    Fail("Unclosed @string definition.");
            } else {
                BibEntry entry;
                entry.entry_type = type;
                SkipSpace();
                const size_t start = pos_;
                while (pos_ < text_.size() && text_[pos_] != ',' &&
                       text_[pos_] != closer)
                    ++pos_;
                entry.key = Trim(text_.substr(start, pos_ - start));
                if (!entry.key.empty() && !ValidKey(entry.key)) {
                    Fail("Invalid citation key. Use letters, numbers, '_', "
                         "':', '.', '+', '-' or '/'.");
                    break;
                }
                if (Take(',')) {
                    ReadFields(closer, entry.fields);
                } else if (!Take(closer)) {
                    Fail("Unclosed BibTeX entry.");
                }
                auto field = [&](const char* name) {
                    const auto it = entry.fields.find(name);
                    return it == entry.fields.end() ? std::string()
                                                    : it->second;
                };
                entry.title = field("title");
                entry.year = field("year");
                entry.authors = SplitBibAuthors(field("author"));
                entry.venue = field("journal");
                if (entry.venue.empty())
                    entry.venue = field("booktitle");
                result.entries.push_back(std::move(entry));
            }
            if (!error_.empty())
                break;
        }
        if (!error_.empty()) {
            result.entries.clear();
            result.error =
                error_ + " (near character " + std::to_string(pos_ + 1) + ")";
        }
        return result;
    }

  private:
    void Fail(const std::string& message) {
        if (error_.empty())
            error_ = message;
    }

    bool Take(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void SkipSpace() {
        while (pos_ < text_.size()) {
            if (std::isspace(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            } else if (text_[pos_] == '%') {
                while (pos_ < text_.size() && text_[pos_] != '\n')
                    ++pos_;
            } else {
                break;
            }
        }
    }

    std::string ReadName() {
        const size_t start = pos_;
        while (pos_ < text_.size() &&
               NameChar(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
        return Lower(text_.substr(start, pos_ - start));
    }

    void SkipBody(char closer) {
        int depth = 0;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '\\' && pos_ < text_.size()) {
                ++pos_;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && depth > 0) {
                --depth;
            } else if (c == closer && depth == 0) {
                return;
            }
        }
        Fail("Unclosed BibTeX comment or preamble.");
    }

    std::string ReadPart(char closer) {
        SkipSpace();
        if (pos_ == text_.size()) {
            Fail("Missing field value.");
            return {};
        }
        const char opener = text_[pos_];
        if (opener == '{' || opener == '"') {
            const size_t start = ++pos_;
            int depth = 0;
            while (pos_ < text_.size()) {
                const char c = text_[pos_++];
                if (c == '\\' && pos_ < text_.size()) {
                    ++pos_;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    if (depth > 0) {
                        --depth;
                    } else if (opener == '{') {
                        return text_.substr(start, pos_ - start - 1);
                    } else {
                        Fail("Unbalanced braces in quoted field.");
                        return {};
                    }
                } else if (c == '"' && opener == '"' && depth == 0) {
                    return text_.substr(start, pos_ - start - 1);
                }
            }
            Fail("Unclosed field value.");
            return {};
        }
        const size_t start = pos_;
        while (pos_ < text_.size() &&
               !std::isspace(static_cast<unsigned char>(text_[pos_])) &&
               text_[pos_] != ',' && text_[pos_] != closer &&
               text_[pos_] != '#' && text_[pos_] != '%')
            ++pos_;
        if (start == pos_)
            Fail("Missing field value.");
        const std::string value = text_.substr(start, pos_ - start);
        const auto macro = macros_.find(Lower(value));
        return macro == macros_.end() ? value : macro->second;
    }

    std::string ReadValue(char closer) {
        std::string value = ReadPart(closer);
        SkipSpace();
        while (Take('#') && error_.empty()) {
            value += ReadPart(closer);
            SkipSpace();
        }
        return NormalizeSpace(value);
    }

    void ReadFields(char closer, std::map<std::string, std::string>& fields) {
        while (error_.empty()) {
            SkipSpace();
            if (Take(closer))
                return;
            const std::string name = ReadName();
            SkipSpace();
            if (!ValidName(name) || !Take('=')) {
                Fail("Expected a field name followed by '='.");
                return;
            }
            fields[name] = ReadValue(closer);
            if (Take(closer))
                return;
            if (!Take(',')) {
                Fail("Expected ',' or the end of the entry after a field "
                     "value.");
                return;
            }
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
    std::map<std::string, std::string> macros_;
};

} // namespace

BibTeXParseResult ParseBibTeX(const std::string& text) {
    return Parser(text).Parse();
}

std::optional<std::string> SerializeBibEntry(const BibEntry& entry,
                                             std::string* error) {
    auto fail = [&](const std::string& detail) -> std::optional<std::string> {
        if (error)
            *error = detail;
        return std::nullopt;
    };
    if (error)
        error->clear();
    if (!ValidKey(entry.key))
        return fail("Enter a citation key using letters, numbers, '_', ':', "
                    "'.', '+', '-' or '/'.");
    const std::string type = Lower(entry.entry_type);
    if (!ValidName(type) || type == "comment" || type == "string" ||
        type == "preamble")
        return fail(
            "Enter a reference type such as article, inproceedings or book.");
    std::map<std::string, std::string> fields;
    for (const auto& [name, value] : entry.fields) {
        if (!ValidName(name) || !fields.emplace(Lower(name), value).second)
            return fail("Invalid or duplicate field name: " + name);
    }
    auto fallback = [&](const char* name, const std::string& value) {
        if (!value.empty() && fields.find(name) == fields.end())
            fields[name] = value;
    };
    fallback("title", entry.title);
    fallback("year", entry.year);
    std::string authors;
    for (const auto& author : entry.authors) {
        if (!authors.empty())
            authors += " and ";
        authors += author;
    }
    fallback("author", authors);
    fallback(type == "article" ? "journal" : "booktitle", entry.venue);
    std::string text = "@" + type + "{" + entry.key + ",\n";
    for (const auto& [name, value] : fields) {
        if (!BalancedValue(value))
            return fail("Unbalanced braces or unfinished escape in field '" +
                        name + "'.");
        if (!Trim(value).empty())
            text += "  " + Lower(name) + " = {" + Trim(value) + "},\n";
    }
    text += "}\n";
    const auto parsed = ParseBibTeX(text);
    if (!parsed.error.empty() || parsed.entries.size() != 1)
        return fail(
            "A field contains unbalanced braces or an unfinished escape. " +
            parsed.error);
    return text;
}

} // namespace pf
