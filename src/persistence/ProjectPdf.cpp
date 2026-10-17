#include "persistence/ProjectPdf.h"

#include <chrono>
#include <fstream>
#include <string>
#include <string_view>

#include "core/IdGenerator.h"
#include "core/ProjectPath.h"

namespace pf {

namespace {

bool IsReadablePdf(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return false;
    std::ifstream input(path, std::ios::binary);
    char header[5] = {};
    return input.read(header, sizeof(header)) && std::string_view(header, sizeof(header)) == "%PDF-";
}

} // namespace

Result<std::filesystem::path, IoError> ProjectPdf::Save(const std::filesystem::path& project_dir,
                                                        const std::filesystem::path& compiled_pdf) {
    auto resolved = ResolveUntrustedProjectPath(project_dir, kFileName);
    if (!resolved.ok()) {
        return Unexpected2<IoError>(MakeIoError({}, IoErrorCode::ValidationFailed, PathErrorMessage(resolved.error()),
                                                project_dir / kFileName));
    }
    const auto destination = project_dir / kFileName;
    std::error_code ec;
    // Do not follow a project.pdf link when replacing a project artifact.
    if (std::filesystem::is_symlink(destination, ec)) {
        return Unexpected2<IoError>(
            MakeIoError({}, IoErrorCode::ValidationFailed, "project.pdf must not be a symbolic link", destination));
    }
    if (!IsReadablePdf(compiled_pdf)) {
        return Unexpected2<IoError>(
            MakeIoError({}, IoErrorCode::ReadBackFailed, "the compiled PDF is missing or unreadable", compiled_pdf));
    }
    if (std::filesystem::equivalent(compiled_pdf, destination, ec))
        return destination;

    // Stage on the same filesystem so the replacement cannot expose a partial
    // document to startup preview loading or the current PDF renderer.
    auto temporary = destination;
    temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 IdGenerator::NewOperationId();
    ec.clear();
    std::filesystem::copy_file(compiled_pdf, temporary, std::filesystem::copy_options::none, ec);
    if (ec) {
        const auto error = ec;
        if (error != std::errc::file_exists)
            std::filesystem::remove(temporary, ec);
        return Unexpected2<IoError>(
            MakeIoError(error, IoErrorCode::WriteFailed, "cannot save the project PDF", destination));
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        const auto error = ec;
        std::filesystem::remove(temporary, ec);
        return Unexpected2<IoError>(
            MakeIoError(error, IoErrorCode::ReplaceFailed, "cannot replace the project PDF", destination));
    }
    return destination;
}

std::optional<std::filesystem::path> ProjectPdf::Find(const std::filesystem::path& project_dir) {
    for (const char* relative : {kFileName, ".paperforge/build/main.pdf"}) {
        auto path = ResolveUntrustedProjectPath(project_dir, relative);
        if (path.ok() && IsReadablePdf(path.value()))
            return path.value();
    }
    return std::nullopt;
}

} // namespace pf
