#pragma once
// DocumentIndex：NodeId -> 逻辑位置查找（架构 九）。
// 只存储逻辑位置（不使用指针），在结构变更后重建。

#include <optional>
#include <unordered_map>
#include <vector>

#include "document/Document.h"

namespace pf {

struct NodeLocation {
    NodeId parent;              // 所属的 section/subsection/subsubsection id
                                //（顶层 section 为空）
    NodeKind kind;
    size_t section_index = 0;
    bool in_subsection = false;
    size_t subsection_index = 0;
    bool in_subsubsection = false;
    size_t subsubsection_index = 0;
    size_t block_index = 0;     // 当 kind 为某种 Block 类型时有效
};

class DocumentIndex {
public:
    void Rebuild(const Document& document);

    std::optional<NodeLocation> Find(const NodeId& id) const;
    bool Contains(const NodeId& id) const { return index_.count(id) > 0; }
    size_t Size() const noexcept { return index_.size(); }
    void Clear() { index_.clear(); }

private:
    std::unordered_map<NodeId, NodeLocation> index_;
};

}  // namespace pf
