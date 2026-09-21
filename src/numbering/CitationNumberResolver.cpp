#include "numbering/CitationNumberResolver.h"

#include "document/DocumentTraversal.h"

namespace pf {

CitationNumberResolver CitationNumberResolver::Build(
    const Document& document, const BibliographyDatabase& bibliography) {
    CitationNumberResolver resolver;
    // 对 bibliography 已知内容的快照，使 resolver 保持为自包含的值
    // （GUI 会把同一个共享实例交给每一行）。
    for (const auto& key : bibliography.Keys()) {
        resolver.known_keys_.insert(key);
    }

    // 编号由首次引用顺序决定。VisitInlineContent 遍历的正是渲染器所输出的
    // inline run（标题、摘要，然后按文档顺序的每个 block），因此这里算出的
    // 编号就是 \bibliographystyle{citation-order} 在 PDF 中产生的编号。
    VisitInlineContent(document, [&](const InlineContent& content,
                                     const NodeAddress&) {
        for (const auto& node : content) {
            const auto* citation = std::get_if<Citation>(&node);
            if (!citation) continue;
            for (const auto& key : citation->keys) {
                // 未知 key 渲染为 [?]，且从不占用编号。
                if (!resolver.known_keys_.count(key)) continue;
                if (resolver.numbers_.count(key)) continue;
                resolver.numbers_[key] =
                    static_cast<int>(resolver.numbers_.size()) + 1;
            }
        }
    });
    return resolver;
}

std::optional<CitationDisplayInfo> CitationNumberResolver::Find(
    const std::string& key) const {
    const auto it = numbers_.find(key);
    if (it != numbers_.end()) {
        return CitationDisplayInfo{it->second, true,
                                   "[" + std::to_string(it->second) + "]"};
    }
    if (!known_keys_.count(key)) {
        // 被引用但不在 bibliography 中：LaTeX 同样显示 [?]，
        // 且不占用编号。
        return CitationDisplayInfo{0, false, "[?]"};
    }
    // 已知但文档尚未引用的 key——未分配编号。
    return std::nullopt;
}

bool CitationNumberResolver::IsKnown(const std::string& key) const {
    return known_keys_.count(key) != 0;
}

std::string CitationNumberResolver::FormatPill(
    const std::vector<std::string>& keys) const {
    // 每个 key 收集一个 token：其编号，无法解析时为 "?"。
    std::vector<int> numbers;
    bool has_unknown = false;
    for (const auto& key : keys) {
        const auto it = numbers_.find(key);
        if (it == numbers_.end()) {
            has_unknown = true;
            continue;
        }
        if (std::find(numbers.begin(), numbers.end(), it->second) ==
            numbers.end()) {
            numbers.push_back(it->second);
        }
    }
    std::sort(numbers.begin(), numbers.end());

    // 将三个及以上的连续串压缩为 "a-c"（natbib 的 sort&compress 写法）；
    // 两个则保留为 "a, b"。
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < numbers.size()) {
        size_t j = i;
        while (j + 1 < numbers.size() && numbers[j + 1] == numbers[j] + 1) {
            ++j;
        }
        if (j - i >= 2) {
            parts.push_back(std::to_string(numbers[i]) + "-" +
                            std::to_string(numbers[j]));
        } else {
            for (size_t k = i; k <= j; ++k) {
                parts.push_back(std::to_string(numbers[k]));
            }
        }
        i = j + 1;
    }
    if (has_unknown) parts.push_back("?");

    std::string out = "[";
    for (size_t p = 0; p < parts.size(); ++p) {
        if (p) out += ", ";
        out += parts[p];
    }
    out += "]";
    return out;
}

}  // namespace pf
