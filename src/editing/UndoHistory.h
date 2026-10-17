#pragma once
// UndoHistory：undo/redo 栈。
// 每次真实改动都会立即产生新的 ProjectRevision（架构规则 14 / 补充 1-2）。

#include <deque>
#include <optional>
#include <string>

#include "editing/EditCommand.h"

namespace pf {

class UndoHistory {
  public:
    void Push(HistoryEntry entry);

    std::optional<HistoryEntry> PopUndo();
    std::optional<HistoryEntry> PopRedo();

    bool CanUndo() const noexcept {
        return !undo_.empty();
    }
    bool CanRedo() const noexcept {
        return !redo_.empty();
    }

    void Clear();

  private:
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
};

} // namespace pf
