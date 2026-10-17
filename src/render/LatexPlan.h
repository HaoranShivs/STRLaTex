#pragma once
#include "render/LatexRenderer.h"
#include <memory>

namespace pf {
// Small IR: text, identifiers, paths and math cannot become template syntax.
// Commands/environments and all adapter choices are resolved before emission.
struct LatexNode {
    enum class Kind {
        Literal,
        Text,
        Identifier,
        AssetPath,
        InlineMath,
        DisplayMath,
        Command,
        Environment
    };
    Kind kind = Kind::Literal;
    std::string value;
    std::string option;
    std::vector<std::vector<LatexNode>> arguments;
    std::vector<LatexNode> children;
    NodeId source_node;
    std::string source_label;
    std::string origin = "common";
};
class LatexGenerationPlan {
  public:
    const std::vector<LatexNode>& nodes() const {
        return nodes_;
    }
    const BuildPackage& package() const {
        return package_;
    }
    const std::string& audit_json() const {
        return audit_json_;
    }

  private:
    friend class LatexPlanner;
    LatexGenerationPlan() = default;
    std::vector<LatexNode> nodes_;
    BuildPackage package_;
    std::string audit_json_;
};
struct PlanResult {
    std::shared_ptr<const LatexGenerationPlan> plan;
    std::vector<Diagnostic> diagnostics;
};
class LatexPlanner {
  public:
    PlanResult Plan(const RenderRequest& request,
                    const TemplateDefinition& definition) const;
};
class LatexEmitter {
  public:
    // No Document, registry, adapter callback or fallback policy here.
    RenderResult Emit(const LatexGenerationPlan& plan) const;
};
} // namespace pf
