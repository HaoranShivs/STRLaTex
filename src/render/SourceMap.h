#pragma once
// SourceMap: generated-source line -> semantic node mapping
// (architecture section 21). Node-level granularity for V1.

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/StrongId.h"
#include "document/Document.h"

namespace pf {

struct GeneratedSourceRange {
    std::string file;      // "main.tex"
    std::uint32_t begin_line = 0;  // 1-based, inclusive
    std::uint32_t end_line = 0;    // inclusive
};

class SourceMap {
public:
    void AddMapping(const GeneratedSourceRange& range, NodeId node);
    void AddMapping(std::uint32_t line, NodeId node);

    // Resolve a 1-based line in the generated source to a semantic node.
    std::optional<NodeId> Resolve(std::uint32_t line) const;

    bool Empty() const noexcept { return line_to_node_.empty(); }
    size_t Size() const noexcept { return line_to_node_.size(); }
    void Clear() { line_to_node_.clear(); }

private:
    // line -> node (later mappings win for overlapping ranges)
    std::vector<std::pair<std::uint32_t, NodeId>> line_to_node_;
};

}  // namespace pf
