#pragma once
// UndoHistory：历史合并 + undo/redo 栈。
// EditTransaction 只合并 undo 条目；每次真实改动仍会立即
// 产生新的 ProjectRevision（架构规则 14 / 补充 1-2）。

#include <deque>
#include <optional>
#include <string>

#include "editing/EditCommand.h"

namespace pf {

class UndoHistory {
  public:
    // 用于合并连续编辑的键（例如段落 id + "typing"）。
    using TransactionKey = std::string;

    void Push(HistoryEntry entry);

    std::optional<HistoryEntry> PopUndo();
    std::optional<HistoryEntry> PopRedo();

    void BeginTransaction(TransactionKey key);
    void EndTransaction();
    bool InTransaction() const noexcept {
        return transaction_active_;
    }
    const TransactionKey* CurrentKey() const {
        return transaction_active_ ? &transaction_key_ : nullptr;
    }

    bool CanUndo() const noexcept {
        return !undo_.empty();
    }
    bool CanRedo() const noexcept {
        return !redo_.empty();
    }
    size_t Depth() const noexcept {
        return undo_.size();
    }

    void Clear();

  private:
    // 合并规则：当最后一条 undo 条目与前一条属于同一事务键时，
    // 将其合并进前一条。
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
    bool transaction_active_ = false;
    TransactionKey transaction_key_;
};

} // namespace pf
