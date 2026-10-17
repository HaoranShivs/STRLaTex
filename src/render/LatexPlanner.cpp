#include "render/LatexPlan.h"

#include "core/ContentHash.h"
#include "core/IdGenerator.h"
#include "core/ProjectPath.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "math/MathGenerator.h"
#include "math/MathValidator.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace pf {
namespace {
using Node = LatexNode;
using Nodes = std::vector<Node>;
Node Leaf(Node::Kind kind, std::string value) {
    Node n;
    n.kind = kind;
    n.value = std::move(value);
    return n;
}
Node Lit(std::string value) {
    return Leaf(Node::Kind::Literal, std::move(value));
}
Node Text(std::string value) {
    return Leaf(Node::Kind::Text, std::move(value));
}
Node Id(std::string value) {
    if (value.empty() ||
        value.find_first_of("\\{}%#&$^~ \t\r\n") != std::string::npos)
        throw std::runtime_error("Unsafe LaTeX identifier: " + value);
    return Leaf(Node::Kind::Identifier, std::move(value));
}
Node Cmd(std::string name, Nodes arg) {
    Node n = Leaf(Node::Kind::Command, std::move(name));
    n.arguments.push_back(std::move(arg));
    return n;
}
Node Env(std::string name, Nodes children) {
    Node n = Leaf(Node::Kind::Environment, std::move(name));
    n.children = std::move(children);
    return n;
}
void Append(Nodes& to, Nodes from) {
    to.insert(to.end(), std::make_move_iterator(from.begin()),
              std::make_move_iterator(from.end()));
}
void Line(Nodes& out, Node node) {
    out.push_back(std::move(node));
    out.push_back(Lit("\n"));
}
Node Origin(Node n, const std::string& label, const std::string& component) {
    n.source_label = label;
    n.origin = component;
    return n;
}
std::string Label(const std::string& input) {
    std::string out;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == ':' || c == '.' || c == '_' || c == '-')
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '-';
    }
    return out;
}
JsonValue NodeJson(const Node& node) {
    JsonArray args, children;
    for (const auto& arg : node.arguments) {
        JsonArray a;
        for (const auto& n : arg)
            a.push_back(NodeJson(n));
        args.push_back(a);
    }
    for (const auto& child : node.children)
        children.push_back(NodeJson(child));
    return JsonObject{{"kind", static_cast<int>(node.kind)},
                      {"value", node.value},
                      {"option", node.option},
                      {"arguments", args},
                      {"children", children},
                      {"node", node.source_node.value()},
                      {"label", node.source_label},
                      {"origin", node.origin}};
}

