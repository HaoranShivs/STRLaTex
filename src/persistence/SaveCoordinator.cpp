#include "persistence/SaveCoordinator.h"

#include "core/IdGenerator.h"

namespace pf {

const char* ToString(SaveKind kind) {
    switch (kind) {
        case SaveKind::User: return "User";
        case SaveKind::Autosave: return "Autosave";
    }
    return "?";
}

SaveCoordinator::SaveCoordinator(
    std::function<void(const SaveCompletion&)> on_completed)
    : on_completed_(std::move(on_completed)) {
    // Started here, not in the init list: the loop touches members declared
    // after the thread, which would otherwise be uninitialized when it runs.
    worker_ = std::thread([this] { WorkerLoop(); });
}

SaveCoordinator::~SaveCoordinator() { Shutdown(); }

SaveId SaveCoordinator::Enqueue(SerializedProject snapshot,
                                const std::filesystem::path& destination,
                                SaveKind kind) {
    SaveTask task;
    task.kind = kind;
    task.project_id = ProjectId(snapshot.project_id);
    task.revision = snapshot.revision;
    task.destination = destination;
    task.snapshot = std::move(snapshot);
    // Autosave ids are prefixed so recovery snapshots are easy to spot in the
    // build log; ids come from the atomic generator, which is also what keeps
    // the atomic-write temp file names unique.
    const std::string id = (kind == SaveKind::Autosave ? "auto" : "") +
                           IdGenerator::NewSaveId();
    task.save_id = SaveId(id);
    const SaveId save_id = task.save_id;
    {
        std::lock_guard lock(mutex_);
        if (!stopping_) queue_.push_back(std::move(task));
    }
    condition_.notify_all();
    return save_id;
}

void SaveCoordinator::Flush() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return queue_.empty() && in_flight_ == 0; });
}

void SaveCoordinator::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    stopped_ = true;
}

size_t SaveCoordinator::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + in_flight_;
}

void SaveCoordinator::WorkerLoop() {
    while (true) {
        SaveTask task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            // Shutdown drains: keep writing what the user already asked for.
            if (queue_.empty() && stopping_) break;
            task = std::move(queue_.front());
            queue_.pop_front();
            ++in_flight_;
        }

        SaveResult result = RunTask(task);
        SaveCompletion completion;
        completion.save_id = task.save_id;
        completion.project_id = task.project_id;
        completion.revision = task.revision;
        completion.kind = task.kind;
        completion.result = result;
        if (on_completed_) on_completed_(completion);

        {
            std::lock_guard lock(mutex_);
            --in_flight_;
        }
        condition_.notify_all();
    }
}

SaveResult SaveCoordinator::RunTask(const SaveTask& task) {
    if (task.kind == SaveKind::User) {
        // Older snapshot must not overwrite a newer save (rule 5).
        if (last_user_saved_revision_ &&
            task.revision < *last_user_saved_revision_) {
            SaveResult result;
            result.status = SaveResult::Status::IoError;
            result.save_id = task.save_id.value();
            result.saved_revision = task.revision;
            result.detail = "stale save rejected (revision " +
                            std::to_string(task.revision.value) + " < saved " +
                            std::to_string(last_user_saved_revision_->value) +
                            ")";
            return result;
        }
    }

    SaveRequest request;
    request.save_id = task.save_id.value();
    request.project_id = task.project_id;
    request.revision = task.revision;
    request.destination = task.destination;
    request.snapshot = task.snapshot;
    auto result = ProjectPersistence::Save(request);
    if (result.status == SaveResult::Status::Ok &&
        task.kind == SaveKind::User) {
        last_user_saved_revision_ = task.revision;
    }
    return result;
}

}  // namespace pf
