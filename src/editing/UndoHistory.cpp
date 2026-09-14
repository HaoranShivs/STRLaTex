#include "editing/UndoHistory.h"

namespace pf {

void UndoHistory::Push(HistoryEntry entry) {
    if (transaction_active_ && !undo_.empty()) {
        // In V1 coalescing means: entries pushed inside one transaction window
        // with the same key stay adjacent; the transaction owner decides when
        // to expose them as one undo step by grouping. We keep them as separate
        // entries but tag nothing extra here - grouping is done by the
        // EditingSystem via the key. (V1 simplification: no destructive merge.)
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
