#pragma once
// PreviewGate: the application-thread decision "may this completed build
// become the current preview?" (architecture section 47).
//
// This is deliberately a pure function of value types: the mutable
// ProjectSession state is only read on the thread that owns it, and the rules
// can be unit-tested without threads or a compiler. A BuildResult that fails
// the gate is discarded - it must never reach PreviewState or the GUI.

#include <string>

#include "build/BuildCoordinator.h"
#include "core/StrongId.h"

namespace pf {

// What the application thread knows when the result arrives. Captured from
// ProjectSession at accept time; never read from a worker.
struct PreviewGateInput {
    bool has_project = false;
    ProjectId project_id;
    ProjectRevision revision;
    // Identity of the most recent build request. Both are minted together by
    // SnapshotFactory, so a result can be matched to the exact ask.
    std::string snapshot_id;
    BuildId build_id;
};

enum class PreviewGateDecision : std::uint8_t {
    Accept,
    NoProject,
    ForeignProject,   // result belongs to a project that is no longer open
    StaleRevision,    // the document changed while the build was running
    StaleSnapshot,    // a newer snapshot superseded this one
    StaleBuild,       // a newer build request superseded this one
};

const char* ToString(PreviewGateDecision decision);

// Accepts only results whose project, revision and build identity all match
// the current application state. Everything else is stale by definition.
PreviewGateDecision EvaluatePreviewGate(const PreviewGateInput& current,
                                        const BuildResult& result);

}  // namespace pf
