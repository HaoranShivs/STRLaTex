#include "render/SourceMap.h"

#include <algorithm>

namespace pf {

void SourceMap::AddMapping(const GeneratedSourceRange& range, NodeId node,
                           std::string label) {
    for (std::uint32_t line = range.begin_line; line <= range.end_line; ++line) {
        AddMapping(line, node, std::string(label));
    }
}

void SourceMap::AddMapping(std::uint32_t line, NodeId node,
                           std::string label) {
    line_to_node_.push_back(
        Entry{line, SourceMapEntry{std::move(node), std::move(label)}});
}

std::optional<SourceMapEntry> SourceMap::ResolveEntry(
    std::uint32_t line) const {
    // 查找位于 `line` 处或之前（节点起始位置最近）的最后一条映射。
    const Entry* best = nullptr;
    for (const auto& entry : line_to_node_) {
        if (entry.line <= line && (!best || entry.line >= best->line)) {
            best = &entry;
        }
    }
    if (best) return best->value;
    return std::nullopt;
}

std::optional<NodeId> SourceMap::Resolve(std::uint32_t line) const {
    if (auto entry = ResolveEntry(line)) return entry->node;
    return std::nullopt;
}

}  // namespace pf
