#pragma once
// DocumentTraversal：遍历 Document 的唯一方式（下一阶段方案 §8）。
//
// 在这个头文件出现之前，每个调用方都各自手写
// `for section / for subsection / for subsubsection` 循环，并手工复制
// block 类型的分支判断。那时新增第三级标题就意味着要改动每一处。现在
// 嵌套结构只存在于一个地方；Renderer、Validator、DocumentIndex、Outline、
// CrossReference 搜索以及编辑器的移动/插入逻辑都构建在它之上。
//
// 这里的任何代码都不会修改 document，也不涉及 LaTeX、Qt、撤销或持久化。

#include <functional>
#include <optional>
#include <vector>

#include "document/Document.h"

namespace pf {

// 节点所处的位置，以 document 自身容器中的下标表示。这是编辑器就地修改
// 所需的取值；仅有 NodeId 并不够，因为 block 分布在四个不同的 vector 中。
struct NodeAddress {
    NodeId node;
    NodeKind kind = NodeKind::Paragraph;

    std::optional<size_t> section;
    std::optional<size_t> subsection;
    std::optional<size_t> subsubsection;
    std::optional<size_t> block;

    // 标题的深度为 1..3，block 为 0。
    int depth() const noexcept { return HeadingDepth(kind); }
    bool is_heading() const noexcept { return IsHeadingKind(kind); }
};

// 按 id 在 body 中任意位置查找节点。不存在时返回 nullopt。
std::optional<NodeAddress> LocateNode(const Document& document, const NodeId& node);

// 节点所在的 blocks vector。标题返回 nullptr。
std::vector<Block>* FindBlockList(Document& document, const NodeAddress& address);
const std::vector<Block>* FindBlockList(const Document& document,
                                        const NodeAddress& address);

// 指向特定节点的指针。类型不匹配时返回 nullptr。
Section* FindSection(Document& document, const NodeId& id);
Subsection* FindSubsection(Document& document, const NodeId& id);
Subsubsection* FindSubsubsection(Document& document, const NodeId& id);
Block* FindBlock(Document& document, const NodeId& id);

// ---------------- 访问器 ----------------

// 按文档顺序遍历所有结构节点：各 section 及其 block，各 subsection 及其
// block，各 subsubsection 及其 block。
void VisitNodes(const Document& document,
                const std::function<void(const NodeAddress&)>& visit);

// 仅遍历 block，按文档顺序。
void VisitBlocks(const Document& document,
                 const std::function<void(const Block&, const NodeAddress&)>& visit);

// 仅遍历标题（Section/Subsection/Subsubsection），按文档顺序。
void VisitHeadings(const Document& document,
                   const std::function<void(const NodeAddress&)>& visit);

// 便捷接口：按顺序遍历每个 Section。
void VisitSections(const Document& document,
                   const std::function<void(const Section&, size_t index)>& visit);

// document 中的所有内联内容（front matter 的标题/摘要/图表标题以及段落
// 正文），按文档顺序。供交叉引用/引文搜索使用。
void VisitInlineContent(
    const Document& document,
    const std::function<void(const InlineContent&, const NodeAddress&)>& visit);

// 按文档顺序收集所有节点的 id。
std::vector<NodeId> CollectAllNodeIds(const Document& document);

// 上述容器访问的逃生通道。集中放在一处，以便「谁可以深入访问 Document」
// 这一规则保持可审计。
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
