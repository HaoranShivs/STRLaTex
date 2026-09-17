#include "render/LatexRenderer.h"

#include <cctype>
#include <cstdio>

#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "math/MathGenerator.h"

namespace pf {

namespace {

// A LaTeX label must survive \label{} and \ref{} verbatim; user labels are
// sanitised to [A-Za-z0-9:._-] so a stray space or brace cannot break the
// generated document.
std::string SanitizeLabel(const std::string& label) {
    std::string out;
    out.reserve(label.size());
    for (char c : label) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == ':' || c == '.' || c == '_' || c == '-') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('-');
        }
    }
    return out;
}

}  // namespace

std::string LatexRenderer::LabelFor(const NodeId& node) const {
    const auto it = label_map_.find(node.value());
    if (it != label_map_.end()) return it->second;
    return node.value();
}

void LatexRenderer::FillLabelMap(const Document& doc) const {
    label_map_.clear();
    VisitBlocks(doc, [this](const Block& block, const NodeAddress&) {
        const auto* eq = std::get_if<EquationBlock>(&block);
        if (eq == nullptr) return;
        std::string label = SanitizeLabel(eq->label);
        if (label.empty()) label = eq->id.value();
        label_map_[eq->id.value()] = label;
    });
}

std::string LatexRenderer::EscapeLatex(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\textbackslash{}"; break;
            case '&': out += "\\&"; break;
            case '%': out += "\\%"; break;
            case '$': out += "\\$"; break;
            case '#': out += "\\#"; break;
            case '_': out += "\\_"; break;
            case '{': out += "\\{"; break;
            case '}': out += "\\}"; break;
            case '~': out += "\\textasciitilde{}"; break;
            case '^': out += "\\textasciicircum{}"; break;
            default: out += c;
        }
    }
    return out;
}

void LatexRenderer::RenderInline(const InlineContent& content, std::string* out) const {
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            std::string escaped = EscapeLatex(run->text);
            // Composite rendering (plan §4.3): Strong + Emphasis nests, one
            // does not override the other.
            const bool strong = HasMark(run->marks, TextMark::Strong);
            const bool emphasis = HasMark(run->marks, TextMark::Emphasis);
            if (strong && emphasis) {
                *out += "\\textbf{\\emph{" + escaped + "}}";
            } else if (strong) {
                *out += "\\textbf{" + escaped + "}";
            } else if (emphasis) {
                *out += "\\emph{" + escaped + "}";
            } else {
                *out += escaped;
            }
        } else if (const auto* eq = std::get_if<InlineMath>(&node)) {
            // Delimiters are generated, never stored (design §6).
            *out += GenerateInlineMath(eq->expression);
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            std::string keys;
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i) keys += ",";
                keys += cit->keys[i];
            }
            if (cit->mode == CitationMode::Narrative) {
                *out += "\\citet{" + keys + "}";
            } else {
                *out += "\\citep{" + keys + "}";
            }
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            *out += "\\ref{" + LabelFor(ref->target) + "}";
        }
    }
}

