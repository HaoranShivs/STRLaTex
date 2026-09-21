#pragma once
// SourceMap：生成后源码的行 -> 语义节点映射（架构 21）。V1 采用节点级粒度。
//
// Build Diagnostics 方案 §9-§11：每条映射还记录产生该范围的块*类型*，
// 从而可把编译器给出的一行转成指名其目标的 Problem（"Figure"、"Text"……），
// 双击即可聚焦到 GUI 中正确的块。

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
    std::uint32_t begin_line = 0;  // 从 1 开始，含端点
    std::uint32_t end_line = 0;    // 含端点
};

// 一条生成行归属的对象：语义节点，以及供 GUI 显示的标签。
struct SourceMapEntry {
    NodeId node;
    std::string label;  // 块类型："Text"、"Figure"、"Table"……
};

class SourceMap {
public:
    void AddMapping(const GeneratedSourceRange& range, NodeId node,
                    std::string label = {});
    void AddMapping(std::uint32_t line, NodeId node, std::string label = {});

    // 把生成源码中从 1 开始计数的行解析为语义节点。
    std::optional<NodeId> Resolve(std::uint32_t line) const;
    // 同一查找返回的完整条目（节点 + 块类型标签）。
    std::optional<SourceMapEntry> ResolveEntry(std::uint32_t line) const;

    bool Empty() const noexcept { return line_to_node_.empty(); }
    size_t Size() const noexcept { return line_to_node_.size(); }
    void Clear() { line_to_node_.clear(); }

private:
    struct Entry {
        std::uint32_t line = 0;
        SourceMapEntry value;
    };
    // 行 -> 条目（范围重叠时后写入的映射生效）
    std::vector<Entry> line_to_node_;
};

}  // namespace pf
