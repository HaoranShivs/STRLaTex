#pragma once
// Structured filesystem errors (P0-03).
//
// Persistence entry points return Result<T, IoError> instead of
// bool + std::string*: the UI and the log need to tell "the disk is full"
// from "the path is a directory" from "permission denied", and a plain
// string cannot be branched on. IoError carries the code, a user-readable
// message, a diagnostic detail and the path involved.

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace pf {

enum class IoErrorCode : std::uint8_t {
    DirectoryCreateFailed,
    OpenFailed,
    WriteFailed,
    FlushFailed,
    SyncFailed,
    ReplaceFailed,
    ReadBackFailed,
    ValidationFailed,
    PermissionDenied,
    NoSpace,
    NotFound,
    Cancelled,
    Superseded,
    Stopping,
    Unknown,
};

struct IoError {
    IoErrorCode code = IoErrorCode::Unknown;
    std::string user_message;   // safe to show in the UI
    std::string detail;         // for the log: errno text, path, context
    std::filesystem::path path; // what the operation was touching
    bool retryable = false;

    // "detail" plus the path, for a one-line log entry.
    std::string ToString() const {
        std::string out = detail.empty() ? user_message : detail;
        if (!path.empty()) {
            out += " [";
            out += path.string();
            out += "]";
        }
        return out;
    }
};

// Map a std::error_code onto the closest IoErrorCode, so a full disk and a
// denied permission are distinguishable instead of both reading "failed".
IoErrorCode IoCodeFromErrorCode(const std::error_code& ec);

// Build an IoError from a std::error_code with a user-facing sentence.
IoError MakeIoError(std::error_code ec, IoErrorCode fallback,
                    std::string user_message, std::filesystem::path path);

}  // namespace pf
