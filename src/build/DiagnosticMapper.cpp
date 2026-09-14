#include "build/DiagnosticMapper.h"

namespace pf {

std::vector<Diagnostic> DiagnosticMapper::Map(
    const CompileResult& result, const SourceMap& source_map,
    ProjectRevision revision) const {
    std::vector<Diagnostic> diagnostics;
    std::uint64_t counter = 0;
    for (const auto& message : result.messages) {
        Diagnostic diagnostic;
        diagnostic.id = MakeDiagnosticId("compiler", ++counter);
        diagnostic.source = DiagnosticSource::Compiler;
        diagnostic.severity = message.is_error
                                  ? DiagnosticSeverity::Error
                                  : DiagnosticSeverity::Warning;
        diagnostic.code =
            message.is_error ? "E-COMPILER" : "W-COMPILER";
        diagnostic.message = message.text;
        diagnostic.revision = revision;
        if (auto node = source_map.Resolve(message.line)) {
            diagnostic.location = DiagnosticLocation::ForNode(*node);
        } else {
            diagnostic.location = DiagnosticLocation::ForProject();
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

}  // namespace pf
