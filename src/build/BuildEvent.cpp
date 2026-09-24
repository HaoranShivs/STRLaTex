#include "build/BuildEvent.h"

#include <cstdio>
#include <ctime>

namespace pf {

const char* ToString(BuildEventType type) {
    switch (type) {
    case BuildEventType::BuildStarted:
        return "BuildStarted";
    case BuildEventType::GenerationStarted:
        return "GenerationStarted";
    case BuildEventType::GenerationFinished:
        return "GenerationFinished";
    case BuildEventType::ProcessStarted:
        return "ProcessStarted";
    case BuildEventType::StdOut:
        return "StdOut";
    case BuildEventType::StdErr:
        return "StdErr";
    case BuildEventType::ProcessFinished:
        return "ProcessFinished";
    case BuildEventType::BuildSucceeded:
        return "BuildSucceeded";
    case BuildEventType::BuildFailed:
        return "BuildFailed";
    case BuildEventType::BuildCancelled:
        return "BuildCancelled";
    case BuildEventType::InternalMessage:
        return "InternalMessage";
    }
    return "?";
}

std::int64_t BuildEventNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string FormatBuildTimestamp(std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    const int millis = static_cast<int>(timestamp_ms % 1000);
    std::tm tm_local{};
#ifndef _WIN32
    localtime_r(&seconds, &tm_local);
#else
    localtime_s(&tm_local, &seconds);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d.%03d]", tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
                  millis < 0 ? 0 : millis);
    return std::string(buffer);
}

} // namespace pf
