#include "render/SourceMap.h"

#include <algorithm>

namespace pf {

void SourceMap::AddMapping(const GeneratedSourceRange& range, NodeId node) {
    for (std::uint32_t line = range.begin_line; line <= range.end_line; ++line) {
        AddMapping(line, node);
    }
}

void SourceMap::AddMapping(std::uint32_t line, NodeId node) {
    line_to_node_.emplace_back(line, std::move(node));
}

std::optional<NodeId> SourceMap::Resolve(std::uint32_t line) const {
    // Find the last mapping at or before `line` (nearest node start).
    const std::pair<std::uint32_t, NodeId>* best = nullptr;
    for (const auto& entry : line_to_node_) {
        if (entry.first <= line && (!best || entry.first >= best->first)) {
            best = &entry;
        }
    }
    if (best) return best->second;
    return std::nullopt;
}

}  // namespace pf
