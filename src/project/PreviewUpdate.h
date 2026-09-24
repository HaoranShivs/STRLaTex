#pragma once
// PreviewUpdate：取代旧的 buildFinished(bool, pdf_path) 信号的强类型应用
// 事件。
//
// GUI 在获准展示这个 PDF 之前，必须能回答「是哪个项目、哪次 build、哪个
// revision 产出了它？」。把这份身份标识带过异步边界，正是让陈旧结果过滤、
// 项目切换、undo 与模板切换都不产生歧义的关键（架构 47）。

#include <filesystem>
#include <string>

#include "core/StrongId.h"

namespace pf {

// 已编译 PDF 产物的身份标识。
struct PdfArtifact {
    std::filesystem::path path;
    BuildId build_id;
    ProjectRevision revision;

    bool valid() const noexcept {
        return !path.empty();
    }
    void clear() {
        path.clear();
    }
};

struct PreviewUpdate {
    ProjectId project_id;
    BuildId build_id;
    ProjectRevision revision;
    PdfArtifact pdf;
    // 仅当编译成功时为 true；失败的 build 同样会产生一次 update，以便 UI
    // 在不触碰 PreviewState 的情况下报告它。
    bool success = false;
};

} // namespace pf
