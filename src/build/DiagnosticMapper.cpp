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
    // 首版分类（Build Diagnostics 方案 §13/§16）。只识别高价值模式；
    // 其余一律保留粗粒度的 LATEX_ERROR / LATEX_WARNING code 及其原始文本。
    // 编译器标记为 error、但内容属于已知 warning 的消息会在这里降级，
    // 因为 Warning 绝不能导致 build 失败（§31）。
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
    // 精确去重（方案 §19）：同一位置的同一问题被报告两次（latexmk 会重跑
    // TeX，日志因此重复）时只显示一次。首版不做语义去重。
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

        // 先把消息归到其在生成源中的位置，再把该行映射回产生它的语义节点
        // （方案 §17）。即使映射找到了 block，file + line 仍会留在
        // diagnostic 上，以便 GUI 回退到 Build Log（§28）。
        if (!message.file.empty() && message.line > 0) {
            diagnostic.location = DiagnosticLocation::ForGeneratedFile(
                message.file, message.line);
        } else {
            diagnostic.location = DiagnosticLocation::ForProject();
        }
        // 该映射索引的是 main.tex 的行；来自其他文件（class 文件、package）
        // 的消息会有意解析不到任何 node（§47）。
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
