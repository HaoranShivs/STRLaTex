#pragma once
// CitationNumberResolver (citation plan §3): the single owner of the
// citation key -> display number mapping.
//
// The document stores citation *keys* only - never a rendered "[1]" - and
// LaTeX/BibTeX decide the final numbering. This resolver implements the same
// policy in the GUI so a pill and the PDF can never disagree:
//
//   * citation-order numeric (MVP policy for every template): a key is
//     numbered by its first citation in document reading order. The Generic
//     Article template therefore renders with `unsrtnat` and IEEE keeps
//     `IEEEtran`, both of which number in citation order - so scanning the
//     document once reproduces the final [1] [2] ... of the PDF exactly.
//   * citing the same key again reuses its number (never allocates a new one),
//   * a multi-citation sorts and compresses its numbers ("[1, 3]", "[1-3]"),
//   * a key that is not in the bibliography resolves to "?" and consumes no
//     number (an undefined \cite does not shift real entries in LaTeX either).
//
// If author-year templates (APA, ...) are added later, the strategy moves
// into TemplateDefinition and this class gains a policy parameter.

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bibliography/BibliographyService.h"
#include "document/Document.h"

namespace pf {

struct CitationDisplayInfo {
    int number = 0;        // 1-based citation-order number; 0 when unresolved
    bool resolved = false; // false = key missing from the bibliography -> [?]
    std::string display;   // compact pill text for a single key, e.g. "[2]"
};

class CitationNumberResolver {
public:
    // Walks `document` in reading order and numbers every key that exists in
    // `bibliography`. Keys cited nowhere get no entry at all.
    static CitationNumberResolver Build(const Document& document,
                                        const BibliographyDatabase& bibliography);

    // Numbering for a single key. nullopt when the bibliography knows the key
    // but the document has not cited it yet; a cited-but-unknown key comes
    // back unresolved with display "[?]" (never silently invisible).
    std::optional<CitationDisplayInfo> Find(const std::string& key) const;

    // True when the bibliography contains `key` (cited yet or not).
    bool IsKnown(const std::string& key) const;

    // Pill text for a one- or multi-key citation: keys deduplicated, numbers
    // sorted ascending, consecutive runs of three or more compressed to
    // "a-c" (what natbib's sort&compress produces), unresolved keys shown as
    // "?". Example outputs: "[1]", "[1, 3]", "[1-3]", "[2, ?]".
    std::string FormatPill(const std::vector<std::string>& keys) const;

    bool empty() const noexcept { return numbers_.empty(); }

    const std::map<std::string, int>& numbers() const noexcept {
        return numbers_;
    }

private:
    // key -> 1-based number, in citation order of first appearance. Owns the
    // complete answer: no back-pointer into the bibliography or document, so
    // the GUI can keep the shared instance without any lifetime coupling.
    std::map<std::string, int> numbers_;
    std::set<std::string> known_keys_;
};

}  // namespace pf
