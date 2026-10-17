#pragma once
// DocumentNumberer：与 PDF 一致的文档编号服务（WP-03）。
//
// 编号规则刻意复刻 LaTeX 默认行为，使 picker / 胶囊 / References 面板显示的
// 编号与 \ref 最终打印的编号相同：
//
//   * Section      -> 1, 2, 3
//   * Subsection   -> 1.1, 1.2（按所属 section）
//   * Subsubsection-> 1.1.1（按所属 subsection）
//   * Figure/Table/Equation 全局递增，不按 section；
//   * unnumbered Equation 不占号，也不进入交叉引用列表。
//
// 本文件是纯 C++（Qt-free），GUI 与渲染器都只消费它给出的值对象。

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "document/Document.h"

namespace pf {

struct NumberedNode {
    NodeId node;
    NodeKind kind = NodeKind::Paragraph;
    int number = 0;         // heading 为层级内序号；图表/公式为全局序号
    std::string kind_label; // "Section" / "Subsection" / "Figure" / "Equation"…
    std::string display;    // "Section 2.1" / "Figure 3" / "Table 1" / "Eq. (4)"
    std::string title;      // 标题/题注纯文本（公式为空）
    std::string latex_label;
};

struct NumberingOptions {
    bool number_sections = true;
    bool number_figures = true;
    bool number_tables = true;
    bool number_equations = true;
};

// picker / 胶囊共用的引用目标。display 已包含类型前缀，与 PDF 的语义一致。
struct CrossReferenceItem {
    NodeId node;
    NodeKind kind = NodeKind::Paragraph;
    std::string display;
    std::string title;
};

class DocumentNumberer {
  public:
    static DocumentNumberer Build(const Document& document, const NumberingOptions& options = {});

    std::optional<NumberedNode> Find(const NodeId& node) const;
    const std::map<NodeId, NumberedNode>& entries() const noexcept {
        return entries_;
    }
    // 文档顺序。
    std::vector<NumberedNode> All() const;
    std::vector<NumberedNode> ByKind(NodeKind kind) const;
    // 未被编号的节点返回空字符串。
    std::string DisplayFor(const NodeId& node) const;

  private:
    std::map<NodeId, NumberedNode> entries_;
    std::vector<NodeId> order_;
};

// 选择器与胶囊的条目：只包含真正可被 \ref 的目标（未编号公式不在其中）。
std::vector<CrossReferenceItem> CollectCrossReferences(const Document& document, const DocumentNumberer& numbering);

} // namespace pf
