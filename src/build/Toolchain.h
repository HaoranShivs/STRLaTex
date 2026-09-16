#pragma once
// Toolchain value types (plan §6, §7, §14).
//
// Deliberately free of includes into render/template/build so both
// TemplateRegistry and Compiler can carry them without an include cycle: the
// renderer depends on the template, the template declares the toolchain, and
// the compiler consumes it.

#include <cstdint>
#include <string>
#include <vector>

namespace pf {

// Which LaTeX engine compiles the document. The template author decides this
// (IEEEtran, for example, is designed for pdfLaTeX and loses its font setup
// under an XeTeX engine); nothing downstream re-derives it from the document.
enum class LatexEngine : std::uint8_t {
    PdfLatex,
    XeLatex,
    LuaLatex,
};

const char* ToString(LatexEngine engine);

enum class BibliographyEngine : std::uint8_t {
    None,
    BibTex,
    Biber,
};

const char* ToString(BibliographyEngine engine);

// What the build needs, resolved by the session from the template and carried
// with every request (plan §14).
struct BuildToolchain {
    LatexEngine engine = LatexEngine::PdfLatex;
    BibliographyEngine bibliography_engine = BibliographyEngine::None;

    bool operator==(const BuildToolchain&) const = default;
};

// How a template declares that requirement (plan §7).
struct TemplateToolchainRequirement {
    LatexEngine engine = LatexEngine::PdfLatex;
    BibliographyEngine bibliography_engine = BibliographyEngine::None;
    std::vector<std::string> required_packages;
};

}  // namespace pf
