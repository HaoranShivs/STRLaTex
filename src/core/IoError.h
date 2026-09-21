#pragma once
// 结构化文件系统错误（P0-03）。
//
// 持久化入口返回 Result<T, IoError>，而不是 bool + std::string*：UI 和日志
// 需要区分「磁盘已满」「路径是目录」「权限被拒绝」，而普通字符串无法据此
// 分支处理。IoError 携带错误码、面向用户的消息、诊断细节以及涉及的路径。

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
    std::string user_message;   // 可以安全地在 UI 中显示
    std::string detail;         // 供日志使用：errno 文本、路径、上下文
    std::filesystem::path path; // 该操作所涉及的路径
    bool retryable = false;

    // 「detail」加上路径，用于单行日志条目。
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

// 将 std::error_code 映射到最接近的 IoErrorCode，使磁盘已满与权限被拒绝
// 可以被区分，而不是都显示为「failed」。
IoErrorCode IoCodeFromErrorCode(const std::error_code& ec);

// 根据 std::error_code 构造 IoError，并附带一句面向用户的说明。
IoError MakeIoError(std::error_code ec, IoErrorCode fallback,
                    std::string user_message, std::filesystem::path path);

}  // namespace pf
