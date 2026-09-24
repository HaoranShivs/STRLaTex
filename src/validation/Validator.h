#pragma once
// Validation（架构 29、41）：在文档的不可变 BuildSnapshot 表示之上，
// 分层做结构/语义/模板校验。

#include <vector>

#include "core/Diagnostic.h"
#include "document/Document.h"

namespace pf {

// Validation profile 选择运行哪些层。
struct ValidationProfile {
    bool structural = true;
    bool semantic = true;
    bool template_check = true;
};

// 注意：真实请求会携带 snapshot 数据；定义于 SnapshotFactory 的类型中。
struct ValidationInput {
    std::string snapshot_id;
    ProjectRevision revision;
    const Document* document = nullptr;
    std::string template_id;
    bool has_bibliography = false;
    std::vector<std::string> bibliography_keys;
    std::vector<std::string> asset_paths; // 已存在资源的相对路径
};

struct ValidationResult {
    std::string snapshot_id;
    ProjectRevision revision;
    bool can_render = true;
    std::vector<Diagnostic> diagnostics;
};

class Validator {
  public:
    ValidationResult Validate(const ValidationInput& input) const;

  private:
    void ValidateSemantic(const Document& doc, const ValidationInput& input, ValidationResult* result) const;
    void ValidateTemplate(const Document& doc, const ValidationInput& input, ValidationResult* result) const;
};

} // namespace pf
