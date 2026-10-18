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
            if constexpr (std::is_same_v<T, Paragraph>)
                return NodeKind::Paragraph;
            if constexpr (std::is_same_v<T, Figure>)
                return NodeKind::Figure;
            if constexpr (std::is_same_v<T, Table>)
                return NodeKind::Table;
            if constexpr (std::is_same_v<T, ListBlock>)
                return NodeKind::List;
            if constexpr (std::is_same_v<T, TheoremBlock>)
                return NodeKind::Theorem;
            if constexpr (std::is_same_v<T, CodeBlock>)
                return NodeKind::Code;
            return NodeKind::Equation;
        },
        block);
}

NodeId BlockId(const Block& block) {
    return std::visit([](const auto& b) { return b.id; }, block);
}

// 脚注 body 按引用点递归访问：citation 首次引用编号、交叉引用校验和
// 数学校验都必须覆盖脚注里的行内内容。非法嵌套由 Validator 报告；这里
// 仍限制深度，防止手工构造的共享 body 形成环。
void VisitInlineContentRecursive(const InlineContent& content, const NodeAddress& address,
                                 const std::function<void(const InlineContent&, const NodeAddress&)>& visit,
                                 int depth) {
    visit(content, address);
    if (depth >= 2)
        return;
    for (const auto& node : content) {
        if (const auto* footnote = std::get_if<Footnote>(&node)) {
            if (footnote->body && !footnote->body->content.empty())
                VisitInlineContentRecursive(footnote->body->content, address, visit, depth + 1);
        }
    }
}

} // namespace

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
    if (!address.section || !address.block)
        return nullptr;
    auto& sections = BodyOf(document).sections;
    if (*address.section >= sections.size())
        return nullptr;
    Section& section = sections[*address.section];
    if (!address.subsection)
        return &section.blocks;
    if (*address.subsection >= section.subsections.size())
        return nullptr;
    Subsection& sub = section.subsections[*address.subsection];
    if (!address.subsubsection)
        return &sub.blocks;
    if (*address.subsubsection >= sub.subsubsections.size())
        return nullptr;
    return &sub.subsubsections[*address.subsubsection].blocks;
}

const std::vector<Block>* FindBlockList(const Document& document, const NodeAddress& address) {
    return FindBlockList(const_cast<Document&>(document), address);
}

Section* FindSection(Document& document, const NodeId& id) {
    for (auto& section : BodyOf(document).sections) {
        if (section.id == id)
            return &section;
    }
    return nullptr;
}

Subsection* FindSubsection(Document& document, const NodeId& id) {
    for (auto& section : BodyOf(document).sections) {
        for (auto& sub : section.subsections) {
            if (sub.id == id)
                return &sub;
        }
    }
    return nullptr;
}

Subsubsection* FindSubsubsection(Document& document, const NodeId& id) {
    for (auto& section : BodyOf(document).sections) {
        for (auto& sub : section.subsections) {
            for (auto& subsub : sub.subsubsections) {
                if (subsub.id == id)
                    return &subsub;
            }
        }
    }
    return nullptr;
}

Block* FindBlock(Document& document, const NodeId& id) {
    auto address = LocateNode(document, id);
    if (!address || !address->block)
        return nullptr;
    auto* blocks = FindBlockList(document, *address);
    if (!blocks)
        return nullptr;
    return &(*blocks)[*address->block];
}

void VisitNodes(const Document& document, const std::function<void(const NodeAddress&)>& visit) {
    if (!visit)
        return;
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

void VisitBlocks(const Document& document, const std::function<void(const Block&, const NodeAddress&)>& visit) {
    if (!visit)
        return;
    VisitNodes(document, [&](const NodeAddress& address) {
        if (!address.block)
            return;
        const auto* blocks = FindBlockList(const_cast<Document&>(document), address);
        if (!blocks)
            return;
        visit((*blocks)[*address.block], address);
    });
}

void VisitHeadings(const Document& document, const std::function<void(const NodeAddress&)>& visit) {
    if (!visit)
        return;
    VisitNodes(document, [&](const NodeAddress& address) {
        if (address.is_heading())
            visit(address);
    });
}

void VisitInlineContent(const Document& document,
                        const std::function<void(const InlineContent&, const NodeAddress&)>& visit) {
    if (!visit)
        return;
    auto visit_content = [&](const InlineContent& content, const NodeAddress& address) {
        VisitInlineContentRecursive(content, address, visit, 0);
    };
    const FrontMatter& front = FrontMatterOf(document);
    {
        NodeAddress address;
        address.node = NodeId("front:title");
        address.kind = NodeKind::Paragraph;
        if (!front.title.empty())
            visit_content(front.title, address);
    }
    if (front.abstract_text) {
        NodeAddress address;
        address.node = NodeId("front:abstract");
        address.kind = NodeKind::Paragraph;
        visit_content(*front.abstract_text, address);
    }
    // 与 renderer 保持阅读顺序：标题、该标题下的块、下一层标题；表格题注
    // 在单元格之前，列表按 item 顺序。citation 首次出现编号与所有引用校验
    // 都消费此遍历。
    VisitNodes(document, [&](const NodeAddress& address) {
        const auto& section = document.body().sections[*address.section];
        if (address.kind == NodeKind::Section) {
            visit_content(section.title, address);
            return;
        }
        if (address.kind == NodeKind::Subsection) {
            visit_content(section.subsections[*address.subsection].title, address);
            return;
        }
        if (address.kind == NodeKind::Subsubsection) {
            visit_content(section.subsections[*address.subsection].subsubsections[*address.subsubsection].title,
                          address);
            return;
        }
        const auto* blocks = FindBlockList(document, address);
        if (!blocks || !address.block)
            return;
        const Block& block = (*blocks)[*address.block];
        std::visit(
            [&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, Paragraph>) {
                    visit_content(b.content, address);
                } else if constexpr (std::is_same_v<T, Figure>) {
                    if (!b.caption.empty())
                        visit_content(b.caption, address);
                } else if constexpr (std::is_same_v<T, Table>) {
                    if (!b.caption.empty())
                        visit_content(b.caption, address);
                    for (const auto& row : b.cells) {
                        for (const auto& cell : row) {
                            if (!cell.content.empty())
                                visit_content(cell.content, address);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ListBlock>) {
                    for (const auto& item : b.items) {
                        if (!item.content.empty())
                            visit_content(item.content, address);
                    }
                } else if constexpr (std::is_same_v<T, TheoremBlock>) {
                    if (!b.title.empty())
                        visit_content(b.title, address);
                    if (!b.body.empty())
                        visit_content(b.body, address);
                } else if constexpr (std::is_same_v<T, CodeBlock>) {
                    if (!b.caption.empty())
                        visit_content(b.caption, address);
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

} // namespace pf
