#include "validation/Validator.h"

#include <algorithm>
#include <atomic>
#include <optional>

#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "math/MathValidator.h"
#include "template/TemplateRegistry.h"

namespace pf {

namespace {

// Validator 运行在持有 snapshot 的线程上——实际是 build worker，
// 直接校验时也可能是应用线程——因此诊断 id 计数器必须是原子的。
// 由 ThreadSanitizer 发现。
std::atomic<std::uint64_t> g_counter{0};

Diagnostic MakeDiag(ProjectRevision rev, DiagnosticSeverity severity, const std::string& code,
                    const std::string& message, DiagnosticLocation loc) {
    Diagnostic d;
    d.id = MakeDiagnosticId("val", g_counter.fetch_add(1) + 1);
    d.source = DiagnosticSource::Validation;
    d.severity = severity;
    d.code = code;
    d.message = message;
    d.revision = rev;
    d.location = std::move(loc);
    return d;
}

} // namespace

ValidationResult Validator::Validate(const ValidationInput& input) const {
    ValidationResult result;
    result.snapshot_id = input.snapshot_id;
    result.revision = input.revision;
    result.can_render = true;

    if (!input.document) {
        result.can_render = false;
        result.diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, "E-NO-DOC",
                                              "no document provided", DiagnosticLocation::ForProject()));
        return result;
    }

    ValidateSemantic(*input.document, input, &result);
    ValidateTemplate(*input.document, input, &result);

    // 语义问题只是诊断，不阻断渲染（V1：仍尽力生成 PDF；问题显示在
    // Problems 面板）。
    result.can_render = true;
    return result;
}

void Validator::ValidateSemantic(const Document& doc, const ValidationInput& input, ValidationResult* result) const {
    // 标题为空
    if (InlineIsBlank(doc.front_matter().title)) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-EMPTY-TITLE",
                                               "paper title is empty", DiagnosticLocation::ForProject()));
    }

    // 引用会解析到参考文献；数学公式体遵循 LaTeX 输入边界（设计 §5）。
    // 行内数学没有自己的 node id，因此问题报到拥有它的块上。
    //
    // 引用校验（Citation 方案 §9）：被引用但不在参考文献中的 key 是错误——
    // 绝不静默丢弃——而项目没有参考文献却出现了引用时，只在项目级报告一次，
    // 而不是为每个 key 各报一个噪声错误。
    bool document_has_citations = false;
    auto check_inline = [&](const InlineContent& content, const std::optional<NodeId>& owner) {
        const DiagnosticLocation location =
            owner ? DiagnosticLocation::ForNode(*owner) : DiagnosticLocation::ForProject();
        for (const auto& node : content) {
            if (const auto* cit = std::get_if<Citation>(&node)) {
                if (!cit->keys.empty())
                    document_has_citations = true;
                if (input.bibliography_keys.empty())
                    continue;
                for (const auto& key : cit->keys) {
                    bool found = std::find(input.bibliography_keys.begin(), input.bibliography_keys.end(), key) !=
                                 input.bibliography_keys.end();
                    if (!found) {
                        result->diagnostics.push_back(MakeDiag(
                            input.revision, DiagnosticSeverity::Error, "E-CITATION-UNKNOWN-KEY",
                            "citation key not in bibliography: " + key, DiagnosticLocation::ForCitationKey(key)));
                    }
                }
            } else if (const auto* math = std::get_if<InlineMath>(&node)) {
                const MathValidation validation = ValidateMath(math->expression.latex, MathFlavor::Inline);
                if (validation.invalid()) {
                    result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, validation.code,
                                                           validation.error, location));
                } else if (validation.pending()) {
                    result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning,
                                                           "W-EMPTY-INLINE-MATH", "inline math is empty", location));
                }
            }
        }
    };

    // 块级语义规则。一次遍历完成；标题嵌套关系在 DocumentTraversal 中，
    // 不在这里。
    VisitBlocks(doc, [&](const Block& block, const NodeAddress& address) {
        if (const auto* para = std::get_if<Paragraph>(&block)) {
            check_inline(para->content, address.node);
        } else if (const auto* fig = std::get_if<Figure>(&block)) {
            if (fig->asset_id.empty()) {
                result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, "E-MISSING-ASSET",
                                                       "figure has no asset", DiagnosticLocation::ForNode(fig->id)));
            }
            check_inline(fig->caption, fig->id);
        } else if (const auto* table = std::get_if<Table>(&block)) {
            if (!table->IsRectangular()) {
                result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, "E-NON-RECT-TABLE",
                                                       "table is not rectangular",
                                                       DiagnosticLocation::ForNode(table->id)));
            }
            check_inline(table->caption, table->id);
        } else if (const auto* eq = std::get_if<EquationBlock>(&block)) {
            const MathValidation validation = ValidateMath(eq->expression.latex, MathFlavor::Display);
            if (validation.invalid()) {
                result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, validation.code,
                                                       validation.error, DiagnosticLocation::ForNode(eq->id)));
            } else if (validation.pending()) {
                result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-EMPTY-EQUATION",
                                                       "display equation is empty",
                                                       DiagnosticLocation::ForNode(eq->id)));
            }
        }
    });

    // 悬空的交叉引用：一次遍历覆盖所有行内内容（段落正文与题注，
    // 以及标题/摘要）。
    auto check_ref = [&](const InlineContent& content) {
        for (const auto& node : content) {
            if (const auto* ref = std::get_if<CrossReference>(&node)) {
                if (!doc.ContainsNode(ref->target)) {
                    result->diagnostics.push_back(
                        MakeDiag(input.revision, DiagnosticSeverity::Error, "E-MISSING-XREF-TARGET",
                                 "cross reference target no longer exists: " + ref->target.value(),
                                 DiagnosticLocation::ForNode(ref->target)));
                }
            }
        }
    };
    VisitInlineContent(doc, [&](const InlineContent& content, const NodeAddress&) { check_ref(content); });

    // 有引用，但项目完全没有参考文献：只报一个项目级错误（Citation 方案
    // §9）。上面按 key 的检查正是为这种情况跳过，以保持 Problems 面板可读。
    if (document_has_citations && input.bibliography_keys.empty()) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, "E-CITATION-NO-BIBLIOGRAPHY",
                                               "document cites references but the project has no bibliography "
                                               "(import a .bib file)",
                                               DiagnosticLocation::ForProject()));
    }
}

