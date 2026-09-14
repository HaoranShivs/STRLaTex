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
};

struct DiagnosticLocation {
    DiagnosticLocationKind kind = DiagnosticLocationKind::None;
    NodeId node;                       // kind == Node / Table
    std::optional<int> row;            // kind == Table
    std::optional<int> column;         // kind == Table
    std::string citation_key;          // kind == CitationKey

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
};

struct Diagnostic {
    std::string id;  // DiagnosticId
    DiagnosticSource source = DiagnosticSource::Document;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string message;
    ProjectRevision revision;
    DiagnosticLocation location;

    std::string Summary() const;
};

std::string MakeDiagnosticId(const std::string& prefix, std::uint64_t counter);

}  // namespace pf