void LatexRenderer::RenderBlock(const Block& block, std::string* out,
                                SourceMap* smap) const {
    auto start_line = static_cast<std::uint32_t>(
        1 + std::count(out->begin(), out->end(), '\n'));

    if (const auto* para = std::get_if<Paragraph>(&block)) {
        std::string body;
        RenderInline(para->content, &body);
        if (!body.empty()) {
            *out += body;
            *out += "\n\n";
        }
    } else if (const auto* fig = std::get_if<Figure>(&block)) {
        const char* width = "1.0";
        switch (fig->width) {
            case FigureWidth::Percent25: width = "0.25"; break;
            case FigureWidth::Percent50: width = "0.5"; break;
            case FigureWidth::Percent75: width = "0.75"; break;
            case FigureWidth::Percent100: width = "1.0"; break;
        }
        std::string caption;
        RenderInline(fig->caption, &caption);
        std::string alt = EscapeLatex(fig->alt_text);
        std::string asset_file = fig->asset_id.value() + ".img";
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", width);
        *out += std::string("\\begin{figure}[htbp]\n\\centering\n") +
                "\\includegraphics[width=" + buf + "\\linewidth]{" + asset_file + "}\n";
        if (!caption.empty()) {
            *out += "\\caption{" + caption + "}";
            if (!alt.empty()) *out += " \\label{" + fig->id.value() + "}";
            *out += "\n";
        } else {
            *out += "\\caption{}\\label{" + fig->id.value() + "}\n";
        }
        *out += "\\end{figure}\n\n";
        (void)alt;
    } else if (const auto* table = std::get_if<Table>(&block)) {
        std::string caption;
        RenderInline(table->caption, &caption);
        std::string colspec;
        for (const auto& col : table->columns) {
            switch (col.alignment) {
                case ColumnAlignment::Left: colspec += 'l'; break;
                case ColumnAlignment::Center: colspec += 'c'; break;
                case ColumnAlignment::Right: colspec += 'r'; break;
            }
        }
        *out += "\\begin{table}[htbp]\n\\centering\n";
        if (!caption.empty()) {
            *out += "\\caption{" + caption + "}\\label{" + table->id.value() + "}\n";
        }
        *out += "\\begin{tabular}{" + colspec + "}\n";
        if (table->has_header_row && !table->cells.empty()) {
            std::string header;
            const auto& header_row = table->cells.front();
            for (size_t c = 0; c < header_row.size(); ++c) {
                if (c) header += " & ";
                RenderInline(header_row[c].content, &header);
            }
            *out += header + " \\\\\n\\midrule\n";
        }
        size_t start_row = table->has_header_row ? 1 : 0;
        for (size_t r = start_row; r < table->cells.size(); ++r) {
            std::string row;
            for (size_t c = 0; c < table->cells[r].size(); ++c) {
                if (c) row += " & ";
                RenderInline(table->cells[r][c].content, &row);
            }
            *out += row + " \\\\\n";
        }
        *out += "\\end{tabular}\n\\end{table}\n\n";
    } else if (const auto* eq = std::get_if<EquationBlock>(&block)) {
        // The outer environment and the label are generated from the block's
        // attributes; the user's source is only the body (design §4/§6).
        *out += GenerateDisplayMath(eq->expression, eq->numbered,
                                    eq->numbered ? LabelFor(eq->id)
                                                 : std::string());
    }

    auto end_line = static_cast<std::uint32_t>(
        1 + std::count(out->begin(), out->end(), '\n'));
    NodeId node_id = std::visit([](const auto& b) { return b.id; }, block);
    // The block kind travels with the mapping (Build Diagnostics plan §10):
    // Problems displays it as the location of a mapped compiler error.
    const std::string block_label = std::visit(
        [](const auto& b) -> std::string {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Paragraph>) return "Text";
            else if constexpr (std::is_same_v<T, Figure>) return "Figure";
            else if constexpr (std::is_same_v<T, Table>) return "Table";
            else if constexpr (std::is_same_v<T, EquationBlock>)
                return "Equation";
            else
                return "Block";
        },
        block);
    if (end_line > start_line) {
        GeneratedSourceRange range;
        range.file = "main.tex";
        range.begin_line = start_line;
        range.end_line = end_line - 1;
        smap->AddMapping(range, node_id, block_label);
    }
}

