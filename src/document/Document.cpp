#include "document/Document.h"

namespace pf {

const char* ToString(NodeKind kind) {
    switch (kind) {
        case NodeKind::Section: return "Section";
        case NodeKind::Subsection: return "Subsection";
        case NodeKind::Paragraph: return "Paragraph";
        case NodeKind::Figure: return "Figure";
        case NodeKind::Table: return "Table";
        case NodeKind::DisplayEquation: return "DisplayEquation";
    }
    return "Unknown";
}

namespace {

}  // namespace

bool Document::ContainsNode(const NodeId& id) const {
    return GetNodeKind(id).has_value();
}

std::optional<NodeKind> Document::GetNodeKind(const NodeId& id) const {
    for (const auto& section : body_.sections) {
        if (section.id == id) return NodeKind::Section;
        for (const auto& block : section.blocks) {
            if (std::visit([&](const auto& b) { return b.id == id; }, block)) {
                return std::visit(
                    [](const auto& b) -> NodeKind {
                        using T = std::decay_t<decltype(b)>;
                        if constexpr (std::is_same_v<T, Paragraph>) return NodeKind::Paragraph;
                        if constexpr (std::is_same_v<T, Figure>) return NodeKind::Figure;
                        if constexpr (std::is_same_v<T, Table>) return NodeKind::Table;
                        return NodeKind::DisplayEquation;
                    },
                    block);
            }
        }
        for (const auto& sub : section.subsections) {
            if (sub.id == id) return NodeKind::Subsection;
            for (const auto& block : sub.blocks) {
                if (std::visit([&](const auto& b) { return b.id == id; }, block)) {
                    return std::visit(
                        [](const auto& b) -> NodeKind {
                            using T = std::decay_t<decltype(b)>;
                            if constexpr (std::is_same_v<T, Paragraph>) return NodeKind::Paragraph;
                            if constexpr (std::is_same_v<T, Figure>) return NodeKind::Figure;
                            if constexpr (std::is_same_v<T, Table>) return NodeKind::Table;
                            return NodeKind::DisplayEquation;
                        },
                        block);
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<NodeId> Document::CollectNodeIds() const {
    std::vector<NodeId> out;
    for (const auto& section : body_.sections) {
        out.push_back(section.id);
        for (const auto& block : section.blocks) {
            out.push_back(std::visit([](const auto& b) { return b.id; }, block));
        }
        for (const auto& sub : section.subsections) {
            out.push_back(sub.id);
            for (const auto& block : sub.blocks) {
                out.push_back(std::visit([](const auto& b) { return b.id; }, block));
            }
        }
    }
    return out;
}

}  // namespace pf
