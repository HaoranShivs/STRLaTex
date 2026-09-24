#pragma once
// StableNodeAnchor 解析（架构 十）。
// 用于针对*当前*文档状态重新解析插入点，
// 例如异步资源导入之后（架构补充 4）。

#include <optional>

#include "core/Result.h"
#include "document/Document.h"
#include "editing/EditCommand.h"

namespace pf {

struct ResolvedInsertionPoint {
    NodeId parent;
    std::optional<size_t> index; // nullopt = 追加
};

enum class AnchorResolveError {
    ReferenceNodeMissing, // 参考节点已不存在
};

const char* ToString(AnchorResolveError error);

class AnchorResolver {
  public:
    Result<ResolvedInsertionPoint, AnchorResolveError> Resolve(const Document& document,
                                                               const StableNodeAnchor& anchor) const;
};

} // namespace pf

namespace pf {
template <> AnchorResolveError pf::ToStringError<AnchorResolveError>(const std::string& value);
} // namespace pf
