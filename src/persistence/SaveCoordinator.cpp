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

SaveOutcome OutcomeFromResult(const SaveResult& result, bool superseded) {
    if (superseded) return SaveOutcome::Superseded;
    switch (result.status) {
        case SaveResult::Status::Ok: return SaveOutcome::Saved;
        case SaveResult::Status::Queued: return SaveOutcome::IoError;
        case SaveResult::Status::IoError: return SaveOutcome::IoError;
        case SaveResult::Status::SerializeError:
            return SaveOutcome::SerializeError;
    }
    return SaveOutcome::IoError;
}

const char* ToString(SaveOutcome outcome) {
    switch (outcome) {
        case SaveOutcome::Saved: return "Saved";
        case SaveOutcome::Superseded: return "Superseded";
        case SaveOutcome::IoError: return "IoError";
        case SaveOutcome::SerializeError: return "SerializeError";
        case SaveOutcome::Stopping: return "Stopping";
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

std::optional<SaveId> SaveCoordinator::Enqueue(
    SerializedProject snapshot, const std::filesystem::path& destination,
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
    {
        std::lock_guard lock(mutex_);
        // P0-03: while stopping (or stopped) there is no worker to drain the
        // task; reporting a fake "Queued" here would make the UI claim a
        // save that never reaches the disk.
        if (stopping_ || stopped_) return std::nullopt;
        const SaveId save_id = task.save_id;
        queue_.push_back(std::move(task));
        condition_.notify_all();
        return save_id;
    }
}

bool SaveCoordinator::Flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    // Bounded wait: a wedged worker (or a very slow disk) must not hang the
    // application thread forever. The completion events keep flowing; the
    // caller decides what a timeout means.
    return condition_.wait_for(lock, timeout, [this] {
        return queue_.empty() && in_flight_ == 0;
    });
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

        // A user save that a newer user save already passed is superseded -
        // reported as such, never as a disk error (P0-03).
        bool superseded = false;
        if (task.kind == SaveKind::User && last_user_saved_revision_ &&
            task.revision < *last_user_saved_revision_) {
            superseded = true;
        }
        SaveResult result;
        if (!superseded) {
            result = RunTask(task);
        } else {
            result.save_id = task.save_id.value();
            result.saved_revision = task.revision;
            result.detail = "superseded by a newer save (revision " +
                            std::to_string(task.revision.value) + " < saved " +
                            std::to_string(last_user_saved_revision_->value) +
                            ")";
        }
        SaveCompletion completion;
        completion.save_id = task.save_id;
        completion.project_id = task.project_id;
        completion.revision = task.revision;
        completion.kind = task.kind;
        completion.superseded = superseded;
        completion.outcome = OutcomeFromResult(result, superseded);
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
