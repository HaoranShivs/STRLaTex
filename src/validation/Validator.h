#pragma once
// Validation (architecture sections 29, 41): structural/semantic/template
// layers on an immutable BuildSnapshot representation of the document.

#include <vector>

#include "core/Diagnostic.h"
#include "document/Document.h"

namespace pf {

// Validation profile selects which layers run.
struct ValidationProfile {
    bool structural = true;
    bool semantic = true;
    bool template_check = true;
};

// NOTE: real request carries snapshot data; defined in SnapshotFactory types.
struct ValidationInput {
    std::string snapshot_id;
    ProjectRevision revision;
    const Document* document = nullptr;
    std::string template_id;
    bool has_bibliography = false;
    std::vector<std::string> bibliography_keys;
    std::vector<std::string> asset_paths;  // existing asset relative paths
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
    void ValidateSemantic(const Document& doc, const ValidationInput& input,
                          ValidationResult* result) const;
    void ValidateTemplate(const Document& doc, const ValidationInput& input,
                          ValidationResult* result) const;
};

}  // namespace pf
