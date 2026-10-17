#include "build/Toolchain.h"
namespace pf {
const char* ToString(LatexEngine engine) {
    switch (engine) {
    case LatexEngine::PdfLatex:
        return "pdflatex";
    case LatexEngine::XeLatex:
        return "xelatex";
    case LatexEngine::LuaLatex:
        return "lualatex";
    }
    return "?";
}

const char* ToString(BibliographyEngine engine) {
    switch (engine) {
    case BibliographyEngine::None:
        return "none";
    case BibliographyEngine::BibTex:
        return "bibtex";
    case BibliographyEngine::Biber:
        return "biber";
    }
    return "?";
}

} // namespace pf
