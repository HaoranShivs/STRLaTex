#include "document/DocumentTraversal.h"

namespace pf {

namespace {

Body& BodyOf(const Document& document) {
    return DocumentMutableAccess::body(const_cast<Document&>(document));
}

FrontMatter& FrontMatterOf(const Document& document) {
    return DocumentMutableAccess::front_matter(const_cast<Document&>(document));
}

NodeKind BlockKind(const Block& block) {
    return std::visit(
        [](const auto& b) -> NodeKind {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Paragraph>) return NodeKind::Paragraph;
            if constexpr (std::is_same_v<T, Figure>) return NodeKind::Figure;
            if constexpr (std::is_same_v<T, Table>) return NodeKind::Table;
            return NodeKind::Equation;
        },
        block);
}

NodeId BlockId(const Block& block) {
    return std::visit([](const auto& b) { return b.id; }, block);
}

}  // namespace

std::optional<NodeAddress> LocateNode(const Document& document, const NodeId& node) {
    const auto& sections = BodyOf(document).sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        const auto& section = sections[si];

        if (section.id == node) {
            NodeAddress address;
            address.node = node;
            address.kind = NodeKind::Section;
            address.section = si;
            return address;
        }

        for (size_t bi = 0; bi < section.blocks.size(); ++bi) {
            if (BlockId(section.blocks[bi]) == node) {
                NodeAddress address;
                address.node = node;
                address.kind = BlockKind(section.blocks[bi]);
                address.section = si;
                address.block = bi;
                return address;
            }
        }

        for (size_t ui = 0; ui < section.subsections.size(); ++ui) {
            const auto& sub = section.subsections[ui];
            if (sub.id == node) {
                NodeAddress address;
                address.node = node;
                address.kind = NodeKind::Subsection;
                address.section = si;
                address.subsection = ui;
                return address;
            }
            for (size_t bi = 0; bi < sub.blocks.size(); ++bi) {
                if (BlockId(sub.blocks[bi]) == node) {
                    NodeAddress address;
                    address.node = node;
                    address.kind = BlockKind(sub.blocks[bi]);
                    address.section = si;
                    address.subsection = ui;
                    address.block = bi;
                    return address;
                }
            }
            for (size_t zi = 0; zi < sub.subsubsections.size(); ++zi) {
                const auto& subsub = sub.subsubsections[zi];
                if (subsub.id == node) {
                    NodeAddress address;
                    address.node = node;
                    address.kind = NodeKind::Subsubsection;
                    address.section = si;
                    address.subsection = ui;
                    address.subsubsection = zi;
                    return address;
                }
                for (size_t bi = 0; bi < subsub.blocks.size(); ++bi) {
                    if (BlockId(subsub.blocks[bi]) == node) {
                        NodeAddress address;
                        address.node = node;
                        address.kind = BlockKind(subsub.blocks[bi]);
                        address.section = si;
                        address.subsection = ui;
                        address.subsubsection = zi;
                        address.block = bi;
                        return address;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<Block>* FindBlockList(Document& document, const NodeAddress& address) {
    if (!address.section || !address.block) return nullptr;
    auto& sections = BodyOf(document).sections;
    if (*address.section >= sections.size()) return nullptr;
    Section& section = sections[*address.section];
    if (!address.subsection) return &section.blocks;
    if (*address.subsection >= section.subsections.size()) return nullptr;
    Subsection& sub = section.subsections[*address.subsection];
    if (!address.subsubsection) return &sub.blocks;
    if (*address.subsubsection >= sub.subsubsections.size()) return nullptr;
    return &sub.subsubsections[*address.subsubsection].blocks;
}

const std::vector<Block>* FindBlockList(const Document& document,
                                        const NodeAddress& address) {
    return FindBlockList(const_cast<Document&>(document), address);
}

Section* FindSection(Document& document, const NodeId& id) {
    for (auto& section : BodyOf(document).sections) {
        if (section.id == id) return &section;
    }
    return nullptr;
}

Subsection* FindSubsection(Document& document, const NodeId& id) {
    for (auto& section : BodyOf(document).sections) {
        for (auto& sub : section.subsections) {
            if (sub.id == id) return &sub;
        }
    }
    return nullptr;
}

Subsubsection* FindSubsubsection(Document& document, const NodeId& id) {
    for (auto& section : BodyOf(document).sections) {
        for (auto& sub : section.subsections) {
            for (auto& subsub : sub.subsubsections) {
                if (subsub.id == id) return &subsub;
            }
        }
    }
    return nullptr;
}

Block* FindBlock(Document& document, const NodeId& id) {
    auto address = LocateNode(document, id);
    if (!address || !address->block) return nullptr;
    auto* blocks = FindBlockList(document, *address);
    if (!blocks) return nullptr;
    return &(*blocks)[*address->block];
}

void VisitNodes(const Document& document,
                const std::function<void(const NodeAddress&)>& visit) {
    if (!visit) return;
    const auto& sections = BodyOf(document).sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        const auto& section = sections[si];

        {
            NodeAddress address;
            address.node = section.id;
            address.kind = NodeKind::Section;
            address.section = si;
            visit(address);
        }
        for (size_t bi = 0; bi < section.blocks.size(); ++bi) {
            NodeAddress address;
            address.node = BlockId(section.blocks[bi]);
            address.kind = BlockKind(section.blocks[bi]);
            address.section = si;
            address.block = bi;
            visit(address);
        }
        for (size_t ui = 0; ui < section.subsections.size(); ++ui) {
            const auto& sub = section.subsections[ui];
            {
                NodeAddress address;
                address.node = sub.id;
                address.kind = NodeKind::Subsection;
                address.section = si;
                address.subsection = ui;
                visit(address);
            }
            for (size_t bi = 0; bi < sub.blocks.size(); ++bi) {
                NodeAddress address;
                address.node = BlockId(sub.blocks[bi]);
                address.kind = BlockKind(sub.blocks[bi]);
                address.section = si;
                address.subsection = ui;
                address.block = bi;
                visit(address);
            }
            for (size_t zi = 0; zi < sub.subsubsections.size(); ++zi) {
                const auto& subsub = sub.subsubsections[zi];
                {
                    NodeAddress address;
                    address.node = subsub.id;
                    address.kind = NodeKind::Subsubsection;
                    address.section = si;
                    address.subsection = ui;
                    address.subsubsection = zi;
                    visit(address);
                }
                for (size_t bi = 0; bi < subsub.blocks.size(); ++bi) {
                    NodeAddress address;
                    address.node = BlockId(subsub.blocks[bi]);
                    address.kind = BlockKind(subsub.blocks[bi]);
                    address.section = si;
                    address.subsection = ui;
                    address.subsubsection = zi;
                    address.block = bi;
                    visit(address);
                }
            }
        }
    }
}

void VisitBlocks(const Document& document,
                 const std::function<void(const Block&, const NodeAddress&)>& visit) {
    if (!visit) return;
    VisitNodes(document, [&](const NodeAddress& address) {
        if (!address.block) return;
        const auto* blocks = FindBlockList(const_cast<Document&>(document), address);
        if (!blocks) return;
        visit((*blocks)[*address.block], address);
    });
}

void VisitHeadings(const Document& document,
                   const std::function<void(const NodeAddress&)>& visit) {
    if (!visit) return;
    VisitNodes(document, [&](const NodeAddress& address) {
        if (address.is_heading()) visit(address);
    });
}

void VisitSections(const Document& document,
                   const std::function<void(const Section&, size_t index)>& visit) {
    if (!visit) return;
    const auto& sections = BodyOf(document).sections;
    for (size_t si = 0; si < sections.size(); ++si) visit(sections[si], si);
}

void VisitInlineContent(
    const Document& document,
    const std::function<void(const InlineContent&, const NodeAddress&)>& visit) {
    if (!visit) return;
    const FrontMatter& front = FrontMatterOf(document);
    {
        NodeAddress address;
        address.node = NodeId("front:title");
        address.kind = NodeKind::Paragraph;
        if (!front.title.empty()) visit(front.title, address);
    }
    if (front.abstract_text) {
        NodeAddress address;
        address.node = NodeId("front:abstract");
        address.kind = NodeKind::Paragraph;
        visit(*front.abstract_text, address);
    }
    VisitBlocks(document, [&](const Block& block, const NodeAddress& address) {
        std::visit(
            [&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, Paragraph>) {
                    visit(b.content, address);
                } else if constexpr (std::is_same_v<T, Figure> ||
                                     std::is_same_v<T, Table>) {
                    if (!b.caption.empty()) visit(b.caption, address);
                }
            },
            block);
    });
}

std::vector<NodeId> CollectAllNodeIds(const Document& document) {
    std::vector<NodeId> out;
    VisitNodes(document, [&](const NodeAddress& address) { out.push_back(address.node); });
    return out;
}

}  // namespace pf
