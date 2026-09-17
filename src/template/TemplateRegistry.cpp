#include "template/TemplateRegistry.h"

namespace pf {

TemplateRegistry& TemplateRegistry::Instance() {
    static TemplateRegistry instance;
    return instance;
}

TemplateRegistry::TemplateRegistry() {
    {
        TemplateDefinition t;
        t.id = "generic-article";
        t.name = "Generic Article";
        t.document_class = "article";
        t.class_options = {"11pt", "a4paper"};
        t.two_column = false;
        // Citation plan §3: the GUI numbers citations by first-citation order
        // (CitationNumberResolver). `plain` sorts the bibliography
        // alphabetically, so [1] in the editor would not match the PDF.
        // `unsrtnat` numbers entries in citation order - exactly the GUI
        // policy - and natbib (loaded by the renderer whenever a .bib is
        // present) provides it. IEEE keeps `IEEEtran`, also citation-order.
        t.bibliography_style = "unsrtnat";
        t.preamble_lines = {
            "\\usepackage{amsmath}",
            "\\usepackage{amssymb}",
            "\\usepackage{graphicx}",
            "\\usepackage{booktabs}",
            "\\usepackage[margin=1in]{geometry}",
            "\\usepackage[hidelinks]{hyperref}",
            "\\usepackage{caption}",
        };
        // Plain article classes are engine-agnostic, but the production path
        // is pdfLaTeX + BibTeX as well (plan §8).
        t.toolchain.engine = LatexEngine::PdfLatex;
        t.toolchain.bibliography_engine = BibliographyEngine::BibTex;
        t.toolchain.required_packages = {"amsmath", "booktabs", "hyperref"};
        templates_[t.id] = t;
    }
    {
        TemplateDefinition t;
        t.id = "ieee-conference";
        t.name = "IEEE Conference";
        t.document_class = "IEEEtran";
        t.class_options = {"conference"};
        t.two_column = true;
        t.bibliography_style = "IEEEtran";
        t.preamble_lines = {
            "\\usepackage{amsmath}",
            "\\usepackage{amssymb}",
            "\\usepackage{graphicx}",
            "\\usepackage{booktabs}",
            "\\usepackage[hidelinks]{hyperref}",
        };
        // IEEE conference papers must list authors with affiliations.
        t.required.authors = true;
        t.required.affiliations = true;
        t.required.author_affiliations = true;
        t.required.abstract_text = true;
        t.required.keywords = true;
        // IEEEtran conference mode supports \\subsubsection, so all three
        // heading levels are available.
        t.capabilities.max_heading_depth = 3;
        templates_[t.id] = t;
    }
}

const TemplateDefinition* TemplateRegistry::Find(const std::string& id) const {
    auto it = templates_.find(id);
    return it == templates_.end() ? nullptr : &it->second;
}

std::vector<TemplateDefinition> TemplateRegistry::All() const {
    std::vector<TemplateDefinition> out;
    for (const auto& [id, def] : templates_) {
        out.push_back(def);
    }
    return out;
}

}  // namespace pf
