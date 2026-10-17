#pragma once
// LatexRenderer：把语义化的 Document 转换为 LaTeX 源码（架构 20）。
// 产出 BuildPackage + SourceMap；不涉及编译器。

#include <map>
#include <string>
#include <vector>

#include "core/Diagnostic.h"
#include "document/Document.h"
#include "render/BuildPackage.h"
#include "render/SourceMap.h"
#include "template/TemplateRegistry.h"
#include <memory>

namespace pf {

struct RenderRequest {
    std::string build_id;
    std::string snapshot_id;
    ProjectRevision revision;
    const Document* document = nullptr;
    std::string template_id;
    std::shared_ptr<const TemplateDefinition> resolved_template;
    std::string template_error;
    // Asset 解析：asset id -> build 工作区中的文件名。
    std::map<std::string, std::string> asset_files;
    // 参考文献：原始 .bib 内容（为空表示没有）。
    std::string bibliography_bibtex;
};

struct RenderResult {
    enum class Status { Ok, Failed };
    Status status = Status::Ok;
    std::string build_id;
    ProjectRevision revision;
    BuildPackage package;
    SourceMap source_map;
    std::vector<Diagnostic> diagnostics;
};

class LatexRenderer {
  public:
    RenderResult Render(const RenderRequest& request) const;
};

} // namespace pf
