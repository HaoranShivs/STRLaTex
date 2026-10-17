#pragma once
#include "core/Json.h"
#include "core/Result.h"
#include <map>
#include <string>

namespace pf {
// Project-level identity: retained by snapshots, saves and undo history.
struct TemplateBinding {
    std::string id;
    std::string version;
    std::string digest;
    std::string profile;
    std::map<std::string, std::string> options;
    bool operator==(const TemplateBinding&) const = default;
};
JsonValue TemplateBindingToJson(const TemplateBinding& binding);
Result<TemplateBinding> TemplateBindingFromJson(const JsonValue& json);
} // namespace pf
