#include "core/IdGenerator.h"
#include "render/LatexPlan.h"

namespace pf {
RenderResult LatexRenderer::Render(const RenderRequest& request) const {
    // Legacy direct callers can resolve built-ins here. Production snapshots
    // already carry the exact immutable definition and verified resource bytes.
    const auto* definition = request.resolved_template.get();
    if (!definition && request.template_error.empty())
        definition = TemplateRegistry::Instance().Find(request.template_id);
    RenderResult result;
    if (!definition || !request.template_error.empty()) {
        result.status = RenderResult::Status::Failed;
        Diagnostic d;
        d.id = MakeDiagnosticId("rnd", 1);
        d.source = DiagnosticSource::Renderer;
        d.severity = DiagnosticSeverity::Error;
        d.code = "E-RENDER-TEMPLATE";
        d.message = request.template_error.empty()
                        ? "Unknown template: " + request.template_id
                        : request.template_error;
        d.location = DiagnosticLocation::ForProject();
        d.revision = request.revision;
        result.diagnostics.push_back(std::move(d));
    } else {
        auto planned = LatexPlanner().Plan(request, *definition);
        if (planned.plan)
            result = LatexEmitter().Emit(*planned.plan);
        else
            result.status = RenderResult::Status::Failed;
        result.diagnostics = std::move(planned.diagnostics);
    }
    result.build_id = request.build_id;
    result.revision = request.revision;
    return result;
}
} // namespace pf
