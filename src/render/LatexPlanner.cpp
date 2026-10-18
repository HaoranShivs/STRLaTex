#include "render/LatexPlan.h"

#include "core/ContentHash.h"
#include "core/IdGenerator.h"
#include "core/ProjectPath.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "numbering/LabelRegistry.h"
#include "math/MathGenerator.h"
#include "math/MathValidator.h"
#include <algorithm>
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
std::vector<TheoremEnvironmentDecl> DefaultTheoremDeclarations() {
    return {
        {TheoremKind::Theorem, "theorem", true},       {TheoremKind::Lemma, "lemma", true},
        {TheoremKind::Proposition, "proposition", true}, {TheoremKind::Corollary, "corollary", true},
        {TheoremKind::Definition, "definition", true}, {TheoremKind::Remark, "remark", true},
        {TheoremKind::Example, "example", true},
    };
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
    CommonPlanner(const RenderRequest& request, const TemplateDefinition& definition, bool use_listings,
                  const std::vector<TheoremEnvironmentDecl>& theorem_declarations,
                  CrossReferenceStyle cross_reference = CrossReferenceStyle::Ref)
        : request_(request), definition_(definition), use_listings_(use_listings),
          theorem_declarations_(theorem_declarations), labels_(LabelRegistry::Build(*request.document)),
          cross_reference_(cross_reference) {
        // Validator 会先给出 E-DUPLICATE-LABEL；这里拒绝生成会被 TeX 静默
        // 覆盖的重复 \label，避免“PDF 编译成功但引用指向错误目标”。
        if (!labels_.conflicts().empty()) {
            const auto& conflict = labels_.conflicts().front();
            throw std::runtime_error("Duplicate LaTeX label '" + conflict.label + "'");
        }
    }
    std::string LabelFor(const NodeId& id) const {
        return labels_.LabelFor(id);
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
            } else if (const auto* ref = std::get_if<CrossReference>(&item)) {
                out.push_back(Cmd(cross_reference_ == CrossReferenceStyle::Cleveref ? "cref" : "ref",
                                  {Id(LabelFor(ref->target))}));
            } else if (const auto* footnote = std::get_if<Footnote>(&item)) {
                Nodes body = footnote->body ? Inline(footnote->body->content) : Nodes{};
                out.push_back(Cmd("footnote", std::move(body)));
            }
        }
        return out;
    }
    const TheoremEnvironmentDecl* TheoremDecl(TheoremKind kind) const {
        for (const auto& decl : theorem_declarations_) {
            if (decl.kind == kind)
                return &decl;
        }
        return nullptr;
    }

    static std::string CodeLanguageOption(const std::string& language) {
        static const std::map<std::string, std::string> names = {
            {"python", "Python"}, {"py", "Python"}, {"c", "C"}, {"cpp", "C++"}, {"c++", "C++"},
            {"java", "Java"}, {"javascript", "JavaScript"}, {"js", "JavaScript"}, {"typescript", "TypeScript"},
            {"ts", "TypeScript"}, {"rust", "Rust"}, {"go", "Go"}, {"bash", "bash"}, {"sh", "bash"},
            {"shell", "bash"}, {"sql", "SQL"}, {"json", "JSON"}, {"yaml", "YAML"}, {"xml", "XML"},
            {"html", "HTML"}, {"css", "CSS"}, {"latex", "TeX"}, {"tex", "TeX"}, {"matlab", "Matlab"},
            {"r", "R"}, {"swift", "Swift"}, {"kotlin", "Kotlin"}, {"scala", "Scala"}};
        if (language.empty())
            return {};
        const auto it = names.find(language);
        if (it == names.end())
            return {}; // Validator 报 W-CODE-LANGUAGE；planner 安全降级。
        return it->second;
    }

    static std::string PlainOption(const InlineContent& content) {
        const std::string out = InlineToPlainText(content);
        std::string safe;
        for (char c : out) {
            if (c == '}' || c == '{' || c == ']' || c == '[' || c == '\\' || c == '%' || c == '\n' ||
                c == '\r' || c == '_' || c == '&' || c == '#' || c == '$' || c == '^' || c == '~') {
                safe += ' ';
            } else {
                safe += c;
            }
        }
        return safe;
    }

    Node TheoremPlan(const TheoremBlock& theorem) const {
        const TheoremEnvironmentDecl* decl = TheoremDecl(theorem.kind);
        const std::string name = decl ? decl->latex_name : "theorem";
        if (theorem.kind == TheoremKind::Proof) {
            Node env = Env("proof", Inline(theorem.body));
            if (!theorem.title.empty())
                env.option = PlainOption(theorem.title);
            return env;
        }
        const bool numbered = theorem.numbered && (!decl || decl->numbered);
        Node env = Env(numbered ? name : name + "*", Inline(theorem.body));
        if (!theorem.title.empty())
            env.option = PlainOption(theorem.title);
        if (numbered) {
            // label 必须在环境内、正文之前；编号由 LaTeX 环境负责。
            env.children.insert(env.children.begin(), Cmd("label", {Id(LabelFor(theorem.id))}));
            env.children.insert(env.children.begin(), Lit("\n"));
        }
        return env;
    }

    Node CodePlan(const CodeBlock& code) const {
            const bool listings = use_listings_;
        const char* end_marker = listings ? "\\end{lstlisting}" : "\\end{verbatim}";
        if (code.code.find(end_marker) != std::string::npos)
            throw std::runtime_error(std::string("code block contains ") + end_marker);
        if (!listings) {
            // 基础 runtime 没有 listings.sty：用 verbatim 保留原文，并用模板
            // 导言区声明的计数宏提供 Listing N + label。
            Nodes out;
            Node caption = Leaf(Node::Kind::Command, "paperforgeListing");
            caption.arguments = {{Lit(LabelFor(code.id))}, {Text(PlainOption(code.caption))}};
            out.push_back(std::move(caption));
            out.push_back(Lit("\n"));
            std::string body = code.code;
            if (code.line_numbers) {
                std::string numbered;
                int line = 1;
                size_t start = 0;
                while (start <= body.size()) {
                    const size_t newline = body.find('\n', start);
                    const std::string chunk = body.substr(start, newline == std::string::npos
                                                                  ? std::string::npos
                                                                  : newline - start);
                    numbered += std::to_string(line++) + "  " + chunk;
                    if (newline == std::string::npos)
                        break;
                    numbered += '\n';
                    start = newline + 1;
                }
                body = std::move(numbered);
            }
            Node env = Env("verbatim", {Lit(body)});
            out.push_back(std::move(env));
            Node group = Lit("");
            group.children = std::move(out);
            return group;
        }
        Node env = Env("lstlisting", {Lit(code.code)});
        std::string options;
        const std::string language = CodeLanguageOption(code.language);
        if (!language.empty())
            options += "language=" + language;
        if (code.line_numbers)
            options += (options.empty() ? "" : ",") + std::string("numbers=left");
        if (!code.caption.empty()) {
            options += (options.empty() ? "" : ",") + std::string("caption={") + PlainOption(code.caption) + "}";
        }
        options += (options.empty() ? "" : ",") + std::string("label={") + LabelFor(code.id) + "}";
        env.option = options;
        return env;
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
                          Cmd("label", {Id(LabelFor(fig->id))}),
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
            // 表格始终写 caption + label（WP-03/X-06）：空题注也生成编号，
            // 使交叉引用绝不会指向悬空 label。
            content.push_back(Cmd("caption", Inline(table->caption)));
            content.push_back(Cmd("label", {Id(LabelFor(table->id))}));
            content.push_back(Lit("\n"));
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
        } else if (const auto* list = std::get_if<ListBlock>(&block)) {
            group.source_label = "List";
            Nodes items;
            for (const auto& item : list->items) {
                // \item 是声明而不是带参数的命令；若包装成 Cmd()，生成器会
                // 输出 \item{...}，虽然多数情况下可编译但语义不标准。
                items.push_back(Lit("\\item "));
                Append(items, Inline(item.content));
                items.push_back(Lit("\n"));
            }
            body.push_back(Env(list->style == ListStyle::Numbered ? "enumerate" : "itemize",
                               std::move(items)));
            body.push_back(Lit("\n"));
        } else if (const auto* theorem = std::get_if<TheoremBlock>(&block)) {
            group.source_label = ToString(theorem->kind);
            body.push_back(TheoremPlan(*theorem));
            body.push_back(Lit("\n"));
        } else if (const auto* code = std::get_if<CodeBlock>(&block)) {
            group.source_label = "Code";
            body.push_back(CodePlan(*code));
            body.push_back(Lit("\n"));
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
    const TemplateDefinition& definition_;
    const bool use_listings_ = false;
    const std::vector<TheoremEnvironmentDecl>& theorem_declarations_;
    LabelRegistry labels_;
    CrossReferenceStyle cross_reference_ = CrossReferenceStyle::Ref;
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
        bool has_code = false;
        bool has_theorem = false;
        VisitBlocks(*request.document, [&](const Block& block, const NodeAddress&) {
            if (std::holds_alternative<CodeBlock>(block))
                has_code = true;
            if (std::holds_alternative<TheoremBlock>(block))
                has_theorem = true;
        });
        // 外部模板可能没有声明 code/theorem capability，但项目可能从别的模板
        // 带入了这些块。只要文档里有，就必须生成可编译的支撑声明。
        const bool use_listings = def.capabilities.code_blocks && def.code_listing.package == "listings";
        std::vector<TheoremEnvironmentDecl> theorem_declarations = def.theorem_environments;
        if (theorem_declarations.empty())
            theorem_declarations = DefaultTheoremDeclarations();
        CommonPlanner common(request, def, use_listings, theorem_declarations, def.capabilities.cross_reference);
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
        if (def.capabilities.cross_reference == CrossReferenceStyle::Cleveref &&
            std::none_of(packages.begin(), packages.end(),
                         [](const auto& p) { return p.name == "cleveref"; }))
            packages.push_back({"cleveref", "", false});
        // hyperref 必须在普通包之后；cleveref 必须在 hyperref 之后。
        auto package_rank = [](const std::string& name) {
            if (name == "hyperref")
                return 1;
            if (name == "cleveref")
                return 2;
            return 0;
        };
        std::stable_sort(packages.begin(), packages.end(),
                         [&](const auto& a, const auto& b) {
                             return package_rank(a.name) < package_rank(b.name);
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
        if (has_theorem) {
            if (seen.find("amsthm") == seen.end()) {
                Line(nodes, Cmd("usepackage", {Id("amsthm")}));
                seen.emplace("amsthm", "");
            }
            for (const auto& theorem : theorem_declarations) {
                Node definition = Leaf(Node::Kind::Command, "newtheorem");
                definition.arguments = {{Lit(theorem.latex_name)}, {Text(ToString(theorem.kind))}};
                Line(nodes, std::move(definition));
                // 即使模板默认编号，也声明星号版本，使单个定理可以关闭 numbered。
                Node starred = Leaf(Node::Kind::Command, "newtheorem*");
                starred.arguments = {{Lit(theorem.latex_name + "*")}, {Text(ToString(theorem.kind))}};
                Line(nodes, std::move(starred));
            }
        }
        if (has_code) {
            if (use_listings) {
                if (seen.find("listings") == seen.end())
                    Line(nodes, Cmd("usepackage", {Id("listings")}));
            } else {
                Node counter = Leaf(Node::Kind::Command, "newcounter");
                counter.arguments = {{Lit("listing")}};
                Line(nodes, std::move(counter));
                // \newcommand 的 [2] 必须位于命令名之后、宏体之前；不能
                // 用 Cmd()（它会把每个参数都包成 {...}）。
                Line(nodes,
                     Lit("\\newcommand{\\paperforgeListing}[2]"
                         "{\\refstepcounter{listing}\\label{#1}\\textbf{Listing \\thelisting: #2}}"));
                // IEEEtran 把 \ttfamily 指向 Courier，而自带 runtime 没有 pcr
                // 字型；verbatim 默认使用 \verbatim@font，显式改回 cmtt。
                Line(nodes, Lit("\\makeatletter\\renewcommand{\\verbatim@font}"
                                "{\\fontfamily{cmtt}\\selectfont}\\makeatother"));
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
            nodes.push_back(Cmd("label", {Id(common.LabelFor(section.id))}));
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
