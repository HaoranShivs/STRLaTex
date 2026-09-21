#include "editing/UndoHistory.h"

namespace pf {

void UndoHistory::Push(HistoryEntry entry) {
    if (transaction_active_ && !undo_.empty()) {
        // 在 V1 中，合并意味着：同一 transaction 窗口内、具有相同 key 的
        // 记录保持相邻；由 transaction 的所有者决定何时通过分组把它们
        // 暴露为一次 undo。这里仍将它们保存为独立记录，不额外打标记——
        // 分组由 EditingSystem 通过 key 完成。（V1 简化：不做破坏性合并。）
        undo_.push_back(std::move(entry));
        return;
    }
    undo_.push_back(std::move(entry));
    redo_.clear();
}

std::optional<HistoryEntry> UndoHistory::PopUndo() {
    if (undo_.empty()) return std::nullopt;
    HistoryEntry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(entry);
    return entry;
}

std::optional<HistoryEntry> UndoHistory::PopRedo() {
    if (redo_.empty()) return std::nullopt;
    HistoryEntry entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(entry);
    return entry;
}

void UndoHistory::BeginTransaction(TransactionKey key) {
    transaction_key_ = std::move(key);
    transaction_active_ = true;
}

void UndoHistory::EndTransaction() { transaction_active_ = false; }

void UndoHistory::Clear() {
    undo_.clear();
    redo_.clear();
    transaction_active_ = false;
}

}  // namespace pf
