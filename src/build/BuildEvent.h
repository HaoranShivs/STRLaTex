#pragma once
// BuildEvent：build 生命周期中的一条结构化记录（Build Diagnostics
// 方案 §4）。BuildController 只发布这些事件；Build Log 视图负责渲染，
// 没有任何 GUI 代码通过解析编译器输出来得知发生了什么。

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

    // 来自 STRTex 自身的消息（validator、runtime、parser 故障）。
    InternalMessage,
};

const char* ToString(BuildEventType type);

struct BuildEvent {
    BuildId build_id;
    // 事件发出时的墙上时钟时间，单位为自 epoch 起的毫秒数；GUI 将其渲染为
    // [HH:mm:ss.zzz]（方案 §7）。
    std::int64_t timestamp_ms = 0;
    BuildEventType type = BuildEventType::InternalMessage;
    // 对于生命周期事件：人类可读的消息（"Generating LaTeX"）。
    // 对于 StdOut/StdErr：未经修改的编译器原始文本（方案 §7）。
    std::string message;
};

// 当前墙上时钟时间，单位为自 epoch 起的毫秒数，用于给事件打时间戳。
std::int64_t BuildEventNowMs();

// 将单个事件的时间戳按本地时间格式化为 [HH:mm:ss.zzz]。
std::string FormatBuildTimestamp(std::int64_t timestamp_ms);

} // namespace pf
