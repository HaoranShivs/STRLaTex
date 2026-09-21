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
    // 在此处启动，而非放进初始化列表：该循环会访问在线程之后声明的
    // 成员，否则这些成员在循环运行时尚未初始化。
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
    // Autosave 的 id 带前缀，便于在 build 日志中识别恢复 snapshot；
    // id 来自原子生成器，它同时也保证原子写入的临时文件名唯一。
    const std::string id = (kind == SaveKind::Autosave ? "auto" : "") +
                           IdGenerator::NewSaveId();
    task.save_id = SaveId(id);
    {
        std::lock_guard lock(mutex_);
        // P0-03：在 stopping（或 stopped）期间没有 worker 来消费该任务；
        // 此时谎报 "Queued" 会让 UI 宣称一次从未落盘的保存。
        if (stopping_ || stopped_) return std::nullopt;
        const SaveId save_id = task.save_id;
        queue_.push_back(std::move(task));
        condition_.notify_all();
        return save_id;
    }
}

bool SaveCoordinator::Flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    // 有界等待：卡死的 worker（或极慢的磁盘）绝不能让应用线程永久挂起。
    // 完成事件会继续流动，超时的含义由调用方决定。
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
            // Shutdown 会排空队列：继续写入用户已经请求的内容。
            if (queue_.empty() && stopping_) break;
            task = std::move(queue_.front());
            queue_.pop_front();
            ++in_flight_;
        }

        // 若某次用户保存已被更新的用户保存超越，则属于 superseded——
        // 应如实报告，绝不报为磁盘错误（P0-03）。
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
