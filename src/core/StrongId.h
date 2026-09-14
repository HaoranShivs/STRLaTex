#pragma once
// StrongId: strongly-typed identifiers to prevent cross-type id misuse.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace pf {

template <typename Tag>
class StrongId {
public:
    StrongId() = default;
    explicit StrongId(std::string value) : value_(std::move(value)) {}

    const std::string& value() const noexcept { return value_; }
    bool empty() const noexcept { return value_.empty(); }
    void clear() { value_.clear(); }

    auto operator<=>(const StrongId&) const = default;
    bool operator==(const StrongId&) const = default;

private:
    std::string value_;
};

struct ProjectIdTag {};
struct NodeIdTag {};
struct AssetIdTag {};
struct BuildIdTag {};
struct OperationIdTag {};
struct SaveIdTag {};
struct SnapshotIdTag {};
struct AffiliationIdTag {};

using ProjectId = StrongId<ProjectIdTag>;
using NodeId = StrongId<NodeIdTag>;
using AssetId = StrongId<AssetIdTag>;
using BuildId = StrongId<BuildIdTag>;
using OperationId = StrongId<OperationIdTag>;
using SaveId = StrongId<SaveIdTag>;
using SnapshotId = StrongId<SnapshotIdTag>;
using AffiliationId = StrongId<AffiliationIdTag>;

// Versions
struct DocumentVersion {
    std::uint64_t value = 0;
    auto operator<=>(const DocumentVersion&) const = default;
};

struct ProjectRevision {
    std::uint64_t value = 0;
    auto operator<=>(const ProjectRevision&) const = default;
    bool operator==(const ProjectRevision&) const = default;
};

struct BibliographyRevision {
    std::uint64_t value = 0;
    auto operator<=>(const BibliographyRevision&) const = default;
};

}  // namespace pf

namespace std {
template <typename Tag>
struct hash<pf::StrongId<Tag>> {
    size_t operator()(const pf::StrongId<Tag>& id) const noexcept {
        return hash<string>()(id.value());
    }
};
}  // namespace std
