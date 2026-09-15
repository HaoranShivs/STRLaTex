#include "document/DocumentIndex.h"

#include "document/DocumentTraversal.h"

namespace pf {

void DocumentIndex::Rebuild(const Document& document) {
    index_.clear();
    // One traversal produces every (node -> logical location) pair, so adding
    // a heading level does not touch this file.
    VisitNodes(document, [this, &document](const NodeAddress& address) {
        NodeLocation loc;
        loc.kind = address.kind;
        if (address.section) loc.section_index = *address.section;
        if (address.subsection) {
            loc.in_subsection = true;
            loc.subsection_index = *address.subsection;
        }
        if (address.subsubsection) {
            loc.in_subsubsection = true;
            loc.subsubsection_index = *address.subsubsection;
        }
        if (address.block) loc.block_index = *address.block;
        if (!address.is_heading()) {
            // Owning container id: the section, the subsection, or the
            // subsubsection the node lives in. A subsubsection heading itself
            // is owned by its subsection.
            const auto& sections = document.body().sections;
            const Section& section = sections[loc.section_index];
            if (!address.subsection) {
                loc.parent = section.id;
            } else if (address.kind == NodeKind::Subsubsection) {
                loc.parent = section.subsections[loc.subsection_index].id;
            } else if (!address.subsubsection) {
                loc.parent = section.subsections[loc.subsection_index].id;
            } else {
                loc.parent =
                    section.subsections[loc.subsection_index]
                        .subsubsections[loc.subsubsection_index]
                        .id;
            }
        } else if (address.kind == NodeKind::Subsubsection) {
            const auto& sections = document.body().sections;
            loc.parent = sections[loc.section_index].subsections[loc.subsection_index].id;
        }
        index_[address.node] = loc;
    });
}

std::optional<NodeLocation> DocumentIndex::Find(const NodeId& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

}  // namespace pf
