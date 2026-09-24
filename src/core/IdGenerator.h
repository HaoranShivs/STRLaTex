#pragma once
// 集中式 id 生成：稳定、唯一、前缀单调递增。
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
    static NodeId NewNode() {
        return NodeId(NewNodeId());
    }
    static std::string NewAssetId() {
        return "a" + std::to_string(++asset_counter_);
    }
    static AssetId NewAsset() {
        return AssetId(NewAssetId());
    }
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

    // 从磁盘加载项目时使用的确定性 id。
    static NodeId NodeFromSerialized(const std::string& value) {
        return NodeId(value);
    }

    // 将计数器推进到刚从磁盘加载的 id 之后。否则新进程在打开项目后仍从零开始，
    // 下一个插入的 block 会复用已存在的 id（出现两个 "n1"）。重复的 node id
    // 会使编辑落到错误的 block 上，并打乱插入顺序。
    static void ObserveNodeId(const std::string& value) {
        ObserveCounter(value, "n", node_counter_);
    }
    static void ObserveAssetId(const std::string& value) {
        ObserveCounter(value, "a", asset_counter_);
    }
    static void ObserveAffiliationId(const std::string& value) {
        ObserveCounter(value, "aff", aff_counter_);
    }

  private:
    // 若 `value` 形如 `<prefix><digits>`，则把 `counter` 至少提升到 <digits>。
    // 其他情况一律忽略：id 是自由格式字符串，非数字 id 不会与生成的序列冲突。
    static void ObserveCounter(const std::string& value, const std::string& prefix,
                               std::atomic<std::uint64_t>& counter) {
        if (value.size() <= prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
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
        while (number > current && !counter.compare_exchange_weak(current, number, std::memory_order_relaxed)) {
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
