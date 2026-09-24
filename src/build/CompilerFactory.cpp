#include "build/CompilerFactory.h"

namespace pf {

BuildToolchain CompilerFactory::ToolchainFor(const TemplateDefinition& tpl) const {
    BuildToolchain toolchain;
    toolchain.engine = tpl.toolchain.engine;
    toolchain.bibliography_engine = tpl.toolchain.bibliography_engine;
    return toolchain;
}

std::unique_ptr<ICompiler> CompilerFactory::Create(const TemplateDefinition& tpl) const {
    // 阶段 1 中所有引擎共用一个后端（方案 §31）：模板的引擎选中哪个
    // 二进制，latexmk 就驱动哪个。引擎按请求告知编译器，因此单个实例
    // 即可服务所有模板。
    CompilerConfig config;
    config.engine = tpl.toolchain.engine;
    config.bibliography_engine = tpl.toolchain.bibliography_engine;
    config.texlive_root = texlive_root_;
    config.keep_logs = true;
    return std::make_unique<TexLiveCompiler>(std::move(config));
}

} // namespace pf
