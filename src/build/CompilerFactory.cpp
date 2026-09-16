#include "build/CompilerFactory.h"

namespace pf {

BuildToolchain CompilerFactory::ToolchainFor(
    const TemplateDefinition& tpl) const {
    BuildToolchain toolchain;
    toolchain.engine = tpl.toolchain.engine;
    toolchain.bibliography_engine = tpl.toolchain.bibliography_engine;
    return toolchain;
}

std::unique_ptr<ICompiler> CompilerFactory::Create(
    const TemplateDefinition& tpl) const {
    // One backend for every engine in stage 1 (plan §31): latexmk drives
    // whichever binary the template's engine selects. The compiler is told the
    // engine per request, so a single instance works for all templates.
    CompilerConfig config;
    config.engine = tpl.toolchain.engine;
    config.bibliography_engine = tpl.toolchain.bibliography_engine;
    config.texlive_root = texlive_root_;
    config.keep_logs = true;
    return std::make_unique<TexLiveCompiler>(std::move(config));
}

}  // namespace pf
