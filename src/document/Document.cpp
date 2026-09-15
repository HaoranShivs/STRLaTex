#include "document/Document.h"

#include "document/DocumentTraversal.h"

namespace pf {

const char* ToString(NodeKind kind) {
    switch (kind) {
        case NodeKind::Section: return "Section";
        case NodeKind::Subsection: return "Subsection";
        case NodeKind::Subsubsection: return "Subsubsection";
        case NodeKind::Paragraph: return "Paragraph";
        case NodeKind::Figure: return "Figure";
        case NodeKind::Table: return "Table";
        case NodeKind::DisplayEquation: return "DisplayEquation";
    }
    return "Unknown";
}

bool Document::ContainsNode(const NodeId& id) const {
    return GetNodeKind(id).has_value();
}

std::optional<NodeKind> Document::GetNodeKind(const NodeId& id) const {
    auto address = LocateNode(*this, id);
    if (!address) return std::nullopt;
    return address->kind;
}

std::vector<NodeId> Document::CollectNodeIds() const {
    return CollectAllNodeIds(*this);
}

}  // namespace pf
