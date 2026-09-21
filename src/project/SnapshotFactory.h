#pragma once
// SnapshotFactory：不可变 snapshot 的创建（架构 十六）。
// snapshot 是应用线程与后台工作之间的隔离墙：一旦捕获，
// worker 只能看到值。

#include <memory>

#include "build/BuildCoordinator.h"
#include "persistence/ProjectPersistence.h"
#include "project/ProjectState.h"

namespace pf {

// 自包含的 save 作业载荷。`serialized` 持有 document 的深拷贝，
// 因此 save worker 绝不会观察到后续编辑。
struct SaveSnapshot {
    ProjectId project_id;
    ProjectRevision revision;
    std::string schema_version = kSchemaVersion;
    SerializedProject serialized;  // 可供持久化使用
};

class SnapshotFactory {
public:
    explicit SnapshotFactory(AssetManager* assets) : assets_(assets) {}

    // 捕获不可变 build snapshot（document 深拷贝；build id 在此生成，
    // 以便应用线程知道该请求的身份）。
    BuildSnapshot CreateBuildSnapshot(const ProjectState& state,
                                      const std::string& bibliography_bibtex) const;

    // 为 save／autosave／恢复捕获项目 snapshot。
    SaveSnapshot CreateSaveSnapshot(const ProjectState& state) const;

private:
    AssetManager* assets_;
};

}  // namespace pf
