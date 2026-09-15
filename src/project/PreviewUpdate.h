#pragma once
// PreviewUpdate: the typed application event that replaces the old
// buildFinished(bool, pdf_path) signal.
//
// The GUI must be able to answer "which project, which build, which revision
// produced this PDF?" before it is allowed to show it. Carrying that identity
// through the async boundary is what makes stale filtering, project switching,
// undo and template switching unambiguous (architecture section 47).

#include <filesystem>
#include <string>

#include "core/StrongId.h"

namespace pf {

// Identity of a compiled PDF artifact.
struct PdfArtifact {
    std::filesystem::path path;
    BuildId build_id;
    ProjectRevision revision;

    bool valid() const noexcept { return !path.empty(); }
    void clear() { path.clear(); }
};

struct PreviewUpdate {
    ProjectId project_id;
    BuildId build_id;
    ProjectRevision revision;
    PdfArtifact pdf;
    // True only for a successful compile; a failed build still produces an
    // update so the UI can report it without touching PreviewState.
    bool success = false;
};

}  // namespace pf