class CommonPlanner {
  public:
    explicit CommonPlanner(const RenderRequest& request) : request_(request) {
        VisitBlocks(
            *request.document, [&](const Block& block, const NodeAddress&) {
                if (const auto* eq = std::get_if<EquationBlock>(&block)) {
                    auto label = Label(eq->label);
                    labels_[eq->id.value()] =
                        label.empty() ? eq->id.value() : label;
                }
            });
    }
    std::string LabelFor(const NodeId& id) const {
        auto it = labels_.find(id.value());
        return it == labels_.end() ? id.value() : it->second;
    }
    std::string Asset(const AssetId& id) const {
        auto it = request_.asset_files.find(id.value());
        auto path =
            "assets/" + (it == request_.asset_files.end() ? id.value() + ".img"
                                                          : it->second);
        if (!ProjectRelativePath::Parse(path) ||
            path.find_first_of("\\{}%#$^~\r\n") != std::string::npos)
            throw std::runtime_error("Unsafe figure resource path");
        return path;
    }
    Nodes Inline(const InlineContent& content) const {
        Nodes out;
        for (const auto& item : content) {
            if (const auto* text = std::get_if<TextRun>(&item)) {
                Node node = Text(text->text);
                if (HasMark(text->marks, TextMark::Emphasis))
                    node = Cmd("emph", {std::move(node)});
                if (HasMark(text->marks, TextMark::Strong))
                    node = Cmd("textbf", {std::move(node)});
                out.push_back(std::move(node));
            } else if (const auto* math = std::get_if<InlineMath>(&item)) {
                CheckMath(math->expression.latex, MathFlavor::Inline);
                out.push_back(
                    Leaf(Node::Kind::InlineMath, math->expression.latex));
            } else if (const auto* cite = std::get_if<Citation>(&item)) {
                Nodes keys;
                for (const auto& key : cite->keys) {
                    if (!keys.empty())
                        keys.push_back(Lit(","));
                    keys.push_back(Id(key));
                }
                out.push_back(Cmd(
                    cite->mode == CitationMode::Narrative ? "citet" : "citep",
                    std::move(keys)));
            } else if (const auto* ref = std::get_if<CrossReference>(&item))
                out.push_back(Cmd("ref", {Id(LabelFor(ref->target))}));
        }
        return out;
    }
    Node BlockPlan(const Block& block) const {
        Node group = Lit("");
        group.source_node =
            std::visit([](const auto& b) { return b.id; }, block);
        Nodes body;
        if (const auto* para = std::get_if<Paragraph>(&block)) {
            group.source_label = "Text";
            body = Inline(para->content);
            if (!body.empty())
                body.push_back(Lit("\n\n"));
        } else if (const auto* fig = std::get_if<Figure>(&block)) {
            group.source_label = "Figure";
            static constexpr const char* widths[] = {"0.25", "0.5", "0.75",
                                                     "1.0"};
            const auto w = static_cast<std::size_t>(fig->width);
            if (w >= 4)
                throw std::runtime_error("Unknown figure width");
            Node image =
                Cmd("includegraphics",
                    {Leaf(Node::Kind::AssetPath, Asset(fig->asset_id))});
            image.option = std::string("width=") + widths[w] + "\\linewidth";
            Nodes content{Lit("\\centering\n"),
                          std::move(image),
                          Lit("\n"),
                          Cmd("caption", Inline(fig->caption)),
                          Cmd("label", {Id(fig->id.value())}),
                          Lit("\n")};
            auto env = Env(fig->span == FigureSpan::DoubleColumn ? "figure*"
                                                                 : "figure",
                           std::move(content));
            env.option = "htbp";
            body = {std::move(env), Lit("\n")};
        } else if (const auto* table = std::get_if<Table>(&block)) {
            group.source_label = "Table";
            std::string cols;
            for (const auto& col : table->columns)
                cols += col.alignment == ColumnAlignment::Left     ? 'l'
                        : col.alignment == ColumnAlignment::Center ? 'c'
                                                                   : 'r';
            Nodes rows;
            for (std::size_t r = 0; r < table->cells.size(); ++r) {
                for (std::size_t c = 0; c < table->cells[r].size(); ++c) {
                    if (c)
                        rows.push_back(Lit(" & "));
                    Append(rows, Inline(table->cells[r][c].content));
                }
                rows.push_back(Lit(" \\\\\n"));
                if (r == 0 && table->has_header_row)
                    rows.push_back(Lit("\\midrule\n"));
            }
            auto tabular = Env("tabular", std::move(rows));
            tabular.arguments = {{Lit(cols)}};
            Nodes content{Lit("\\centering\n")};
            if (!InlineIsBlank(table->caption)) {
                content.push_back(Cmd("caption", Inline(table->caption)));
                content.push_back(Cmd("label", {Id(table->id.value())}));
                content.push_back(Lit("\n"));
            }
            content.push_back(std::move(tabular));
            auto env = Env("table", std::move(content));
            env.option = "htbp";
            body = {std::move(env), Lit("\n")};
        } else if (const auto* eq = std::get_if<EquationBlock>(&block)) {
            group.source_label = "Equation";
            CheckMath(eq->expression.latex, MathFlavor::Display);
            const auto label = LabelFor(eq->id);
            if (eq->numbered)
                (void)Id(label);
            body.push_back(Leaf(
                Node::Kind::DisplayMath,
                GenerateDisplayMath(eq->expression, eq->numbered,
                                    eq->numbered ? label : std::string{})));
        }
        // A zero-syntax group retains provenance without manufacturing an
        // environment.
        group.children = std::move(body);
        return group;
    }
    static void CheckMath(const std::string& value, MathFlavor flavor) {
        const auto validation = ValidateMath(value, flavor);
        if (validation.invalid())
            throw std::runtime_error(validation.code + ": " + validation.error);
    }

  private:
    const RenderRequest& request_;
    std::map<std::string, std::string> labels_;
};

