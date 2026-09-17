#pragma once
// Centralized id generation: stable, unique, monotonically-prefixed.
#include <atomic>
#include <cstdint>
#include <string>

#include "core/StrongId.h"

namespace pf {

class IdGenerator {
public:
  static std::string NewNodeId() {
    return "n" + std::to_string(++node_counter_);
  }
  static NodeId NewNode() { return NodeId(NewNodeId()); }
  static std::string NewAssetId() {
    return "a" + std::to_string(++asset_counter_);
  }
  static AssetId NewAsset() { return AssetId(NewAssetId()); }
  static std::string NewAffiliationId() {
    return "aff" + std::to_string(++aff_counter_);
  }
  static std::string NewBuildId() {
    return "b" + std::to_string(++build_counter_);
  }
  static std::string NewOperationId() {
    return "op" + std::to_string(++op_counter_);
  }
  static std::string NewSaveId() {
    return "s" + std::to_string(++save_counter_);
  }
  static std::string NewSnapshotId() {
    return "snap" + std::to_string(++snapshot_counter_);
  }
  static std::string NewProjectId() {
    return "p" + std::to_string(++project_counter_);
  }

  // Deterministic id used when loading a project from disk.
  static NodeId NodeFromSerialized(const std::string &value) {
    return NodeId(value);
  }

  // Advance the counters past ids that were just loaded from disk. Without
  // this a fresh process starts at zero after opening a project, so the next
  // inserted block reuses an id that already exists ("n1" twice). Duplicate
  // node ids make an edit hit the wrong block and scramble insert order.
  static void ObserveNodeId(const std::string &value) {
    ObserveCounter(value, "n", node_counter_);
  }
  static void ObserveAssetId(const std::string &value) {
    ObserveCounter(value, "a", asset_counter_);
  }
  static void ObserveAffiliationId(const std::string &value) {
    ObserveCounter(value, "aff", aff_counter_);
  }

private:
  // If `value` is `<prefix><digits>`, raise `counter` to at least <digits>.
  // Anything else is ignored: ids are free-form strings, and a non-numeric
  // one cannot collide with the generated sequence.
  static void ObserveCounter(const std::string &value,
                             const std::string &prefix,
                             std::atomic<std::uint64_t> &counter) {
    if (value.size() <= prefix.size() ||
        value.compare(0, prefix.size(), prefix) != 0) {
      return;
    }
    std::uint64_t number = 0;
    for (size_t i = prefix.size(); i < value.size(); ++i) {
      const char c = value[i];
      if (c < '0' || c > '9')
        return;
      number = number * 10 + static_cast<std::uint64_t>(c - '0');
    }
    std::uint64_t current = counter.load(std::memory_order_relaxed);
    while (number > current &&
           !counter.compare_exchange_weak(current, number,
                                          std::memory_order_relaxed)) {
    }
  }

  inline static std::atomic<std::uint64_t> node_counter_{0};
  inline static std::atomic<std::uint64_t> asset_counter_{0};
  inline static std::atomic<std::uint64_t> aff_counter_{0};
  inline static std::atomic<std::uint64_t> build_counter_{0};
  inline static std::atomic<std::uint64_t> op_counter_{0};
  inline static std::atomic<std::uint64_t> save_counter_{0};
  inline static std::atomic<std::uint64_t> snapshot_counter_{0};
  inline static std::atomic<std::uint64_t> project_counter_{0};
};

} // namespace pf
