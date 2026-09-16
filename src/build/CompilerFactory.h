#pragma once
// CompilerFactory: the only place that turns a template's toolchain
// requirement into a compiler instance (plan §13).
//
// The template decides what the build needs; the factory decides which backend
// provides it. Nothing downstream re-derives the engine from the document.

#include <memory>
#include <string>

#include "build/Compiler.h"
#include "build/RuntimeManager.h"
#include "template/TemplateRegistry.h"

namespace pf {

class CompilerFactory {
public:
    // texlive_root is the bundled portable runtime; empty means the runtime is
    // unavailable and the production backend will refuse the build with a
    // runtime error (never a document error).
    explicit CompilerFactory(std::filesystem::path texlive_root)
        : texlive_root_(std::move(texlive_root)) {}

    // Build the compiler the template asks for (plan §31): pdfLaTeX through
    // the bundled TeX Live runtime. XeLaTeX/LuaLaTeX are part of the protocol
    // but no shipped template selects them yet; until the runtime carries
    // those engines they fall back to the same pdfLaTeX backend with the
    // engine flag the template requested.
    std::unique_ptr<ICompiler> Create(const TemplateDefinition& tpl) const;

    // The toolchain the build request should carry (plan §14).
    BuildToolchain ToolchainFor(const TemplateDefinition& tpl) const;

private:
    std::filesystem::path texlive_root_;
};

}  // namespace pf
