#include "editing/UndoHistory.h"

namespace pf {

void UndoHistory::Push(HistoryEntry entry) {
    undo_.push_back(std::move(entry));
    // 新编辑使任何已存在的重做分支失效。
    redo_.clear();
}

std::optional<HistoryEntry> UndoHistory::PopUndo() {
    if (undo_.empty())
        return std::nullopt;
    HistoryEntry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(entry);
    return entry;
}

std::optional<HistoryEntry> UndoHistory::PopRedo() {
    if (redo_.empty())
        return std::nullopt;
    HistoryEntry entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(entry);
    return entry;
}

void UndoHistory::Clear() {
    undo_.clear();
    redo_.clear();
}

} // namespace pf