RenderResult LatexRenderer::Render(const RenderRequest& request) const {
    RenderResult result;
    result.build_id = request.build_id;
    result.revision = request.revision;

    const auto* tpl = TemplateRegistry::Instance().Find(request.template_id);
    if (!tpl) {
        result.status = RenderResult::Status::Failed;
        Diagnostic d;
        d.id = MakeDiagnosticId("rnd", 1);
        d.source = DiagnosticSource::Renderer;
        d.severity = DiagnosticSeverity::Error;
        d.code = "E-RENDER-TEMPLATE";
        d.message = "unknown template: " + request.template_id;
        d.revision = request.revision;
        d.location = DiagnosticLocation::ForProject();
        result.diagnostics.push_back(std::move(d));
        return result;
    }

    const Document& doc = *request.document;
    std::string tex;
    SourceMap& smap = result.source_map;
    smap.Clear();
    // Resolve equation labels before any \ref is emitted.
    FillLabelMap(doc);

    // --- Preamble ---
    std::string options;
    for (size_t i = 0; i < tpl->class_options.size(); ++i) {
        if (i) options += ",";
        options += tpl->class_options[i];
    }
    tex += "\\documentclass[" + options + "]{" + tpl->document_class + "}\n";
    for (const auto& line : tpl->preamble_lines) {
        tex += line + "\n";
    }
    if (!request.bibliography_bibtex.empty()) {
        tex += "\\usepackage[numbers,sort&compress]{natbib}\n";
    }
    tex += "\n\\begin{document}\n\n";

    // --- FrontMatter ---
    const auto& fm = doc.front_matter();
    std::string title;
    RenderInline(fm.title, &title);
    if (!title.empty()) tex += "\\title{" + title + "}\n";

    // Author / institution block: each institution is listed exactly once,
    // numbered by position, and authors carry the matching superscripts.
    // (Emitting one \thanks per author repeated shared institutions and let
    // the footnote markers drift away from the list.)
    if (!fm.authors.empty()) {
        const auto ordinal = [](size_t index) -> std::string {
            static const char* kSupers[] = {"1", "2", "3", "4",
                                            "5", "6", "7", "8", "9"};
            if (index < 9) {
                return std::string("\\textsuperscript{") + kSupers[index] +
                       "}";
            }
            return {};
        };
        const auto slot_of = [&fm](const AffiliationId& id) -> int {
            for (size_t i = 0; i < fm.affiliations.size(); ++i) {
                if (fm.affiliations[i].id == id) {
                    return static_cast<int>(i) + 1;
                }
            }
            return 0;
        };

        std::string authors;
        std::string emails;
        std::string institutions;
        for (size_t i = 0; i < fm.authors.size(); ++i) {
            const auto& author = fm.authors[i];
            if (i) authors += " \\and ";
            authors += EscapeLatex(author.name);
            // Numbers in institution order, duplicates collapsed.
            std::vector<int> slots;
            for (const auto& aff_id : author.affiliations) {
                const int slot = slot_of(aff_id);
                if (slot > 0 && std::find(slots.begin(), slots.end(), slot) ==
                                    slots.end()) {
                    slots.push_back(slot);
                }
            }
            std::sort(slots.begin(), slots.end());
            for (size_t s = 0; s < slots.size(); ++s) {
                if (s) authors += ",";
                authors += ordinal(static_cast<size_t>(slots[s]) - 1);
            }
            if (author.email) {
                if (!emails.empty()) emails += " \\\\ ";
                emails += "Email: " + EscapeLatex(*author.email);
            }
        }
        for (size_t i = 0; i < fm.affiliations.size(); ++i) {
            if (i) institutions += " \\\\ ";
            institutions += ordinal(i) + " " +
                            EscapeLatex(fm.affiliations[i].name);
        }
        if (!institutions.empty() || !emails.empty()) {
            authors += "\\thanks{";
            authors += institutions;
            if (!institutions.empty() && !emails.empty()) authors += " \\\\ ";
            authors += emails;
            authors += "}";
        }
        tex += "\\author{" + authors + "}\n";
    }
    tex += "\\maketitle\n\n";

    if (fm.abstract_text && !InlineIsBlank(*fm.abstract_text)) {
        std::string abstract_text;
        RenderInline(*fm.abstract_text, &abstract_text);
        tex += "\\begin{abstract}\n" + abstract_text + "\n\\end{abstract}\n\n";
    }
    if (!fm.keywords.empty()) {
        std::string kw;
        for (size_t i = 0; i < fm.keywords.size(); ++i) {
            if (i) kw += ", ";
            kw += EscapeLatex(fm.keywords[i]);
        }
        tex += "\\noindent\\textbf{Keywords:} " + kw + "\n\n";
    }

    // --- Body ---
    for (const auto& section : doc.body().sections) {
        std::string stitle;
        RenderInline(section.title, &stitle);
        tex += "\\section{" + stitle + "}\\label{" + section.id.value() + "}\n\n";
        for (const auto& block : section.blocks) {
            RenderBlock(block, &tex, &smap);
        }
        for (const auto& sub : section.subsections) {
            std::string sub_title;
            RenderInline(sub.title, &sub_title);
            tex += "\\subsection{" + sub_title + "}\\label{" + sub.id.value() + "}\n\n";
            for (const auto& block : sub.blocks) {
                RenderBlock(block, &tex, &smap);
            }
            for (const auto& subsub : sub.subsubsections) {
                std::string subsub_title;
                RenderInline(subsub.title, &subsub_title);
                tex += "\\subsubsection{" + subsub_title + "}\\label{" +
                       subsub.id.value() + "}\n\n";
                for (const auto& block : subsub.blocks) {
                    RenderBlock(block, &tex, &smap);
                }
            }
        }
    }

    // --- BackMatter / Bibliography ---
    if (!request.bibliography_bibtex.empty()) {
        BuildPackageFile bib_file;
        bib_file.path = "references.bib";
        bib_file.content = request.bibliography_bibtex;
        result.package.files.push_back(std::move(bib_file));
        result.package.bibliography_files.push_back("references.bib");
        tex += "\n\\bibliographystyle{" + tpl->bibliography_style + "}\n";
        if (tpl->document_class == "IEEEtran") {
            tex += "\\bibliography{references}\n";
        } else {
            tex += "\\bibliography{references}\n";
        }
    }

    tex += "\n\\end{document}\n";

    // --- Package assembly ---
    BuildPackageFile main_file;
    main_file.path = "main.tex";
    main_file.content = std::move(tex);
    result.package.files.push_back(std::move(main_file));
    result.package.entry_file = "main.tex";

    // Assets: copy referenced assets into the package with .img placeholder
    // names (renderer maps AssetId -> file via asset_files).
    for (const auto& section : doc.body().sections) {
        auto collect = [&](const std::vector<Block>& blocks) {
            for (const auto& block : blocks) {
                if (const auto* fig = std::get_if<Figure>(&block)) {
                    result.package.required_assets.push_back(
                        fig->asset_id.value() + ".img");
                }
            }
        };
        collect(section.blocks);
        for (const auto& sub : section.subsections) {
            collect(sub.blocks);
            for (const auto& subsub : sub.subsubsections) collect(subsub.blocks);
        }
    }

    result.status = RenderResult::Status::Ok;
    return result;
}

}  // namespace pf
