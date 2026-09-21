#include "core/Diagnostic.h"

namespace pf {

const char* ToString(DiagnosticSource source) {
    switch (source) {
        case DiagnosticSource::Document: return "Document";
        case DiagnosticSource::Template: return "Template";
        case DiagnosticSource::Bibliography: return "Bibliography";
        case DiagnosticSource::Renderer: return "Renderer";
        case DiagnosticSource::Compiler: return "Compiler";
        case DiagnosticSource::Persistence: return "Persistence";
        case DiagnosticSource::Asset: return "Asset";
        case DiagnosticSource::Validation: return "Validation";
    }
    return "Unknown";
}

const char* ToString(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Info: return "Info";
        case DiagnosticSeverity::Warning: return "Warning";
        case DiagnosticSeverity::Error: return "Error";
    }
    return "Unknown";
}

std::string Diagnostic::Summary() const {
    std::string out = ToString(severity);
    out += " [";
    out += code;
    out += "] ";
    out += message;
    if (location.kind == DiagnosticLocationKind::Node ||
        location.kind == DiagnosticLocationKind::Table) {
        out += " (node ";
        out += location.node.value();
        if (location.row) {
            out += " row " + std::to_string(*location.row);
        }
        if (location.column) {
            out += " col " + std::to_string(*location.column);
        }
        out += ")";
    } else if (location.kind == DiagnosticLocationKind::CitationKey) {
        out += " (key " + location.citation_key + ")";
    } else if (location.kind == DiagnosticLocationKind::GeneratedFile) {
        out += " (" + location.file + ":" +
               std::to_string(location.line.value_or(0)) + ")";
    }
    // 已映射的编译器错误同时携带两种身份：它所属的 block，以及它来源的
    // 生成源码位置（方案 §17）。
    if (location.kind != DiagnosticLocationKind::GeneratedFile &&
        location.has_file_location()) {
        out += " [" + location.file + ":" +
               std::to_string(location.line.value_or(0)) + "]";
    }
    return out;
}

std::string MakeDiagnosticId(const std::string& prefix, std::uint64_t counter) {
    return prefix + "-" + std::to_string(counter);
}

}  // namespace pf
