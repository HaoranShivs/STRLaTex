#include "persistence/SaveCoordinator.h"

#include "core/IdGenerator.h"

namespace pf {

SaveResult SaveCoordinator::RequestSave(SerializedProject snapshot,
                                        const std::filesystem::path& destination) {
    std::lock_guard<std::mutex> lock(save_mutex_);
    // Older snapshot must not overwrite a newer save (rule 5).
    if (last_saved_revision_ && snapshot.revision < *last_saved_revision_) {
        SaveResult result;
        result.status = SaveResult::Status::IoError;
        result.detail = "stale save rejected (revision " +
                        std::to_string(snapshot.revision.value) + " < saved " +
                        std::to_string(last_saved_revision_->value) + ")";
        return result;
    }
    SaveRequest request;
    request.save_id = IdGenerator::NewSaveId();
    request.project_id = ProjectId(snapshot.project_id);
    request.revision = snapshot.revision;
    request.destination = destination;
    request.snapshot = std::move(snapshot);
    auto result = ProjectPersistence::Save(request);
    if (result.status == SaveResult::Status::Ok) {
        last_saved_revision_ = request.revision;
    }
    return result;
}
SaveResult SaveCoordinator::Autosave(SerializedProject snapshot,
                                     const std::filesystem::path& autosave_dir) {
    std::lock_guard<std::mutex> lock(save_mutex_);
    std::error_code ec;
    std::filesystem::create_directories(autosave_dir, ec);
    SaveRequest request;
    request.save_id = "auto" + std::to_string(++save_counter_);
    request.project_id = ProjectId(snapshot.project_id);
    request.revision = snapshot.revision;
    request.destination = autosave_dir / "autosave.paper";
    request.snapshot = std::move(snapshot);
    return ProjectPersistence::Save(request);
}

}  // namespace pf
