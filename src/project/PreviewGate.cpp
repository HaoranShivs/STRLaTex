#include "project/PreviewGate.h"

namespace pf {

const char* ToString(PreviewGateDecision decision) {
    switch (decision) {
        case PreviewGateDecision::Accept: return "Accept";
        case PreviewGateDecision::NoProject: return "NoProject";
        case PreviewGateDecision::ForeignProject: return "ForeignProject";
        case PreviewGateDecision::StaleRevision: return "StaleRevision";
        case PreviewGateDecision::StaleSnapshot: return "StaleSnapshot";
        case PreviewGateDecision::StaleBuild: return "StaleBuild";
    }
    return "?";
}

PreviewGateDecision EvaluatePreviewGate(const PreviewGateInput& current,
                                        const BuildResult& result) {
    if (!current.has_project) return PreviewGateDecision::NoProject;
    // Project identity first: switching projects reuses revisions, so a
    // revision check alone would let project A's PDF into project B.
    if (result.project_id != current.project_id) {
        return PreviewGateDecision::ForeignProject;
    }
    if (result.revision != current.revision) {
        return PreviewGateDecision::StaleRevision;
    }
    if (!current.snapshot_id.empty() &&
        result.snapshot_id != current.snapshot_id) {
        return PreviewGateDecision::StaleSnapshot;
    }
    if (!current.build_id.empty() && result.build_id != current.build_id) {
        return PreviewGateDecision::StaleBuild;
    }
    return PreviewGateDecision::Accept;
}

}  // namespace pf