std::vector<std::size_t> Slots(const FrontMatter& fm, const Author& author) {
    std::vector<std::size_t> slots;
    for (std::size_t i = 0; i < fm.affiliations.size(); ++i)
        if (std::find(author.affiliations.begin(), author.affiliations.end(),
                      fm.affiliations[i].id) != author.affiliations.end())
            slots.push_back(i + 1);
    return slots;
}
Nodes Abstract(const FrontMatter& fm, const CommonPlanner& common,
               const std::string& component) {
    Nodes out;
    if (fm.abstract_text && !InlineIsBlank(*fm.abstract_text)) {
        auto body = common.Inline(*fm.abstract_text);
        body.push_back(Lit("\n"));
        out.push_back(
            Origin(Env("abstract", std::move(body)), "Abstract", component));
        out.push_back(Lit("\n"));
    }
    return out;
}
Nodes Keywords(const FrontMatter& fm, const std::string& component) {
    if (fm.keywords.empty())
        return {};
    Nodes body;
    for (const auto& keyword : fm.keywords) {
        if (!body.empty())
            body.push_back(
                Lit(component == "elsevier-cas-v1" ? " \\sep " : ", "));
        body.push_back(Text(keyword));
    }
    body.push_back(Lit("\n"));
    Node node;
    if (component == "article-v1") {
        node = Lit("\\noindent\\textbf{Keywords:} ");
        node.children = std::move(body);
    } else
        node =
            Env(component == "ieee-conference-v1" ? "IEEEkeywords" : "keywords",
                std::move(body));
    return {Origin(std::move(node), "Keywords", component), Lit("\n")};
}

// Complete frontmatter components own order as well as syntax. No emitter
// override.
Nodes ArticleFrontmatter(const FrontMatter& fm, const CommonPlanner& common) {
    Nodes out;
    if (!InlineIsBlank(fm.title))
        Line(out, Origin(Cmd("title", common.Inline(fm.title)), "Title",
                         "article-v1"));
    Nodes authors, notes;
    for (const auto& author : fm.authors) {
        if (!authors.empty())
            authors.push_back(Lit(" \\and "));
        authors.push_back(Text(author.name));
        auto slots = Slots(fm, author);
        for (std::size_t s = 0; s < slots.size(); ++s) {
            if (s)
                authors.push_back(Lit(","));
            authors.push_back(
                Cmd("textsuperscript", {Id(std::to_string(slots[s]))}));
        }
    }
    for (std::size_t i = 0; i < fm.affiliations.size(); ++i) {
        if (!notes.empty())
            notes.push_back(Lit(" \\\\ "));
        notes.push_back(Cmd("textsuperscript", {Id(std::to_string(i + 1))}));
        notes.push_back(Text(" " + fm.affiliations[i].name));
    }
    for (const auto& author : fm.authors)
        if (author.email && !author.email->empty()) {
            if (!notes.empty())
                notes.push_back(Lit(" \\\\ "));
            notes.push_back(Text("Email: " + *author.email));
        }
    if (!authors.empty()) {
        if (!notes.empty())
            authors.push_back(Cmd("thanks", std::move(notes)));
        Line(out, Origin(Cmd("author", std::move(authors)),
                         "Authors and affiliations", "article-v1"));
    }
    Line(out, Cmd("date", {}));
    out.push_back(Lit("\\maketitle\n\n"));
    Append(out, Abstract(fm, common, "article-v1"));
    Append(out, Keywords(fm, "article-v1"));
    return out;
}
Nodes IeeeFrontmatter(const FrontMatter& fm, const CommonPlanner& common) {
    Nodes out, authors, affiliations;
    if (!InlineIsBlank(fm.title))
        Line(out, Origin(Cmd("title", common.Inline(fm.title)), "Title",
                         "ieee-conference-v1"));
    for (const auto& author : fm.authors) {
        if (!authors.empty())
            authors.push_back(Text(", "));
        authors.push_back(Text(author.name));
        for (auto slot : Slots(fm, author))
            authors.push_back(
                Cmd("IEEEauthorrefmark", {Id(std::to_string(slot))}));
    }
    for (std::size_t i = 0; i < fm.affiliations.size(); ++i) {
        if (!affiliations.empty())
            affiliations.push_back(Lit(" \\\\ "));
        affiliations.push_back(
            Cmd("IEEEauthorrefmark", {Id(std::to_string(i + 1))}));
        affiliations.push_back(Text(fm.affiliations[i].name));
    }
    for (const auto& author : fm.authors)
        if (author.email && !author.email->empty()) {
            if (!affiliations.empty())
                affiliations.push_back(Lit(" \\\\ "));
            affiliations.push_back(Text(*author.email));
        }
    if (!authors.empty()) {
        Nodes block{Cmd("IEEEauthorblockN", std::move(authors)), Lit("\n")};
        if (!affiliations.empty())
            block.push_back(Cmd("IEEEauthorblockA", std::move(affiliations)));
        Line(out, Origin(Cmd("author", std::move(block)),
                         "Authors and affiliations", "ieee-conference-v1"));
    }
    out.push_back(Lit("\\maketitle\n\n"));
    Append(out, Abstract(fm, common, "ieee-conference-v1"));
    Append(out, Keywords(fm, "ieee-conference-v1"));
    return out;
}
Nodes CasFrontmatter(const FrontMatter& fm, const CommonPlanner& common) {
    Nodes out;
    Line(out, Origin(Cmd("shorttitle", common.Inline(fm.title)), "Short title",
                     "elsevier-cas-v1"));
    Line(out,
         Origin(Cmd("shortauthors",
                    {Text(fm.authors.empty()
                              ? ""
                              : fm.authors.front().name +
                                    (fm.authors.size() > 1 ? " et al." : ""))}),
                "Short authors", "elsevier-cas-v1"));
    auto title = Cmd("title", common.Inline(fm.title));
    title.option = "mode=title";
    Line(out, Origin(std::move(title), "Title", "elsevier-cas-v1"));
    for (const auto& author : fm.authors) {
        auto node = Cmd("author", {Text(author.name)});
        for (auto slot : Slots(fm, author)) {
            if (!node.option.empty())
                node.option += ',';
            node.option += std::to_string(slot);
        }
        Line(out, Origin(std::move(node), "Author", "elsevier-cas-v1"));
        if (author.email && !author.email->empty())
            Line(out, Origin(Cmd("ead", {Text(*author.email)}), "Author email",
                             "elsevier-cas-v1"));
    }
    for (std::size_t i = 0; i < fm.affiliations.size(); ++i) {
        auto node =
            Cmd("affiliation", {Lit("organization={"),
                                Text(fm.affiliations[i].name), Lit("}")});
        node.option = std::to_string(i + 1);
        Line(out, Origin(std::move(node), "Affiliation", "elsevier-cas-v1"));
    }
    Append(out, Abstract(fm, common, "elsevier-cas-v1"));
    Append(out, Keywords(fm, "elsevier-cas-v1"));
    out.push_back(Lit("\\maketitle\n\n"));
    return out;
}
} // namespace

