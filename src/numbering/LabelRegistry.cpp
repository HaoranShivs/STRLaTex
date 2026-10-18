#include "numbering/LabelRegistry.h"

#include <cctype>
#include <type_traits>

#include "document/DocumentTraversal.h"

namespace pf {

namespace {

std::string Trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string EffectiveLabel(const std::string& requested, const NodeId& node) {
    return requested.empty() ? node.value() : requested;
}

} // namespace

std::string LabelRegistry::SanitizeLabel(const std::string& label) {
    const std::string trimmed = Trim(label);
    std::string out;
    for (unsigned char c : trimmed) {
        if (std::isalnum(c) || c == ':' || c == '.' || c == '_' || c == '-')
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '-';
    }
    return out;
}

void LabelRegistry::Register(const NodeId& node, const std::string& requested) {
    std::string label = SanitizeLabel(requested);
    if (label.empty())
        label = SanitizeLabel(node.value());
    if (label.empty())
        return; // 理论上 node id 总是非空且可清洗；防御性分支。
    labels_[node] = label;
    const auto existing = owners_.find(label);
    if (existing == owners_.end()) {
        owners_.emplace(label, node);
        return;
    }
    if (existing->second != node)
        conflicts_.push_back(LabelConflict{label, existing->second, node});
}

LabelRegistry LabelRegistry::Build(const Document& document) {
    LabelRegistry registry;
    VisitNodes(document, [&](const NodeAddress& address) {
        if (address.is_heading()) {
            registry.Register(address.node, address.node.value());
            return;
        }
        if (!address.block)
            return;
        const std::vector<Block>* blocks = FindBlockList(document, address);
        if (!blocks || *address.block >= blocks->size())
            return;
        const Block& block = (*blocks)[*address.block];
        std::visit(
            [&](const auto& typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, Figure> || std::is_same_v<T, Table>) {
                    registry.Register(typed.id, EffectiveLabel(typed.label, typed.id));
                } else if constexpr (std::is_same_v<T, EquationBlock>) {
                    // Unnumbered equations never emit \label, so they must not
                    // occupy a label or produce a conflict.
                    if (typed.numbered)
                        registry.Register(typed.id, EffectiveLabel(typed.label, typed.id));
                } else if constexpr (std::is_same_v<T, TheoremBlock>) {
                    if (typed.kind != TheoremKind::Proof && typed.numbered)
                        registry.Register(typed.id, EffectiveLabel(typed.label, typed.id));
                } else if constexpr (std::is_same_v<T, CodeBlock>) {
                    registry.Register(typed.id, EffectiveLabel(typed.label, typed.id));
                }
            },
            block);
    });
    return registry;
}

std::string LabelRegistry::LabelFor(const NodeId& node) const {
    const auto it = labels_.find(node);
    return it == labels_.end() ? SanitizeLabel(node.value()) : it->second;
}

} // namespace pf
