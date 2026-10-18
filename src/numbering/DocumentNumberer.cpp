#include "numbering/DocumentNumberer.h"

#include <map>
#include <utility>

#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "numbering/LabelRegistry.h"

namespace pf {

namespace {

std::string HeadingDisplay(int section, int subsection, int subsubsection, NodeKind kind) {
    std::string out = "Section " + std::to_string(section);
    if (kind != NodeKind::Section) {
        out += "." + std::to_string(subsection);
        if (kind == NodeKind::Subsubsection)
            out += "." + std::to_string(subsubsection);
    }
    return out;
}

} // namespace

DocumentNumberer DocumentNumberer::Build(const Document& document, const NumberingOptions& options) {
    DocumentNumberer numberer;
    const LabelRegistry labels = LabelRegistry::Build(document);

    int section = 0;
    int subsection = 0;
    int subsubsection = 0;
    int figure = 0;
    int table = 0;
    int equation = 0;
    int code = 0;
    std::map<TheoremKind, int> theorems;

    auto add = [&](const NodeId& node, NodeKind kind, int number, std::string kind_label, std::string display,
                   std::string title) {
        NumberedNode entry;
        entry.node = node;
        entry.kind = kind;
        entry.number = number;
        entry.kind_label = std::move(kind_label);
        entry.display = std::move(display);
        entry.title = std::move(title);
        entry.latex_label = labels.LabelFor(node);
        numberer.entries_[node] = std::move(entry);
        numberer.order_.push_back(node);
    };

    VisitNodes(document, [&](const NodeAddress& address) {
        switch (address.kind) {
        case NodeKind::Section: {
            ++section;
            subsection = 0;
            subsubsection = 0;
            if (!options.number_sections)
                return;
            const auto& title = document.body().sections[*address.section].title;
            add(address.node, NodeKind::Section, section, "Section", HeadingDisplay(section, 0, 0, NodeKind::Section),
                InlineToPlainText(title));
            return;
        }
        case NodeKind::Subsection: {
            ++subsection;
            subsubsection = 0;
            if (!options.number_sections)
                return;
            const auto& title = document.body().sections[*address.section].subsections[*address.subsection].title;
            add(address.node, NodeKind::Subsection, subsection, "Subsection",
                HeadingDisplay(section, subsection, 0, NodeKind::Subsection), InlineToPlainText(title));
            return;
        }
        case NodeKind::Subsubsection: {
            ++subsubsection;
            if (!options.number_sections)
                return;
            const auto& title = document.body()
                                    .sections[*address.section]
                                    .subsections[*address.subsection]
                                    .subsubsections[*address.subsubsection]
                                    .title;
            add(address.node, NodeKind::Subsubsection, subsubsection, "Subsubsection",
                HeadingDisplay(section, subsection, subsubsection, NodeKind::Subsubsection), InlineToPlainText(title));
            return;
        }
        default:
            break;
        }

        if (!address.block)
            return;
        const std::vector<Block>* blocks = FindBlockList(document, address);
        if (!blocks || *address.block >= blocks->size())
            return;
        const Block& block = (*blocks)[*address.block];
        if (const auto* fig = std::get_if<Figure>(&block)) {
            if (options.number_figures) {
                ++figure;
                add(fig->id, NodeKind::Figure, figure, "Figure", "Figure " + std::to_string(figure),
                    InlineToPlainText(fig->caption));
            }
            return;
        }
        if (const auto* tab = std::get_if<Table>(&block)) {
            if (options.number_tables) {
                ++table;
                add(tab->id, NodeKind::Table, table, "Table", "Table " + std::to_string(table),
                    InlineToPlainText(tab->caption));
            }
            return;
        }
        if (const auto* eq = std::get_if<EquationBlock>(&block)) {
            if (eq->numbered && options.number_equations) {
                ++equation;
                add(eq->id, NodeKind::Equation, equation, "Equation", "Eq. (" + std::to_string(equation) + ")", {});
            }
            return;
        }
        if (const auto* theorem = std::get_if<TheoremBlock>(&block)) {
            if (theorem->kind == TheoremKind::Proof || !theorem->numbered)
                return;
            const int number = ++theorems[theorem->kind];
            const std::string label = ToString(theorem->kind);
            add(theorem->id, NodeKind::Theorem, number, label, label + " " + std::to_string(number),
                InlineToPlainText(theorem->title));
            return;
        }
        if (const auto* listing = std::get_if<CodeBlock>(&block)) {
            ++code;
            add(listing->id, NodeKind::Code, code, "Listing", "Listing " + std::to_string(code),
                InlineToPlainText(listing->caption));
            return;
        }
    });
    return numberer;
}

std::optional<NumberedNode> DocumentNumberer::Find(const NodeId& node) const {
    const auto it = entries_.find(node);
    if (it == entries_.end())
        return std::nullopt;
    return it->second;
}

std::vector<NumberedNode> DocumentNumberer::All() const {
    std::vector<NumberedNode> out;
    out.reserve(order_.size());
    for (const auto& node : order_) {
        const auto it = entries_.find(node);
        if (it != entries_.end())
            out.push_back(it->second);
    }
    return out;
}

std::vector<NumberedNode> DocumentNumberer::ByKind(NodeKind kind) const {
    std::vector<NumberedNode> out;
    for (const auto& node : order_) {
        const auto it = entries_.find(node);
        if (it != entries_.end() && it->second.kind == kind)
            out.push_back(it->second);
    }
    return out;
}

std::string DocumentNumberer::DisplayFor(const NodeId& node) const {
    const auto it = entries_.find(node);
    return it == entries_.end() ? std::string{} : it->second.display;
}

std::vector<CrossReferenceItem> CollectCrossReferences(const Document& document, const DocumentNumberer& numbering) {
    (void)document; // titles/labels already live in the numbering value object
    std::vector<CrossReferenceItem> items;
    for (const auto& entry : numbering.All()) {
        if (entry.kind == NodeKind::Paragraph)
            continue;
        CrossReferenceItem item;
        item.node = entry.node;
        item.kind = entry.kind;
        item.display = entry.display;
        item.title = entry.title;
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace pf
