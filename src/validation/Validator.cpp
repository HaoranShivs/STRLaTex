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

// Validator runs on whatever thread owns the snapshot - in practice the build
// worker, but also the application thread for a direct validation - so the
// diagnostic id counter must be atomic. Found by ThreadSanitizer.
std::atomic<std::uint64_t> g_counter{0};

Diagnostic MakeDiag(ProjectRevision rev, DiagnosticSeverity severity,
                    const std::string& code, const std::string& message,
                    DiagnosticLocation loc) {
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

}  // namespace

ValidationResult Validator::Validate(const ValidationInput& input) const {
    ValidationResult result;
    result.snapshot_id = input.snapshot_id;
    result.revision = input.revision;
    result.can_render = true;

    if (!input.document) {
        result.can_render = false;
        result.diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Error, "E-NO-DOC",
            "no document provided", DiagnosticLocation::ForProject()));
        return result;
    }

    ValidateSemantic(*input.document, input, &result);
    ValidateTemplate(*input.document, input, &result);

    // Semantic issues are diagnostics, not render blockers (V1: best-effort
    // PDF generation continues; problems show in the Problems panel).
    result.can_render = true;
    return result;
}

void Validator::ValidateSemantic(const Document& doc, const ValidationInput& input,
                                 ValidationResult* result) const {
    // Empty title
    if (InlineIsBlank(doc.front_matter().title)) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Warning, "W-EMPTY-TITLE",
            "paper title is empty", DiagnosticLocation::ForProject()));
    }

    // Citations resolve into the bibliography; math bodies obey the LaTeX
    // input boundary (design §5). Inline math has no node id of its own, so a
    // problem is reported against the block that owns it.
    //
    // Citation validation (citation plan §9): a cited key that is not in the
    // bibliography is an error - never silently dropped - and a document that
    // cites at all while the project has no bibliography reports that once at
    // project level instead of one noise error per key.
    bool document_has_citations = false;
    auto check_inline = [&](const InlineContent& content,
                            const std::optional<NodeId>& owner) {
        const DiagnosticLocation location =
            owner ? DiagnosticLocation::ForNode(*owner)
                  : DiagnosticLocation::ForProject();
        for (const auto& node : content) {
            if (const auto* cit = std::get_if<Citation>(&node)) {
                if (!cit->keys.empty()) document_has_citations = true;
                if (input.bibliography_keys.empty()) continue;
                for (const auto& key : cit->keys) {
                    bool found = std::find(input.bibliography_keys.begin(),
                                           input.bibliography_keys.end(),
                                           key) != input.bibliography_keys.end();
                    if (!found) {
                        result->diagnostics.push_back(MakeDiag(
                            input.revision, DiagnosticSeverity::Error, "E-CITATION-UNKNOWN-KEY",
                            "citation key not in bibliography: " + key,
                            DiagnosticLocation::ForCitationKey(key)));
                    }
                }
            } else if (const auto* math = std::get_if<InlineMath>(&node)) {
                const MathValidation validation =
                    ValidateMath(math->expression.latex, MathFlavor::Inline);
                if (validation.invalid()) {
                    result->diagnostics.push_back(MakeDiag(
                        input.revision, DiagnosticSeverity::Error,
                        validation.code, validation.error, location));
                } else if (validation.pending()) {
                    result->diagnostics.push_back(MakeDiag(
                        input.revision, DiagnosticSeverity::Warning,
                        "W-EMPTY-INLINE-MATH", "inline math is empty",
                        location));
                }
            }
        }
    };

    // Block-level semantic rules. One traversal; the heading nesting lives in
    // DocumentTraversal, not here.
    VisitBlocks(doc, [&](const Block& block, const NodeAddress& address) {
        if (const auto* para = std::get_if<Paragraph>(&block)) {
            check_inline(para->content, address.node);
        } else if (const auto* fig = std::get_if<Figure>(&block)) {
            if (fig->asset_id.empty()) {
                result->diagnostics.push_back(MakeDiag(
                    input.revision, DiagnosticSeverity::Error, "E-MISSING-ASSET",
                    "figure has no asset", DiagnosticLocation::ForNode(fig->id)));
            }
            check_inline(fig->caption, fig->id);
        } else if (const auto* table = std::get_if<Table>(&block)) {
            if (!table->IsRectangular()) {
                result->diagnostics.push_back(MakeDiag(
                    input.revision, DiagnosticSeverity::Error, "E-NON-RECT-TABLE",
                    "table is not rectangular",
                    DiagnosticLocation::ForNode(table->id)));
            }
            check_inline(table->caption, table->id);
        } else if (const auto* eq = std::get_if<EquationBlock>(&block)) {
            const MathValidation validation =
                ValidateMath(eq->expression.latex, MathFlavor::Display);
            if (validation.invalid()) {
                result->diagnostics.push_back(MakeDiag(
                    input.revision, DiagnosticSeverity::Error, validation.code,
                    validation.error, DiagnosticLocation::ForNode(eq->id)));
            } else if (validation.pending()) {
                result->diagnostics.push_back(MakeDiag(
                    input.revision, DiagnosticSeverity::Warning,
                    "W-EMPTY-EQUATION", "display equation is empty",
                    DiagnosticLocation::ForNode(eq->id)));
            }
        }
    });

    // Dangling cross references: every inline run (paragraph bodies and
    // captions, plus the title/abstract) in one traversal.
    auto check_ref = [&](const InlineContent& content) {
        for (const auto& node : content) {
            if (const auto* ref = std::get_if<CrossReference>(&node)) {
                if (!doc.ContainsNode(ref->target)) {
                    result->diagnostics.push_back(MakeDiag(
                        input.revision, DiagnosticSeverity::Error,
                        "E-MISSING-XREF-TARGET",
                        "cross reference target no longer exists: " +
                            ref->target.value(),
                        DiagnosticLocation::ForNode(ref->target)));
                }
            }
        }
    };
    VisitInlineContent(doc, [&](const InlineContent& content, const NodeAddress&) {
        check_ref(content);
    });

    // Cited, but the project has no bibliography at all: one project-level
    // error (citation plan §9). The per-key pass above was skipped for
    // exactly this case so the Problems panel stays readable.
    if (document_has_citations && input.bibliography_keys.empty()) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Error,
            "E-CITATION-NO-BIBLIOGRAPHY",
            "document cites references but the project has no bibliography "
            "(import a .bib file)",
            DiagnosticLocation::ForProject()));
    }
}

