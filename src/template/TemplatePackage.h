#pragma once
#include "template/TemplateRegistry.h"
#include <filesystem>
#include <memory>

namespace pf {
struct TemplatePackage {
    std::string digest;
    std::string version;
    std::map<std::string, std::string> files;
    std::vector<TemplateDefinition> profiles;
};

// No extraction occurs until all ZIP entries have passed path, type, size and
// CRC checks.
Result<TemplatePackage>
InspectTemplateArchive(const std::filesystem::path& archive);
// Publish only after fixture compilation succeeds. Existing bundles are
// verified, never overwritten.
Result<void> InstallTemplatePackage(const std::filesystem::path& project_dir,
                                    const TemplatePackage& package);
Result<std::vector<TemplateDefinition>>
LoadProjectTemplates(const std::filesystem::path& project_dir);
// Checks a pinned package again when capturing a new build (detect on-disk
// edits).
Result<void> VerifyTemplateBinding(const std::filesystem::path& project_dir,
                                   const TemplateBinding& binding);
} // namespace pf
