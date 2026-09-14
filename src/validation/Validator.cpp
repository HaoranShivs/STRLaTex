#include "validation/Validator.h"

#include <algorithm>

#include "core/IdGenerator.h"
#include "document/InlineText.h"
#include "template/TemplateRegistry.h"

namespace pf {

namespace {

std::uint64_t g_counter = 0;

Diagnostic MakeDiag(ProjectRevision rev, DiagnosticSeverity severity,
                    const std::string& code, const std::string& message,
                    DiagnosticLocation loc) {
    Diagnostic d;
    d.id = MakeDiagnosticId("val", ++g_counter);
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

    // Citations resolve into the bibliography
    auto check_inline = [&](const InlineContent& content) {
        for (const auto& node : content) {
            if (const auto* cit = std::get_if<Citation>(&node)) {
                for (const auto& key : cit->keys) {
                    bool found = std::find(input.bibliography_keys.begin(),
                                           input.bibliography_keys.end(),
                                           key) != input.bibliography_keys.end();
                    if (!found) {
                        result->diagnostics.push_back(MakeDiag(
                            input.revision, DiagnosticSeverity::Error, "E-MISSING-CITATION",
                            "citation key not in bibliography: " + key,
                            DiagnosticLocation::ForCitationKey(key)));
                    }
                }
            }
        }
    };

    auto check_blocks = [&](const std::vector<Block>& blocks) {
        for (const auto& block : blocks) {
            if (const auto* para = std::get_if<Paragraph>(&block)) {
                check_inline(para->content);
            } else if (const auto* fig = std::get_if<Figure>(&block)) {
                if (fig->asset_id.empty()) {
                    result->diagnostics.push_back(MakeDiag(
                        input.revision, DiagnosticSeverity::Error, "E-MISSING-ASSET",
                        "figure has no asset", DiagnosticLocation::ForNode(fig->id)));
                }
            } else if (const auto* table = std::get_if<Table>(&block)) {
                if (!table->IsRectangular()) {
                    result->diagnostics.push_back(MakeDiag(
                        input.revision, DiagnosticSeverity::Error, "E-NON-RECT-TABLE",
                        "table is not rectangular",
                        DiagnosticLocation::ForNode(table->id)));
                }
                check_inline(table->caption);
            } else if (const auto* eq = std::get_if<DisplayEquation>(&block)) {
                if (eq->math_source.empty()) {
                    result->diagnostics.push_back(MakeDiag(
                        input.revision, DiagnosticSeverity::Warning, "W-EMPTY-EQUATION",
                        "display equation is empty", DiagnosticLocation::ForNode(eq->id)));
                }
            }
        }
    };

    for (const auto& section : doc.body().sections) {
        check_blocks(section.blocks);
        for (const auto& sub : section.subsections) {
            check_blocks(sub.blocks);
        }
    }

    // Dangling cross references
    for (const auto& section : doc.body().sections) {
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
        auto check_ref_blocks = [&](const std::vector<Block>& blocks) {
            for (const auto& block : blocks) {
                if (const auto* para = std::get_if<Paragraph>(&block)) check_ref(para->content);
                if (const auto* fig = std::get_if<Figure>(&block)) check_ref(fig->caption);
                if (const auto* tbl = std::get_if<Table>(&block)) check_ref(tbl->caption);
            }
        };
        check_ref_blocks(section.blocks);
        for (const auto& sub : section.subsections) check_ref_blocks(sub.blocks);
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
}

}  // namespace pf