PlanResult LatexPlanner::Plan(const RenderRequest& request,
                              const TemplateDefinition& def) const {
    PlanResult result;
    try {
        if (!request.document)
            throw std::runtime_error("No document to plan");
        if (def.api_version != 1 ||
            def.citation_contract != "natbib-numeric-first-use-v1" ||
            !def.binding.options.empty())
            throw std::runtime_error(
                "Unsupported template API, options or citation contract");
        if (def.toolchain.bibliography_engine != BibliographyEngine::BibTex)
            throw std::runtime_error(
                "The numeric citation component requires BibTeX");
        CommonPlanner common(request);
        auto plan =
            std::shared_ptr<LatexGenerationPlan>(new LatexGenerationPlan());
        auto& nodes = plan->nodes_;
        auto& package = plan->package_;
        auto cls = Cmd("documentclass", {Id(def.document_class)});
        for (const auto& option : def.class_options) {
            (void)Id(option);
            if (!cls.option.empty())
                cls.option += ',';
            cls.option += option;
        }
        Line(nodes, Origin(std::move(cls), "Template class", def.frontmatter));
        std::vector<LatexPackageRequirement> packages = def.packages;
        // Core declares semantic dependencies; the adapter can satisfy them via
        // its class.
        for (const auto& name : {"amsmath", "amssymb", "graphicx", "booktabs"})
            if (std::none_of(packages.begin(), packages.end(),
                             [&](const auto& p) { return p.name == name; }))
                packages.push_back({name, "", false});
        // Citations may appear before a bibliography is imported; keep commands
        // defined.
        packages.push_back({"natbib", "numbers,sort&compress", false});
        std::stable_sort(packages.begin(), packages.end(),
                         [](const auto& a, const auto& b) {
                             return (a.name == "hyperref" ? 1 : 0) <
                                    (b.name == "hyperref" ? 1 : 0);
                         });
        std::map<std::string, std::string> seen;
        JsonArray package_audit;
        for (const auto& p : packages) {
            (void)Id(p.name);
            if (auto it = seen.find(p.name); it != seen.end()) {
                if (it->second != p.options)
                    throw std::runtime_error("Conflicting package options: " +
                                             p.name);
                continue;
            }
            seen.emplace(p.name, p.options);
            package_audit.push_back(
                JsonObject{{"name", p.name},
                           {"options", p.options},
                           {"providedByClass", p.provided_by_class}});
            if (!p.provided_by_class) {
                auto node = Cmd("usepackage", {Id(p.name)});
                node.option = p.options;
                Line(nodes, std::move(node));
            }
        }
        nodes.push_back(Lit("\n\\begin{document}\n\n"));
        const auto& fm = request.document->front_matter();
        if (def.frontmatter == "article-v1")
            Append(nodes, ArticleFrontmatter(fm, common));
        else if (def.frontmatter == "ieee-conference-v1")
            Append(nodes, IeeeFrontmatter(fm, common));
        else if (def.frontmatter == "elsevier-cas-v1")
            Append(nodes, CasFrontmatter(fm, common));
        else
            throw std::runtime_error("Unknown frontmatter component: " +
                                     def.frontmatter);
        auto heading = [&](const auto& section, const char* name) {
            auto title = Cmd(name, common.Inline(section.title));
            title.source_node = section.id;
            title.source_label = "Heading";
            nodes.push_back(std::move(title));
            nodes.push_back(Cmd("label", {Id(section.id.value())}));
            nodes.push_back(Lit("\n\n"));
            for (const auto& block : section.blocks)
                nodes.push_back(common.BlockPlan(block));
        };
        for (const auto& section : request.document->body().sections) {
            heading(section, "section");
            for (const auto& sub : section.subsections) {
                if (def.capabilities.max_heading_depth < 2)
                    throw std::runtime_error(
                        "Template does not support subsection headings");
                heading(sub, "subsection");
                for (const auto& subsub : sub.subsubsections) {
                    if (def.capabilities.max_heading_depth < 3)
                        throw std::runtime_error(
                            "Template does not support subsubsection headings");
                    heading(subsub, "subsubsection");
                }
            }
        }
        if (!request.bibliography_bibtex.empty()) {
            Line(nodes, Cmd("bibliographystyle", {Id(def.bibliography_style)}));
            Line(nodes, Cmd("bibliography", {Id("references")}));
            package.files.push_back(
                {"references.bib", request.bibliography_bibtex, false});
            package.bibliography_files.push_back("references.bib");
        }
        nodes.push_back(Lit("\n\\end{document}\n"));
        VisitBlocks(*request.document,
                    [&](const Block& block, const NodeAddress&) {
                        if (const auto* figure = std::get_if<Figure>(&block))
                            package.required_assets.push_back(
                                common.Asset(figure->asset_id));
                    });
        JsonObject resources;
        for (const auto& [path, bytes] : def.resources) {
            if (!ProjectRelativePath::Parse(path) || path == "main.tex" ||
                path == "references.bib" || path == "render-plan.json" ||
                path == "template.lock.json")
                throw std::runtime_error(
                    "Template resource collides with a generated file: " +
                    path);
            package.files.push_back({path, bytes, false});
            package.template_files.push_back(path);
            resources[path] = Sha256(bytes);
        }
        package.toolchain = {def.toolchain.engine,
                             request.bibliography_bibtex.empty()
                                 ? BibliographyEngine::None
                                 : def.toolchain.bibliography_engine};
        package.requires_isolation = def.external;
        JsonArray ir;
        for (const auto& node : nodes)
            ir.push_back(NodeJson(node));
        JsonObject files;
        for (const auto& file : package.files)
            files[file.path] = Sha256(file.content);
        JsonValue audit =
            JsonObject{{"planVersion", 1},
                       {"plannerVersion", "1"},
                       {"emitterVersion", "1"},
                       {"template", TemplateBindingToJson(def.binding)},
                       {"frontmatter", def.frontmatter},
                       {"citationContract", def.citation_contract},
                       {"engine", ToString(package.toolchain.engine)},
                       {"bibliographyEngine",
                        ToString(package.toolchain.bibliography_engine)},
                       {"packages", package_audit},
                       {"files", files},
                       {"requiredAssets", JsonArray{}},
                       {"nodes", ir}};
        JsonArray assets;
        for (const auto& asset : package.required_assets)
            assets.push_back(asset);
        audit.set("requiredAssets", assets);
        package.plan_digest = Sha256(audit.Dump(0));
        audit.set("digest", package.plan_digest);
        plan->audit_json_ = audit.Dump();
        package.files.push_back(
            {"template.lock.json",
             JsonValue(
                 JsonObject{{"template", TemplateBindingToJson(def.binding)},
                            {"planDigest", package.plan_digest},
                            {"resources", resources},
                            {"frontmatter", def.frontmatter},
                            {"citationContract", def.citation_contract}})
                 .Dump(),
             false});
        result.plan = std::move(plan);
    } catch (const std::exception& error) {
        Diagnostic d;
        d.id = MakeDiagnosticId("plan", 1);
        d.build_id = BuildId(request.build_id);
        d.revision = request.revision;
        d.source = DiagnosticSource::Renderer;
        d.severity = DiagnosticSeverity::Error;
        d.location = DiagnosticLocation::ForProject();
        d.code = "E-LATEX-PLAN";
        d.message = error.what();
        result.diagnostics.push_back(std::move(d));
    }
    return result;
}
} // namespace pf
