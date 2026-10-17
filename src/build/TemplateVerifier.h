#pragma once
#include "build/Compiler.h"
#include "template/TemplatePackage.h"

namespace pf {
struct TemplateProbe {
    std::string profile_id;
    std::string plan_digest;
    std::filesystem::path pdf_path;
    std::string log;
};
struct VerifiedTemplatePackage {
    TemplatePackage package;
    std::vector<TemplateProbe> probes;
};
// Static inspection -> isolated fixture builds of EVERY profile -> ready to
// install. No ProjectSession or mutable catalog is touched on this worker path.
Result<VerifiedTemplatePackage>
VerifyTemplateArchive(const std::filesystem::path& archive,
                      const std::filesystem::path& runtime,
                      const std::filesystem::path& workspace,
                      const std::atomic<bool>* cancelled = nullptr,
                      std::function<void(const std::string&)> progress = {});
} // namespace pf
