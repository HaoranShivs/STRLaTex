#include "numbering/CitationNumberResolver.h"

#include "document/DocumentTraversal.h"

namespace pf {

CitationNumberResolver CitationNumberResolver::Build(
    const Document& document, const BibliographyDatabase& bibliography) {
    CitationNumberResolver resolver;
    // Snapshot of what the bibliography knows, so the resolver stays a
    // self-contained value (the GUI hands one shared instance to every row).
    for (const auto& key : bibliography.Keys()) {
        resolver.known_keys_.insert(key);
    }

    // First-citation order defines the numbering. VisitInlineContent walks
    // exactly the inline runs the renderer emits (title, abstract, then every
    // block in document order), so the numbers computed here are the numbers
    // \bibliographystyle{citation-order} produces in the PDF.
    VisitInlineContent(document, [&](const InlineContent& content,
                                     const NodeAddress&) {
        for (const auto& node : content) {
            const auto* citation = std::get_if<Citation>(&node);
            if (!citation) continue;
            for (const auto& key : citation->keys) {
                // Unknown keys render [?] and never consume a number.
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
        // Cited but not in the bibliography: LaTeX shows [?] for it too, and
        // it consumes no number.
        return CitationDisplayInfo{0, false, "[?]"};
    }
    // Known key the document has not cited yet - no number assigned.
    return std::nullopt;
}

bool CitationNumberResolver::IsKnown(const std::string& key) const {
    return known_keys_.count(key) != 0;
}

std::string CitationNumberResolver::FormatPill(
    const std::vector<std::string>& keys) const {
    // Collect one token per key: its number, or "?" when unresolvable.
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

    // Compress consecutive runs of three or more into "a-c" (natbib
    // sort&compress spelling); pairs stay "a, b".
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
