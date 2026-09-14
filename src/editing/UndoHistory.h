#pragma once
// UndoHistory: history coalescing + undo/redo stacks.
// EditTransaction only merges undo entries; every real mutation still
// produces a new ProjectRevision immediately (architecture rule 14 / 补充 1-2).

#include <deque>
#include <optional>
#include <string>

#include "editing/EditCommand.h"

namespace pf {

class UndoHistory {
public:
    // Key used to coalesce consecutive edits (e.g. paragraph id + "typing").
    using TransactionKey = std::string;

    void Push(HistoryEntry entry);

    std::optional<HistoryEntry> PopUndo();
    std::optional<HistoryEntry> PopRedo();

    void BeginTransaction(TransactionKey key);
    void EndTransaction();
    bool InTransaction() const noexcept { return transaction_active_; }
    const TransactionKey* CurrentKey() const {
        return transaction_active_ ? &transaction_key_ : nullptr;
    }

    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }
    size_t Depth() const noexcept { return undo_.size(); }

    void Clear();

private:
    // Coalesce rule: last undo entry merges into the previous one when both
    // belong to the same transaction key.
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
    bool transaction_active_ = false;
    TransactionKey transaction_key_;
};

}  // namespace pf
