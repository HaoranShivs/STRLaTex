#include "document/Document.h"

#include "document/DocumentTraversal.h"

namespace pf {

bool Footnote::operator==(const Footnote& other) const {
    if (body == other.body)
        return true;
    if (!body || !other.body)
        return false;
    return body->content == other.body->content;
}

const char* ToString(NodeKind kind) {
    switch (kind) {
    case NodeKind::Section:
        return "Section";
    case NodeKind::Subsection:
        return "Subsection";
    case NodeKind::Subsubsection:
        return "Subsubsection";
    case NodeKind::Paragraph:
        return "Paragraph";
    case NodeKind::Figure:
        return "Figure";
    case NodeKind::Table:
        return "Table";
    case NodeKind::Equation:
        return "Equation";
    case NodeKind::List:
        return "List";
    case NodeKind::Theorem:
        return "Theorem";
    case NodeKind::Code:
        return "Code";
    }
    return "Unknown";
}

const char* ToString(TheoremKind kind) {
    switch (kind) {
    case TheoremKind::Theorem:
        return "Theorem";
    case TheoremKind::Lemma:
        return "Lemma";
    case TheoremKind::Proposition:
        return "Proposition";
    case TheoremKind::Corollary:
        return "Corollary";
    case TheoremKind::Definition:
        return "Definition";
    case TheoremKind::Proof:
        return "Proof";
    case TheoremKind::Remark:
        return "Remark";
    case TheoremKind::Example:
        return "Example";
    }
    return "Theorem";
}

bool Document::ContainsNode(const NodeId& id) const {
    return GetNodeKind(id).has_value();
}

std::optional<NodeKind> Document::GetNodeKind(const NodeId& id) const {
    auto address = LocateNode(*this, id);
    if (!address)
        return std::nullopt;
    return address->kind;
}

std::vector<NodeId> Document::CollectNodeIds() const {
    return CollectAllNodeIds(*this);
}

} // namespace pf
