#pragma once
// ApplicationEvent: the typed protocol between background workers and the
// single-owner application thread (M1).
//
// Background threads never touch ProjectSession / ProjectState / Document /
// EditingSystem. They produce value objects and post an ApplicationEvent;
// ProjectSession::ProcessApplicationEvents() applies them on the thread that
// owns the mutable state. This is the only seam where async results enter the
// domain.

#include <variant>

#include "build/BuildCoordinator.h"
#include "persistence/SaveCoordinator.h"

namespace pf {

// A build phase transition, observed on the worker, applied on the app thread.
struct BuildPhaseChangedEvent {
    BuildPhase previous = BuildPhase::Idle;
    BuildPhase current = BuildPhase::Idle;
};

// A completed build, still to be checked against the current revision/build.
struct BuildResultReadyEvent {
    BuildResult result;
};

// A save worker finished one snapshot.
struct SaveCompletedEvent {
    SaveCompletion completion;
};

// The autosave timer fired. The timer thread only posts this; capturing the
// snapshot (which reads the live Document) happens on the app thread.
struct AutosaveTickEvent {};

using ApplicationEvent =
    std::variant<BuildPhaseChangedEvent, BuildResultReadyEvent,
                 SaveCompletedEvent, AutosaveTickEvent>;

// Short label used by tracing/logging.
const char* ToString(const ApplicationEvent& event);

}  // namespace pf
