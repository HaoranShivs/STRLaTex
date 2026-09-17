#pragma once
// BuildEvent: one structured line in a build's lifecycle (Build Diagnostics
// plan §4). The BuildController publishes only these events; the Build Log
// view renders them, and no GUI code parses compiler output to learn what
// happened.

#include <chrono>
#include <cstdint>
#include <string>

#include "core/StrongId.h"

namespace pf {

enum class BuildEventType : std::uint8_t {
    BuildStarted,
    GenerationStarted,
    GenerationFinished,

    ProcessStarted,
    StdOut,
    StdErr,

    ProcessFinished,

    BuildSucceeded,
    BuildFailed,
    BuildCancelled,

    // Messages from STRTex itself (validator, runtime, parser trouble).
    InternalMessage,
};

const char* ToString(BuildEventType type);

struct BuildEvent {
    BuildId build_id;
    // Wall-clock time the event was emitted, in ms since the epoch; the GUI
    // renders it as [HH:mm:ss.zzz] (plan §7).
    std::int64_t timestamp_ms = 0;
    BuildEventType type = BuildEventType::InternalMessage;
    // For lifecycle events: the human-readable message ("Generating LaTeX").
    // For StdOut/StdErr: the raw compiler text, unmodified (plan §7).
    std::string message;
};

// Current wall-clock time in ms since epoch, for event stamping.
std::int64_t BuildEventNowMs();

// Formats one event's timestamp as [HH:mm:ss.zzz] in local time.
std::string FormatBuildTimestamp(std::int64_t timestamp_ms);

}  // namespace pf
