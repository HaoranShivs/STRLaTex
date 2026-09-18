#pragma once
// SaveCoordinator: serializes saves on a dedicated worker.
//
// M1 (immutable async pipeline): the coordinator only ever sees an immutable
// SaveTask - a deep copy of the project taken on the application thread. The
// worker never touches ProjectState, the Document or any UI type. Completions
// are handed back through a callback that ProjectSession turns into an
// application event; the revision that was written travels with the result so
// the application thread can decide whether the project is still Clean.
//
// Ordering rule (architecture 补充 rule 5): saves are processed in FIFO order
// by a single worker, and a snapshot older than the last user save is rejected,
// so an older snapshot can never overwrite a newer one.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "persistence/ProjectPersistence.h"

namespace pf {

enum class SaveKind : std::uint8_t {
    User,      // explicit save to project.paper
    Autosave,  // crash-recovery snapshot, never clears Dirty
};

const char* ToString(SaveKind kind);

// P0-03: the outcome of one save task. Superseded is distinct from IoError -
// an old snapshot replaced by a newer save is normal latest-wins behaviour,
// not a disk failure, and the UI must not show a scary error for it.
enum class SaveOutcome : std::uint8_t {
    Saved,
    Superseded,
    IoError,
    SerializeError,
    Stopping,
};

SaveOutcome OutcomeFromResult(const SaveResult& result, bool superseded);
const char* ToString(SaveOutcome outcome);

// An immutable, self-contained save job. Owned by the worker once enqueued.
struct SaveTask {
    SaveId save_id;
    ProjectId project_id;
    ProjectRevision revision;
    std::filesystem::path destination;
    SaveKind kind = SaveKind::User;
    SerializedProject snapshot;
};

// Result of one save task, delivered on the worker thread. `outcome` is the
// structured verdict; `result` carries the legacy status + detail for
// diagnostics. `error` is set only when outcome == IoError.
struct SaveCompletion {
    SaveId save_id;
    ProjectId project_id;
    ProjectRevision revision;
    SaveKind kind = SaveKind::User;
    SaveOutcome outcome = SaveOutcome::Saved;
    SaveResult result;
    bool superseded = false;
};

class SaveCoordinator {
public:
    // `on_completed` is invoked on the save worker thread and must therefore be
    // thread-safe. Empty callback is allowed (result is then dropped).
    explicit SaveCoordinator(
        std::function<void(const SaveCompletion&)> on_completed = {});
    ~SaveCoordinator();

    SaveCoordinator(const SaveCoordinator&) = delete;
    SaveCoordinator& operator=(const SaveCoordinator&) = delete;

    // Application thread: hand the worker an immutable snapshot. Non-blocking.
    // Returns nullopt when the coordinator is stopping (or stopped): the
    // caller must not report a save as queued when it never entered the
    // queue (P0-03: no fake "Queued" during shutdown).
    std::optional<SaveId> Enqueue(SerializedProject snapshot,
                                  const std::filesystem::path& destination,
                                  SaveKind kind);

    // Application thread: block until every enqueued task has been written.
    // Used at shutdown and by CLI/test drivers.
    void Flush();

    // Application thread: stop accepting work, drain the queue, join.
    // Idempotent; called by the destructor.
    void Shutdown();

    // Tasks enqueued but not yet written (diagnostics/tests).
    size_t pending() const;

private:
    void WorkerLoop();
    SaveResult RunTask(const SaveTask& task);

    std::function<void(const SaveCompletion&)> on_completed_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<SaveTask> queue_;
    std::thread worker_;
    bool stopping_ = false;
    size_t in_flight_ = 0;
    // Worker-thread-only: last revision successfully written by a user save.
    std::optional<ProjectRevision> last_user_saved_revision_;
    bool stopped_ = false;
};

}  // namespace pf
