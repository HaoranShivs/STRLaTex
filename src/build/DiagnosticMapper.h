#pragma once

#include <vector>

#include "build/Compiler.h"
#include "core/Diagnostic.h"
#include "render/SourceMap.h"

namespace pf {

class DiagnosticMapper {
public:
    std::vector<Diagnostic> Map(const CompileResult& result,
                                const SourceMap& source_map,
                                ProjectRevision revision) const;
};

}  // namespace pf
