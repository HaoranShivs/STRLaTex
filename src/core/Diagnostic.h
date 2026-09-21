#pragma once
// 统一的 Diagnostic 结构（架构 28）。
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

// Location：诊断作用于*语义*文档中的哪个位置。
// 只使用一种表示（借助 optional 字段实现的类 variant 结构，
// V1 中保持简单）。
enum class DiagnosticLocationKind : std::uint8_t {
    None,
    Node,           // 指向某个文档节点
    Table,          // 节点 + 行 + 列
    CitationKey,    // 参考文献引用键
    Project,        // 项目级（无具体节点）
    GeneratedFile,  // 生成的 LaTeX 文件 + 行号，没有可解析的 block
                    // （Build Diagnostics 方案 §17/§47：template/package/class
                    // 错误保留 file + line，但没有 blockId）
};

struct DiagnosticLocation {
    DiagnosticLocationKind kind = DiagnosticLocationKind::None;
    NodeId node;                       // kind == Node / Table
    std::optional<int> row;            // kind == Table
    std::optional<int> column;         // kind == Table
    std::string citation_key;          // kind == CitationKey
    // 生成源码位置（方案 §12/§17）：当编译器错误能够映射回某个 block 时，与
    // Node 一同携带；对于属于 template 或 package 的错误则单独使用。
    std::string file;                  // 例如 "main.tex"
    std::optional<std::uint32_t> line; // `file` 中从 1 开始的行号
    // 供 GUI 展示的 block 类型可读名称（"Figure"、"Text" 等）。由渲染器的
    // source map 填充，使 Problems 能够指明目标。
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
    // 产生该诊断的 build 尝试（方案 §12）：诊断只会作为其所属 build 结果的一部分
    // 展示，这正是防止旧 build 污染新 build 的关键。
    BuildId build_id;
    DiagnosticSource source = DiagnosticSource::Document;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string message;
    ProjectRevision revision;
    DiagnosticLocation location;
    // 未经修改的编译器原始行（方案 §12）：使没有 block 的双击也能在
    // Build Log 中定位到出错的文本（§29）。
    std::string raw_message;

    std::string Summary() const;
};

std::string MakeDiagnosticId(const std::string& prefix, std::uint64_t counter);

}  // namespace pf