void Validator::ValidateTemplate(const Document& doc, const ValidationInput& input, ValidationResult* result) const {
    const auto* def = TemplateRegistry::Instance().Find(input.template_id);
    if (!def) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Error, "E-TEMPLATE-UNKNOWN",
                                               "unknown template: " + input.template_id,
                                               DiagnosticLocation::ForProject()));
        return;
    }

    // 模板专用的必填字段检查（同时驱动编辑器的字段提示与 Problems
    // 面板）。
    const auto& fm = doc.front_matter();
    const auto& req = def->required;
    if (req.title && InlineIsBlank(fm.title)) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-REQ-TITLE",
                                               "template requires a title", DiagnosticLocation::ForProject()));
    }
    if (req.authors && fm.authors.empty()) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-REQ-AUTHORS",
                                               "template requires at least one author",
                                               DiagnosticLocation::ForProject()));
    }
    if (req.affiliations && fm.affiliations.empty()) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-REQ-AFFILIATIONS",
                                               "template requires at least one affiliation",
                                               DiagnosticLocation::ForProject()));
    }
    if (req.author_affiliations) {
        for (const auto& author : fm.authors) {
            if (author.affiliations.empty()) {
                result->diagnostics.push_back(
                    MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-REQ-AUTHOR-AFFILIATION",
                             "author has no affiliation: " + author.name, DiagnosticLocation::ForProject()));
            }
        }
    }
    if (req.abstract_text && (!fm.abstract_text || InlineIsBlank(*fm.abstract_text))) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-REQ-ABSTRACT",
                                               "template requires an abstract", DiagnosticLocation::ForProject()));
    }
    if (req.keywords && fm.keywords.empty()) {
        result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-REQ-KEYWORDS",
                                               "template requires keywords", DiagnosticLocation::ForProject()));
    }

    // 标题深度能力（方案 §9）：比模板支持层级更深的标题不会渲染成独立层级。
    const int max_depth = def->capabilities.max_heading_depth;
    if (max_depth > 0 && max_depth < 3) {
        VisitHeadings(doc, [&](const NodeAddress& address) {
            if (address.depth() <= max_depth)
                return;
            result->diagnostics.push_back(MakeDiag(input.revision, DiagnosticSeverity::Warning, "W-HEADING-DEPTH",
                                                   std::string("template supports ") + std::to_string(max_depth) +
                                                       " heading levels: " + ToString(address.kind),
                                                   DiagnosticLocation::ForNode(address.node)));
        });
    }
}

} // namespace pf
