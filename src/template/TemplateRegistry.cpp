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
        t.bibliography_style = "plain";
        t.preamble_lines = {
            "\\usepackage{amsmath}",
            "\\usepackage{amssymb}",
            "\\usepackage{graphicx}",
            "\\usepackage{booktabs}",
            "\\usepackage[margin=1in]{geometry}",
            "\\usepackage[hidelinks]{hyperref}",
            "\\usepackage{caption}",
        };
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
