#pragma once
// Unified Diagnostic structure (architecture section 28).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/StrongId.h"

namespace pf {

enum class DiagnosticSource : std::uint8_t {
    Document,
    Template,
    Bibliography,
    Renderer,
    Compiler,
    Persistence,
    Asset,
    Validation,
};

enum class DiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error,
};

const char* ToString(DiagnosticSource source);
const char* ToString(DiagnosticSeverity severity);

// Location: where in the *semantic* document the diagnostic applies.
// Exactly one representation is used (variant-like via optional fields,
// kept simple for V1).
enum class DiagnosticLocationKind : std::uint8_t {
    None,
    Node,           // points to a document node
    Table,          // node + row + column
    CitationKey,    // bibliography citation key
    Project,        // project-level (no specific node)
    GeneratedFile,  // generated LaTeX file + line, no resolvable block
                    // (Build Diagnostics plan §17/§47: template/package/class
                    // errors keep file + line but have no blockId)
};

struct DiagnosticLocation {
    DiagnosticLocationKind kind = DiagnosticLocationKind::None;
    NodeId node;                       // kind == Node / Table
    std::optional<int> row;            // kind == Table
    std::optional<int> column;         // kind == Table
    std::string citation_key;          // kind == CitationKey
    // Generated-source position (plan §12/§17): carried alongside a Node when
    // a compiler error could be mapped back to a block, and used alone for
    // errors that belong to the template or a package.
    std::string file;                  // e.g. "main.tex"
    std::optional<std::uint32_t> line; // 1-based line in `file`
    // Human-readable block kind for the GUI ("Figure", "Text", ...). Filled
    // from the renderer's source map so Problems can name the target.
    std::string label;

    bool has_file_location() const {
        return !file.empty() && line.has_value();
    }
    bool has_block_location() const {
        return (kind == DiagnosticLocationKind::Node ||
                kind == DiagnosticLocationKind::Table) &&
               !node.empty();
    }

    static DiagnosticLocation None() { return {}; }
    static DiagnosticLocation ForNode(NodeId id) {
        DiagnosticLocation loc;
        loc.kind = DiagnosticLocationKind::Node;
        loc.node = std::move(id);
        return loc;
    }
    static DiagnosticLocation ForTableCell(NodeId id, int row, int column) {
        DiagnosticLocation loc;
        loc.kind = DiagnosticLocationKind::Table;
        loc.node = std::move(id);
        loc.row = row;
        loc.column = column;
        return loc;
    }
    static DiagnosticLocation ForCitationKey(std::string key) {
        DiagnosticLocation loc;
        loc.kind = DiagnosticLocationKind::CitationKey;
        loc.citation_key = std::move(key);
        return loc;
    }
    static DiagnosticLocation ForProject() {
        DiagnosticLocation loc;
        loc.kind = DiagnosticLocationKind::Project;
        return loc;
    }
    static DiagnosticLocation ForGeneratedFile(std::string file_path,
                                               std::uint32_t line_number) {
        DiagnosticLocation loc;
        loc.kind = DiagnosticLocationKind::GeneratedFile;
        loc.file = std::move(file_path);
        loc.line = line_number;
        return loc;
    }
};

struct Diagnostic {
    std::string id;  // DiagnosticId
    // The build attempt that produced this diagnostic (plan §12): a
    // diagnostic is only ever displayed as part of its own build's result,
    // which is what keeps an old build from polluting a new one.
    BuildId build_id;
    DiagnosticSource source = DiagnosticSource::Document;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string message;
    ProjectRevision revision;
    DiagnosticLocation location;
    // Unmodified compiler line (plan §12): lets a double-click without a
    // block locate the offending text in the Build Log (§29).
    std::string raw_message;

    std::string Summary() const;
};

std::string MakeDiagnosticId(const std::string& prefix, std::uint64_t counter);

}  // namespace pf
