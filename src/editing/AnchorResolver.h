#pragma once
// StableNodeAnchor resolution (architecture section 十).
// Used to re-resolve insertion points against the *current* document state,
// e.g. after an async asset import (architecture 补充 rule 4).

#include <optional>

#include "core/Result.h"
#include "document/Document.h"
#include "editing/EditCommand.h"

namespace pf {

struct ResolvedInsertionPoint {
    NodeId parent;
    std::optional<size_t> index;  // nullopt = append
};

enum class AnchorResolveError {
    ReferenceNodeMissing,  // reference node no longer exists
};

const char* ToString(AnchorResolveError error);

class AnchorResolver {
public:
    Result<ResolvedInsertionPoint, AnchorResolveError> Resolve(
        const Document& document, const StableNodeAnchor& anchor) const;
};

}  // namespace pf

namespace pf {
template <>
AnchorResolveError pf::ToStringError<AnchorResolveError>(const std::string& value);
}  // namespace pf
