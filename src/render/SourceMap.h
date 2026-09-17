#pragma once
// SourceMap: generated-source line -> semantic node mapping
// (architecture section 21). Node-level granularity for V1.
//
// Build Diagnostics plan §9-§11: each mapping also records which *kind* of
// block produced the range, so a compiler line can be turned into a Problem
// that names its target ("Figure", "Text", ...) and double-clicks can focus
// the right GUI block.

#include <cstdint>
#include <optional>
#include <string>
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

// What one generated line belongs to: the semantic node and a display label
// for the GUI.
struct SourceMapEntry {
    NodeId node;
    std::string label;  // block kind: "Text", "Figure", "Table", ...
};

class SourceMap {
public:
    void AddMapping(const GeneratedSourceRange& range, NodeId node,
                    std::string label = {});
    void AddMapping(std::uint32_t line, NodeId node, std::string label = {});

    // Resolve a 1-based line in the generated source to a semantic node.
    std::optional<NodeId> Resolve(std::uint32_t line) const;
    // Full entry (node + block-kind label) for the same lookup.
    std::optional<SourceMapEntry> ResolveEntry(std::uint32_t line) const;

    bool Empty() const noexcept { return line_to_node_.empty(); }
    size_t Size() const noexcept { return line_to_node_.size(); }
    void Clear() { line_to_node_.clear(); }

private:
    struct Entry {
        std::uint32_t line = 0;
        SourceMapEntry value;
    };
    // line -> entry (later mappings win for overlapping ranges)
    std::vector<Entry> line_to_node_;
};

}  // namespace pf
