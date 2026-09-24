#pragma once
// PreviewGate：应用线程上的判定「这次完成的 build 能否成为当前预览？」
// （架构 47）。
//
// 这里刻意做成基于值类型的纯函数：可变的 ProjectSession 状态只在持有它的
// 线程上读取，规则本身也无需线程或编译器即可单元测试。未通过 gate 的
// BuildResult 会被直接丢弃——它绝不能进入 PreviewState 或 GUI。

#include <string>

#include "build/BuildCoordinator.h"
#include "core/StrongId.h"

namespace pf {

// 结果到达时应用线程所掌握的信息。在 accept 时从 ProjectSession 抓取，
// 绝不在 worker 线程读取。
struct PreviewGateInput {
    bool has_project = false;
    ProjectId project_id;
    ProjectRevision revision;
    // 最近一次 build 请求的身份标识。两者由 SnapshotFactory 一同生成，
    // 因此可以把结果精确匹配回对应的请求。
    std::string snapshot_id;
    BuildId build_id;
};

enum class PreviewGateDecision : std::uint8_t {
    Accept,
    NoProject,
    ForeignProject, // 结果所属的项目已不再处于打开状态
    StaleRevision,  // build 运行期间文档发生了变更
    StaleSnapshot,  // 更新的 snapshot 取代了这一个
    StaleBuild,     // 更新的 build 请求取代了这一个
};

const char* ToString(PreviewGateDecision decision);

// 只接受 project、revision 与 build 身份都与当前应用状态一致的结果；
// 其余结果按定义均属陈旧。
PreviewGateDecision EvaluatePreviewGate(const PreviewGateInput& current, const BuildResult& result);

} // namespace pf
