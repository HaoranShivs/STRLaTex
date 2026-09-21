#pragma once
// Template 系统（架构 19）：V1 内置 Generic Article 与 IEEE Conference。
// Template 独立于 Document。

#include <map>
#include <string>
#include <vector>

#include "build/Toolchain.h"

namespace pf {

// 模板要求哪些 front-matter 字段。驱动编辑器的字段完整性提示与模板校验层。
struct TemplateRequiredFields {
    bool title = true;
    bool authors = true;
    bool affiliations = false;   // 至少一个 affiliation
    bool abstract_text = true;
    bool keywords = false;
    bool author_affiliations = false;  // 每位作者至少需要 1 个 affiliation
};

// 模板能够表达的内容。驱动插入菜单（只支持两级的模板不提供 Subsubsection
// Title）与校验器。
struct TemplateCapabilities {
    // Section = 1，Subsection = 2，Subsubsection = 3。限制在 [1, 3]。
    int max_heading_depth = 3;
};

struct TemplateDefinition {
    std::string id;
    std::string name;
    std::string document_class;  // 例如 "article"、"IEEEtran"
    std::vector<std::string> class_options;
    bool two_column = false;
    std::string bibliography_style = "plain";
    // 模板所需的额外 preamble 行。
    std::vector<std::string> preamble_lines;
    TemplateRequiredFields required;
    TemplateCapabilities capabilities;
    // 模板所需的编译 toolchain（方案 §7、§28）。引擎由模板作者决定，
    // 下游不再重新推断。
    TemplateToolchainRequirement toolchain;
};

class TemplateRegistry {
public:
    static TemplateRegistry& Instance();

    const TemplateDefinition* Find(const std::string& id) const;
    std::vector<TemplateDefinition> All() const;

private:
    TemplateRegistry();
    std::map<std::string, TemplateDefinition> templates_;
};

using TemplateSelection = std::string;  // 模板 id

}  // namespace pf
