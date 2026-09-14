#pragma once
// SaveCoordinator: serializes user saves per destination; older snapshots
// never overwrite newer ones (architecture 补充 rule 5).

#include <filesystem>
#include <mutex>
#include <optional>

#include "persistence/ProjectPersistence.h"

namespace pf {

class SaveCoordinator {
public:
    // Synchronous save with serialization guard. Returns result.
    SaveResult RequestSave(SerializedProject snapshot,
                           const std::filesystem::path& destination);

    // Autosave: writes to autosave dir; does not change Clean/Dirty state.
    SaveResult Autosave(SerializedProject snapshot,
                        const std::filesystem::path& autosave_dir);

private:
    std::mutex save_mutex_;
    std::uint64_t save_counter_ = 0;
    std::optional<ProjectRevision> last_saved_revision_;
};

}  // namespace pf
