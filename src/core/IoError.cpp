#include "core/IoError.h"

namespace pf {

IoErrorCode IoCodeFromErrorCode(const std::error_code& ec) {
    if (!ec)
        return IoErrorCode::Unknown;
    if (ec == std::errc::permission_denied)
        return IoErrorCode::PermissionDenied;
    if (ec == std::errc::no_space_on_device || ec == std::errc::file_too_large)
        return IoErrorCode::NoSpace;
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory)
        return IoErrorCode::NotFound;
    return IoErrorCode::Unknown;
}

IoError MakeIoError(std::error_code ec, IoErrorCode fallback, std::string user_message, std::filesystem::path path) {
    IoError error;
    const IoErrorCode mapped = IoCodeFromErrorCode(ec);
    error.code = mapped == IoErrorCode::Unknown ? fallback : mapped;
    error.user_message = std::move(user_message);
    error.detail = ec ? ec.message() : error.user_message;
    error.path = std::move(path);
    error.retryable = error.code == IoErrorCode::NoSpace || error.code == IoErrorCode::PermissionDenied;
    return error;
}

} // namespace pf
