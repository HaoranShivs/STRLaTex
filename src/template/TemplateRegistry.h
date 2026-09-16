#pragma once
// Template system (architecture section 19): V1 ships Generic Article and
// IEEE Conference. Templates are independent of the Document.

#include <map>
#include <string>
#include <vector>

#include "build/Toolchain.h"

namespace pf {

// Which front-matter fields a template requires. Drives the editor's
// field-completeness hints and the template validator layer.
struct TemplateRequiredFields {
    bool title = true;
    bool authors = true;
    bool affiliations = false;   // at least one affiliation
    bool abstract_text = true;
    bool keywords = false;
    bool author_affiliations = false;  // each author needs >=1 affiliation
};

// What a template is able to express. Drives the insert menu (a template that
// only supports two levels offers no Subsubsection Title) and the validator.
struct TemplateCapabilities {
    // Section = 1, Subsection = 2, Subsubsection = 3. Clamped to [1, 3].
    int max_heading_depth = 3;
};

struct TemplateDefinition {
    std::string id;
    std::string name;
    std::string document_class;  // e.g. "article", "IEEEtran"
    std::vector<std::string> class_options;
    bool two_column = false;
    std::string bibliography_style = "plain";
    // Additional preamble lines required by the template.
    std::vector<std::string> preamble_lines;
    TemplateRequiredFields required;
    TemplateCapabilities capabilities;
    // Compile toolchain the template needs (plan §7, §28). The template
    // author decides the engine; nothing downstream re-derives it.
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

using TemplateSelection = std::string;  // template id

}  // namespace pf
