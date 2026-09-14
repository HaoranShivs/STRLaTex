#include "document/DocumentIndex.h"

namespace pf {

void DocumentIndex::Rebuild(const Document& document) {
    index_.clear();
    const auto& sections = document.body().sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        const auto& section = sections[si];
        {
            NodeLocation loc;
            loc.kind = NodeKind::Section;
            loc.section_index = si;
            index_[section.id] = loc;
        }
        for (size_t bi = 0; bi < section.blocks.size(); ++bi) {
            const auto& block = section.blocks[bi];
            NodeLocation loc;
            loc.kind = std::visit([](const auto& b) -> NodeKind {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, Paragraph>) return NodeKind::Paragraph;
                if constexpr (std::is_same_v<T, Figure>) return NodeKind::Figure;
                if constexpr (std::is_same_v<T, Table>) return NodeKind::Table;
                return NodeKind::DisplayEquation;
            }, block);
            loc.parent = section.id;
            loc.section_index = si;
            loc.block_index = bi;
            index_[std::visit([](const auto& b) { return b.id; }, block)] = loc;
        }
        for (size_t ui = 0; ui < section.subsections.size(); ++ui) {
            const auto& sub = section.subsections[ui];
            {
                NodeLocation loc;
                loc.kind = NodeKind::Subsection;
                loc.parent = section.id;
                loc.section_index = si;
                loc.in_subsection = true;
                loc.subsection_index = ui;
                index_[sub.id] = loc;
            }
            for (size_t bi = 0; bi < sub.blocks.size(); ++bi) {
                const auto& block = sub.blocks[bi];
                NodeLocation loc;
                loc.kind = std::visit([](const auto& b) -> NodeKind {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, Paragraph>) return NodeKind::Paragraph;
                    if constexpr (std::is_same_v<T, Figure>) return NodeKind::Figure;
                    if constexpr (std::is_same_v<T, Table>) return NodeKind::Table;
                    return NodeKind::DisplayEquation;
                }, block);
                loc.parent = sub.id;
                loc.section_index = si;
                loc.in_subsection = true;
                loc.subsection_index = ui;
                loc.block_index = bi;
                index_[std::visit([](const auto& b) { return b.id; }, block)] = loc;
            }
        }
    }
}

std::optional<NodeLocation> DocumentIndex::Find(const NodeId& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

}  // namespace pf
