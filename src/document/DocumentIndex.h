#pragma once
// DocumentIndex: NodeId -> logical location lookup (architecture section 九).
// Stores logical positions (no pointers), rebuilt after structural edits.

#include <optional>
#include <unordered_map>
#include <vector>

#include "document/Document.h"

namespace pf {

struct NodeLocation {
    NodeId parent;              // owning section/subsection/subsubsection id
                                // (empty for top-level sections)
    NodeKind kind;
    size_t section_index = 0;
    bool in_subsection = false;
    size_t subsection_index = 0;
    bool in_subsubsection = false;
    size_t subsubsection_index = 0;
    size_t block_index = 0;     // valid when kind is a Block kind
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
