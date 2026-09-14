#pragma once
// Centralized id generation: stable, unique, monotonically-prefixed.
#include <atomic>
#include <cstdint>
#include <string>

#include "core/StrongId.h"

namespace pf {

class IdGenerator {
public:
    static std::string NewNodeId() { return "n" + std::to_string(++node_counter_); }
    static NodeId NewNode() { return NodeId(NewNodeId()); }
    static std::string NewAssetId() { return "a" + std::to_string(++asset_counter_); }
    static AssetId NewAsset() { return AssetId(NewAssetId()); }
    static std::string NewAffiliationId() { return "aff" + std::to_string(++aff_counter_); }
    static std::string NewBuildId() { return "b" + std::to_string(++build_counter_); }
    static std::string NewOperationId() { return "op" + std::to_string(++op_counter_); }
    static std::string NewSaveId() { return "s" + std::to_string(++save_counter_); }
    static std::string NewSnapshotId() { return "snap" + std::to_string(++snapshot_counter_); }
    static std::string NewProjectId() { return "p" + std::to_string(++project_counter_); }

    // Deterministic id used when loading a project from disk.
    static NodeId NodeFromSerialized(const std::string& value) { return NodeId(value); }

private:
    inline static std::atomic<std::uint64_t> node_counter_{0};
    inline static std::atomic<std::uint64_t> asset_counter_{0};
    inline static std::atomic<std::uint64_t> aff_counter_{0};
    inline static std::atomic<std::uint64_t> build_counter_{0};
    inline static std::atomic<std::uint64_t> op_counter_{0};
    inline static std::atomic<std::uint64_t> save_counter_{0};
    inline static std::atomic<std::uint64_t> snapshot_counter_{0};
    inline static std::atomic<std::uint64_t> project_counter_{0};
};

}  // namespace pf
