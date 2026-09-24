#pragma once
// SaveCoordinator：在专用 worker 上串行化保存。
//
// M1（不可变异步流水线）：coordinator 只会看到不可变的 SaveTask——
// 在应用线程上对项目所做的深拷贝。worker 绝不触碰 ProjectState、
// Document 或任何 UI 类型。完成结果通过回调交回，由 ProjectSession
// 转换为应用事件；被写入的 revision 随结果一同传递，
// 以便应用线程判断项目是否仍为 Clean。
//
// 排序规则（架构补充规则 5）：保存由单个 worker 按 FIFO 顺序处理，
// 早于最近一次用户保存的 snapshot 会被拒绝，
// 因此较旧的 snapshot 绝不会覆盖较新的。

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
    User,     // 显式保存到 project.paper
    Autosave, // 崩溃恢复 snapshot，绝不清除 Dirty
};

const char* ToString(SaveKind kind);

// P0-03：单个保存任务的结果。Superseded 与 IoError 不同——
// 旧 snapshot 被更新的保存取代属于正常的「最新者胜」行为，
// 并非磁盘故障，UI 不得为此显示吓人的错误。
enum class SaveOutcome : std::uint8_t {
    Saved,
    Superseded,
    IoError,
    SerializeError,
    Stopping,
};

SaveOutcome OutcomeFromResult(const SaveResult& result, bool superseded);
const char* ToString(SaveOutcome outcome);

// 不可变、自包含的保存作业。入队后由 worker 持有。
struct SaveTask {
    SaveId save_id;
    ProjectId project_id;
    ProjectRevision revision;
    std::filesystem::path destination;
    SaveKind kind = SaveKind::User;
    SerializedProject snapshot;
};

// 单个保存任务的结果，在 worker 线程上投递。`outcome` 是结构化的判定；
// `result` 携带旧式 status + detail 用于诊断。`error` 仅在 outcome == IoError 时设置。
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
    // `on_completed` 在 save worker 线程上调用，因此必须线程安全。
    // 允许传入空回调（此时结果会被丢弃）。
    explicit SaveCoordinator(std::function<void(const SaveCompletion&)> on_completed = {});
    ~SaveCoordinator();

    SaveCoordinator(const SaveCoordinator&) = delete;
    SaveCoordinator& operator=(const SaveCoordinator&) = delete;

    // 应用线程：把不可变 snapshot 交给 worker。非阻塞。
    // 当 coordinator 正在 stopping（或已 stopped）时返回 nullopt：
    // 若任务从未进入队列，调用方不得将其报告为已排队
    // （P0-03：关闭期间不得谎报 "Queued"）。
    std::optional<SaveId> Enqueue(SerializedProject snapshot, const std::filesystem::path& destination, SaveKind kind);

    // 应用线程：阻塞直到所有已入队任务写入完成，或超时到期
    // （P0-03：生产代码绝不无界等待 worker）。超时时返回 false。
    bool Flush(std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

    // 应用线程：停止接受新工作、排空队列、join。
    // 幂等；由析构函数调用。
    void Shutdown();

    // 已入队但尚未写入的任务（诊断/测试用）。
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
    // 仅限 worker 线程：最近一次由用户保存成功写入的 revision。
    std::optional<ProjectRevision> last_user_saved_revision_;
    bool stopped_ = false;
};

} // namespace pf
