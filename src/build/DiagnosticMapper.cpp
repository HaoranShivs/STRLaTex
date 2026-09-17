#include "build/DiagnosticMapper.h"

#include <set>
#include <string>
#include <tuple>

namespace pf {
namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

std::string DiagnosticMapper::CodeFor(const CompilerMessage& message,
                                      DiagnosticSeverity* severity) {
    // First-version classification (Build Diagnostics plan §13/§16). Only
    // high-value patterns are recognised; everything else keeps the coarse
    // LATEX_ERROR / LATEX_WARNING code and its raw text. A message the
    // compiler flagged as an error but whose content is a known warning is
    // downgraded here, because a Warning must never fail a build (§31).
    const std::string& text = message.text;

    if (Contains(text, "Overfull \\hbox")) {
        if (severity) *severity = DiagnosticSeverity::Warning;
        return "LATEX_OVERFULL_HBOX";
    }
    if (Contains(text, "Underfull \\hbox")) {
        if (severity) *severity = DiagnosticSeverity::Warning;
        return "LATEX_UNDERFULL_HBOX";
    }
    if (Contains(text, "Overfull \\vbox")) {
        if (severity) *severity = DiagnosticSeverity::Warning;
        return "LATEX_OVERFULL_VBOX";
    }
    if (Contains(text, "Underfull \\vbox")) {
        if (severity) *severity = DiagnosticSeverity::Warning;
        return "LATEX_UNDERFULL_VBOX";
    }
    if (Contains(text, "undefined") && Contains(text, "Citation")) {
        if (severity) *severity = DiagnosticSeverity::Warning;
        return "LATEX_UNDEFINED_CITATION";
    }
    if (Contains(text, "undefined") &&
        (Contains(text, "Reference") || Contains(text, "reference"))) {
        if (severity) *severity = DiagnosticSeverity::Warning;
        return "LATEX_UNDEFINED_REFERENCE";
    }
    if (Contains(text, "Emergency stop")) {
        if (severity) *severity = DiagnosticSeverity::Error;
        return "LATEX_EMERGENCY_STOP";
    }
    if (message.is_error) {
        if (severity) *severity = DiagnosticSeverity::Error;
        return "LATEX_ERROR";
    }
    if (severity) *severity = DiagnosticSeverity::Warning;
    return "LATEX_WARNING";
}

std::vector<Diagnostic> DiagnosticMapper::Map(
    const CompileResult& result, const SourceMap& source_map,
    ProjectRevision revision, const BuildId& build_id) const {
    std::vector<Diagnostic> diagnostics;
    // Exact-duplicate suppression (plan §19): the same problem reported for
    // the same place twice (latexmk reruns TeX, and the log repeats) is
    // displayed once. No semantic dedup in the first version.
    std::set<std::tuple<std::string, std::string, std::string, std::string,
                        std::uint32_t>>
        seen;
    std::uint64_t counter = 0;
    for (const auto& message : result.messages) {
        DiagnosticSeverity severity = DiagnosticSeverity::Error;
        const std::string code = CodeFor(message, &severity);

        const std::string line_key =
            std::to_string(message.line) + '|' + code + '|' + message.text;
        if (!seen.insert(std::make_tuple(message.file, line_key,
                                         message.is_error ? "E" : "W",
                                         code,
                                         message.line))
                             .second) {
            continue;
        }

        Diagnostic diagnostic;
        diagnostic.id = MakeDiagnosticId("compiler", ++counter);
        diagnostic.build_id = build_id;
        diagnostic.source = DiagnosticSource::Compiler;
        diagnostic.severity = severity;
        diagnostic.code = code;
        diagnostic.message = message.text;
        diagnostic.revision = revision;
        diagnostic.raw_message = message.text;

        // Attribute the message to its generated-source position first, then
        // map that line back to the semantic node that produced it (plan
        // §17). Even when the map finds a block, the file + line stay on the
        // diagnostic so the GUI can fall back to the Build Log (§28).
        if (!message.file.empty() && message.line > 0) {
            diagnostic.location = DiagnosticLocation::ForGeneratedFile(
                message.file, message.line);
        } else {
            diagnostic.location = DiagnosticLocation::ForProject();
        }
        // The map indexes main.tex lines; messages from other files (the
        // class file, a package) intentionally resolve to no node (§47).
        if (auto entry = source_map.ResolveEntry(message.line)) {
            diagnostic.location.kind = DiagnosticLocationKind::Node;
            diagnostic.location.node = entry->node;
            diagnostic.location.label = entry->label;
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

}  // namespace pf
