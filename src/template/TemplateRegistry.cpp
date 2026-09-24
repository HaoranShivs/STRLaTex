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
        // Citation 方案 §3：GUI 按首次引用顺序为引用编号
        //（CitationNumberResolver）。`plain` 会按字母序排列参考文献，
        //因此编辑器里的 [1] 与 PDF 对不上。`unsrtnat` 按引用顺序编号——
        //正好符合 GUI 的策略——由 natbib 提供（renderer 只要遇到 .bib
        // 就会加载 natbib）。IEEE 保留 `IEEEtran`，同样是按引用顺序。
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
        // 普通 article 类对引擎无要求，但生产路径同样是 pdfLaTeX + BibTeX（方案 §8）。
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
        // IEEE 会议论文必须列出作者的所属机构。
        t.required.authors = true;
        t.required.affiliations = true;
        t.required.author_affiliations = true;
        t.required.abstract_text = true;
        t.required.keywords = true;
        // IEEEtran 会议模式支持 \\subsubsection，因此三个标题层级都可用。
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

} // namespace pf
