#include "render/SourceMap.h"

#include <algorithm>

namespace pf {

void SourceMap::AddMapping(const GeneratedSourceRange& range, NodeId node, std::string label) {
    for (std::uint32_t line = range.begin_line; line <= range.end_line; ++line) {
        AddMapping(line, node, std::string(label));
    }
}

void SourceMap::AddMapping(std::uint32_t line, NodeId node, std::string label) {
    line_to_node_.push_back(Entry{line, SourceMapEntry{std::move(node), std::move(label)}});
}

std::optional<SourceMapEntry> SourceMap::ResolveEntry(std::uint32_t line) const {
    // 仅使用实际生成范围内的映射，避免把后置内容的错误归给前一个正文节点。
    const Entry* best = nullptr;
    for (const auto& entry : line_to_node_) {
        if (entry.line == line) {
            best = &entry;
        }
    }
    if (best)
        return best->value;
    return std::nullopt;
}

std::optional<NodeId> SourceMap::Resolve(std::uint32_t line) const {
    if (auto entry = ResolveEntry(line); entry && !entry->node.empty())
        return entry->node;
    return std::nullopt;
}

} // namespace pf