void Validator::ValidateTemplate(const Document& doc, const ValidationInput& input,
                                 ValidationResult* result) const {
    const auto* def = TemplateRegistry::Instance().Find(input.template_id);
    if (!def) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Error, "E-TEMPLATE-UNKNOWN",
            "unknown template: " + input.template_id, DiagnosticLocation::ForProject()));
        return;
    }

    // Template-specific required-field checks (drive the editor's field
    // hints as well as the Problems panel).
    const auto& fm = doc.front_matter();
    const auto& req = def->required;
    if (req.title && InlineIsBlank(fm.title)) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Warning, "W-REQ-TITLE",
            "template requires a title", DiagnosticLocation::ForProject()));
    }
    if (req.authors && fm.authors.empty()) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Warning, "W-REQ-AUTHORS",
            "template requires at least one author",
            DiagnosticLocation::ForProject()));
    }
    if (req.affiliations && fm.affiliations.empty()) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Warning, "W-REQ-AFFILIATIONS",
            "template requires at least one affiliation",
            DiagnosticLocation::ForProject()));
    }
    if (req.author_affiliations) {
        for (const auto& author : fm.authors) {
            if (author.affiliations.empty()) {
                result->diagnostics.push_back(MakeDiag(
                    input.revision, DiagnosticSeverity::Warning,
                    "W-REQ-AUTHOR-AFFILIATION",
                    "author has no affiliation: " + author.name,
                    DiagnosticLocation::ForProject()));
            }
        }
    }
    if (req.abstract_text &&
        (!fm.abstract_text || InlineIsBlank(*fm.abstract_text))) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Warning, "W-REQ-ABSTRACT",
            "template requires an abstract", DiagnosticLocation::ForProject()));
    }
    if (req.keywords && fm.keywords.empty()) {
        result->diagnostics.push_back(MakeDiag(
            input.revision, DiagnosticSeverity::Warning, "W-REQ-KEYWORDS",
            "template requires keywords", DiagnosticLocation::ForProject()));
    }

    // Heading depth capability (plan §9): a heading deeper than the template
    // supports will not render as a distinct level.
    const int max_depth = def->capabilities.max_heading_depth;
    if (max_depth > 0 && max_depth < 3) {
        VisitHeadings(doc, [&](const NodeAddress& address) {
            if (address.depth() <= max_depth) return;
            result->diagnostics.push_back(MakeDiag(
                input.revision, DiagnosticSeverity::Warning, "W-HEADING-DEPTH",
                std::string("template supports ") + std::to_string(max_depth) +
                    " heading levels: " + ToString(address.kind),
                DiagnosticLocation::ForNode(address.node)));
        });
    }
}

}  // namespace pf
