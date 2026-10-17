#include "document/InlineTokenCodec.h"

#include <algorithm>

namespace pf {

namespace {

constexpr char kParentheticalPrefix = 'p';
constexpr char kNarrativePrefix = 'n';
constexpr char kPrefixSeparator = '|';

std::string Trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// 把逗号分隔的 key 列表规范化：trim、丢弃空项、去重（保持首次出现顺序）。
std::vector<std::string> NormalizeKeys(const std::string& list) {
    std::vector<std::string> keys;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        std::string key = Trim(list.substr(start, end - start));
        if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(std::move(key));
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return keys;
}

} // namespace

std::string EncodeCitationToken(const Citation& citation) {
    std::vector<std::string> keys;
    for (const auto& key : citation.keys) {
        const std::string trimmed = Trim(key);
        if (!trimmed.empty() && std::find(keys.begin(), keys.end(), trimmed) == keys.end()) {
            keys.push_back(trimmed);
        }
    }
    if (keys.empty())
        return {};
    std::string payload;
    payload += citation.mode == CitationMode::Narrative ? kNarrativePrefix : kParentheticalPrefix;
    payload += kPrefixSeparator;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i)
            payload += ',';
        payload += keys[i];
    }
    return payload;
}

std::optional<Citation> DecodeCitationToken(const std::string& payload) {
    Citation citation;
    std::string key_list = payload;
    // 只在第一个 '|' 之前识别已知前缀。未知前缀说明这是旧 key（或未来扩展），
    // 整体仍按旧格式解析，绝不让旧 token 丢失。
    const size_t separator = payload.find(kPrefixSeparator);
    if (separator != std::string::npos) {
        if (separator == 1) {
            if (payload[0] == kNarrativePrefix) {
                citation.mode = CitationMode::Narrative;
                key_list = payload.substr(separator + 1);
            } else if (payload[0] == kParentheticalPrefix) {
                citation.mode = CitationMode::Parenthetical;
                key_list = payload.substr(separator + 1);
            }
        }
    }
    citation.keys = NormalizeKeys(key_list);
    if (citation.keys.empty())
        return std::nullopt;
    return citation;
}

} // namespace pf
