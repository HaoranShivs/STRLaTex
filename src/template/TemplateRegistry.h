#pragma once
// Template system (architecture section 19): V1 ships Generic Article and
// IEEE Conference. Templates are independent of the Document.

#include <map>
#include <string>
#include <vector>

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
