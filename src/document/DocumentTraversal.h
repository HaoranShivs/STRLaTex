#pragma once
// DocumentTraversal: the single way to walk a Document (next-stage plan §8).
//
// Before this header every consumer rolled its own
// `for section / for subsection / for subsubsection` loop and hand-copied the
// block-kind switch. Adding the third heading level then meant touching every
// one of them. Now the nesting lives in exactly one place; Renderer, Validator,
// DocumentIndex, Outline, CrossReference search and the editor's move/insert
// logic all sit on top of it.
//
// Nothing here mutates the document, and nothing here knows about LaTeX, Qt,
// undo or persistence.

#include <functional>
#include <optional>
#include <vector>

#include "document/Document.h"

namespace pf {

// Where a node sits, as indices into the document's own containers. This is
// the value the editor needs to mutate in place; NodeId alone is not enough
// because blocks live in four different vectors.
struct NodeAddress {
    NodeId node;
    NodeKind kind = NodeKind::Paragraph;

    std::optional<size_t> section;
    std::optional<size_t> subsection;
    std::optional<size_t> subsubsection;
    std::optional<size_t> block;

    // Depth 1..3 for headings, 0 for blocks.
    int depth() const noexcept { return HeadingDepth(kind); }
    bool is_heading() const noexcept { return IsHeadingKind(kind); }
};

// Locate a node by id anywhere in the body. nullopt when absent.
std::optional<NodeAddress> LocateNode(const Document& document, const NodeId& node);

// The blocks vector a node lives in. Nullptr for headings.
std::vector<Block>* FindBlockList(Document& document, const NodeAddress& address);
const std::vector<Block>* FindBlockList(const Document& document,
                                        const NodeAddress& address);

// Pointers to a specific node. Nullptr when the kind does not match.
Section* FindSection(Document& document, const NodeId& id);
Subsection* FindSubsection(Document& document, const NodeId& id);
Subsubsection* FindSubsubsection(Document& document, const NodeId& id);
Block* FindBlock(Document& document, const NodeId& id);

// ---------------- Visitors ----------------

// Every structural node in document order: sections, their blocks,
// subsections, their blocks, subsubsections, their blocks.
void VisitNodes(const Document& document,
                const std::function<void(const NodeAddress&)>& visit);

// Blocks only, in document order.
void VisitBlocks(const Document& document,
                 const std::function<void(const Block&, const NodeAddress&)>& visit);

// Headings only (Section/Subsection/Subsubsection), in document order.
void VisitHeadings(const Document& document,
                   const std::function<void(const NodeAddress&)>& visit);

// Convenience: every Section, in order.
void VisitSections(const Document& document,
                   const std::function<void(const Section&, size_t index)>& visit);

// All inline content in the document (front matter title/abstract/captions and
// paragraph bodies), in document order. Used by cross-reference / citation
// searches.
void VisitInlineContent(
    const Document& document,
    const std::function<void(const InlineContent&, const NodeAddress&)>& visit);

// Collect the ids of all nodes in document order.
std::vector<NodeId> CollectAllNodeIds(const Document& document);

// Escape hatch for the container access above. Kept in one place so the
// "who may reach into a Document" rule stays auditable.
class DocumentMutableAccess {
public:
    static FrontMatter& front_matter(Document& document) {
        return document.front_matter();
    }
    static Body& body(Document& document) { return document.body(); }
    static BackMatter& back_matter(Document& document) {
        return document.back_matter();
    }
};

}  // namespace pf
