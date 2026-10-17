#pragma once

#include <filesystem>
#include <optional>

#include "core/IoError.h"
#include "core/Result.h"

namespace pf {

// The last successful build is a project artifact, independent of temporary
// compiler workspaces. Failed writes leave the previous PDF intact.
class ProjectPdf {
  public:
    static constexpr const char* kFileName = "project.pdf";

    static Result<std::filesystem::path, IoError> Save(const std::filesystem::path& project_dir,
                                                       const std::filesystem::path& compiled_pdf);
    // Older projects kept their PDF in .paperforge/build/main.pdf.
    static std::optional<std::filesystem::path> Find(const std::filesystem::path& project_dir);
};

} // namespace pf
