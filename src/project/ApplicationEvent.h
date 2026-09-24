#pragma once
// ApplicationEvent：后台 worker 与单一属主应用线程之间的强类型协议（M1）。
//
// 后台线程绝不触碰 ProjectSession / ProjectState / Document /
// EditingSystem。它们只生成值对象并投递一个 ApplicationEvent；
// ProjectSession::ProcessApplicationEvents() 在持有可变状态的那个线程上应用
// 这些事件。异步结果进入领域模型的唯一接缝就在这里。

#include <variant>

#include "build/BuildCoordinator.h"
#include "persistence/SaveCoordinator.h"

namespace pf {

// 一次 build 阶段跃迁：在 worker 上观察到，在应用线程上应用。
struct BuildPhaseChangedEvent {
    BuildPhase previous = BuildPhase::Idle;
    BuildPhase current = BuildPhase::Idle;
};

// 一次已完成的 build，仍需对照当前 revision/build 进行校验。
struct BuildResultReadyEvent {
    BuildResult result;
};

// 一条结构化的 build 日志事件（Build Diagnostics 方案 §4/§35）。由 build
// worker 在尝试推进过程中投递；应用线程在展示之前先判定其 build id 是否仍然
// 是当前值。
struct BuildEventReadyEvent {
    BuildEvent event;
};

// 一个 save worker 完成了一次 snapshot。
struct SaveCompletedEvent {
    SaveCompletion completion;
};

// autosave 定时器触发。定时器线程只负责投递这一事件；捕获 snapshot（会读取
// 实时的 Document）发生在应用线程上。
struct AutosaveTickEvent {};

using ApplicationEvent = std::variant<BuildPhaseChangedEvent, BuildResultReadyEvent, BuildEventReadyEvent,
                                      SaveCompletedEvent, AutosaveTickEvent>;

// 供 tracing/日志使用的简短标签。
const char* ToString(const ApplicationEvent& event);

} // namespace pf
