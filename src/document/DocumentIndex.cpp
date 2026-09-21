#include "document/DocumentIndex.h"

#include "document/DocumentTraversal.h"

namespace pf {

void DocumentIndex::Rebuild(const Document& document) {
    index_.clear();
    // 一次遍历即可产生全部（node -> 逻辑位置）对，因此新增一级标题
    // 无需改动本文件。
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
            // 所属容器 id：该 node 所在的 section、subsection 或
            // subsubsection。subsubsection 标题本身
            // 归其 subsection 所有。
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
