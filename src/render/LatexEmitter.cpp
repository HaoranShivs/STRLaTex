#include "render/LatexPlan.h"
#include <algorithm>

namespace pf {
namespace {
std::string Escape(const std::string& text) {
    std::string out;
    for (char c : text)
        switch (c) {
        case '\\':
            out += "\\textbackslash{}";
            break;
        case '&':
            out += "\\&";
            break;
        case '%':
            out += "\\%";
            break;
        case '$':
            out += "\\$";
            break;
        case '#':
            out += "\\#";
            break;
        case '_':
            out += "\\_";
            break;
        case '{':
            out += "\\{";
            break;
        case '}':
            out += "\\}";
            break;
        case '~':
            out += "\\textasciitilde{}";
            break;
        case '^':
            out += "\\textasciicircum{}";
            break;
        default:
            out += c;
        }
    return out;
}
class Writer {
  public:
    std::string text;
    SourceMap map;
    std::uint32_t line = 1;
    void Add(const std::string& s) {
        text += s;
        line +=
            static_cast<std::uint32_t>(std::count(s.begin(), s.end(), '\n'));
    }
    void Nodes(const std::vector<LatexNode>& nodes) {
        for (const auto& node : nodes)
            Node(node);
    }
    void Node(const LatexNode& node) {
        const auto first = line;
        switch (node.kind) {
        case LatexNode::Kind::Text:
            Add(Escape(node.value));
            break;
        case LatexNode::Kind::Literal:
        case LatexNode::Kind::Identifier:
        case LatexNode::Kind::AssetPath:
            Add(node.value);
            Nodes(node.children);
            break;
        case LatexNode::Kind::InlineMath:
            Add("\\(" + node.value + "\\)");
            break;
        case LatexNode::Kind::DisplayMath:
            Add(node.value);
            break;
        case LatexNode::Kind::Command:
            Add("\\" + node.value);
            if (!node.option.empty())
                Add("[" + node.option + "]");
            for (const auto& arg : node.arguments) {
                Add("{");
                Nodes(arg);
                Add("}");
            }
            break;
        case LatexNode::Kind::Environment:
            Add("\\begin{" + node.value + "}");
            if (!node.option.empty())
                Add("[" + node.option + "]");
            for (const auto& arg : node.arguments) {
                Add("{");
                Nodes(arg);
                Add("}");
            }
            Add("\n");
            Nodes(node.children);
            Add("\\end{" + node.value + "}\n");
            break;
        }
        if (!node.source_node.empty() || !node.source_label.empty()) {
            const auto last =
                line > first && !text.empty() && text.back() == '\n' ? line - 1
                                                                     : line;
            for (auto l = first; l <= last; ++l)
                map.AddMapping(l, node.source_node, node.source_label);
        }
    }
};
} // namespace
RenderResult LatexEmitter::Emit(const LatexGenerationPlan& plan) const {
    RenderResult result;
    result.package = plan.package();
    Writer writer;
    writer.Nodes(plan.nodes());
    result.package.files.insert(result.package.files.begin(),
                                {"main.tex", std::move(writer.text), false});
    result.package.files.push_back(
        {"render-plan.json", plan.audit_json(), false});
    result.source_map = std::move(writer.map);
    return result;
}
} // namespace pf
