#include "editing/AnchorResolver.h"

#include "document/DocumentEditor.h"

namespace pf {

const char* ToString(AnchorResolveError error) {
    switch (error) {
        case AnchorResolveError::ReferenceNodeMissing:
            return "ReferenceNodeMissing";
    }
    return "Unknown";
}

Result<ResolvedInsertionPoint, AnchorResolveError> AnchorResolver::Resolve(
    const Document& document, const StableNodeAnchor& anchor) const {
    const NodeId& ref = anchor.reference_node;

    // Locate the reference node's logical position with a read-only walk.
    const auto& sections = document.body().sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        const auto& section = sections[si];
        if (section.id == ref) {
            switch (anchor.bias) {
                case AnchorBias::Before:
                    return ResolvedInsertionPoint{ref, 0};
                case AnchorBias::After:
                case AnchorBias::InsideEnd:
                    return ResolvedInsertionPoint{ref, std::nullopt};
            }
        }
        for (size_t bi = 0; bi < section.blocks.size(); ++bi) {
            NodeId bid = std::visit([](const auto& b) { return b.id; },
                                    section.blocks[bi]);
            if (bid == ref) {
                switch (anchor.bias) {
                    case AnchorBias::Before:
                        return ResolvedInsertionPoint{section.id, bi};
                    case AnchorBias::After:
                    case AnchorBias::InsideEnd:
                        return ResolvedInsertionPoint{section.id, bi + 1};
                }
            }
        }
        for (size_t ui = 0; ui < section.subsections.size(); ++ui) {
            const auto& sub = section.subsections[ui];
            if (sub.id == ref) {
                switch (anchor.bias) {
                    case AnchorBias::Before:
                        return ResolvedInsertionPoint{ref, 0};
                    case AnchorBias::After:
                    case AnchorBias::InsideEnd:
                        return ResolvedInsertionPoint{ref, std::nullopt};
                }
            }
            for (size_t bi = 0; bi < sub.blocks.size(); ++bi) {
                NodeId bid = std::visit([](const auto& b) { return b.id; },
                                        sub.blocks[bi]);
                if (bid == ref) {
                    switch (anchor.bias) {
                        case AnchorBias::Before:
                            return ResolvedInsertionPoint{sub.id, bi};
                        case AnchorBias::After:
                        case AnchorBias::InsideEnd:
                            return ResolvedInsertionPoint{sub.id, bi + 1};
                    }
                }
            }
        }
    }
    return Unexpected(ToString(AnchorResolveError::ReferenceNodeMissing));
}

}  // namespace pf

namespace pf {
template <>
AnchorResolveError pf::ToStringError<AnchorResolveError>(const std::string& value) {
    if (value == "ReferenceNodeMissing") return AnchorResolveError::ReferenceNodeMissing;
    return AnchorResolveError::ReferenceNodeMissing;
}
}  // namespace pf
