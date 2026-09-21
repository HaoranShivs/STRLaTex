#pragma once

#include <string>
#include <vector>

#include "build/Compiler.h"
#include "core/Diagnostic.h"
#include "render/SourceMap.h"

namespace pf {

// 把编译器的原始消息转换为结构化 Diagnostic（Build Diagnostics 方案
// §15-§19）。mapper 是编译器输出行变成问题的唯一场所：GUI 绝不自己解析
// 日志文本。
class DiagnosticMapper {
public:
    std::vector<Diagnostic> Map(const CompileResult& result,
                                const SourceMap& source_map,
                                ProjectRevision revision,
                                const BuildId& build_id = BuildId()) const;

    // 单条编译器消息的稳定 diagnostic code（方案 §13/§16）：首版会归类高
    // 价值的 LaTeX 模式，其余一律留作 LATEX_ERROR / LATEX_WARNING。分类器
    // 可以把实为 warning 的原始 "is_error" 行降级（Overfull、
    // undefined reference/citation 等）。
    static std::string CodeFor(const CompilerMessage& message,
                               DiagnosticSeverity* severity);
};

}  // namespace pf
