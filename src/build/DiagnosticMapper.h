#pragma once

#include <string>
#include <vector>

#include "build/Compiler.h"
#include "core/Diagnostic.h"
#include "render/SourceMap.h"

namespace pf {

// Turns raw compiler messages into structured Diagnostics (Build Diagnostics
// plan §15-§19). The mapper is the only place where a compiler line becomes a
// problem: the GUI never parses log text itself.
class DiagnosticMapper {
public:
    std::vector<Diagnostic> Map(const CompileResult& result,
                                const SourceMap& source_map,
                                ProjectRevision revision,
                                const BuildId& build_id = BuildId()) const;

    // Stable diagnostic code for one compiler message (plan §13/§16): the
    // first version classifies the high-value LaTeX patterns and leaves
    // everything else as LATEX_ERROR / LATEX_WARNING. The classifier may
    // downgrade a raw "is_error" line that is really a warning (Overfull,
    // undefined reference/citation, ...).
    static std::string CodeFor(const CompilerMessage& message,
                               DiagnosticSeverity* severity);
};

}  // namespace pf
