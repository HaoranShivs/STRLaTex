#pragma once
// CitationNumberResolver（引用方案 §3）：citation key -> 显示编号
// 映射的唯一所有者。
//
// 文档只存储 citation *key*，从不存储渲染后的 "[1]"；最终编号由
// LaTeX/BibTeX 决定。本 resolver 在 GUI 中实现同一策略，使 pill 与 PDF
// 绝不会不一致：
//
//   * citation-order 数字制（所有模板的 MVP 策略）：key 按其首次引用在文档
//     阅读顺序中的位置编号。Generic Article 模板因此使用 `unsrtnat` 渲染，
//     IEEE 保留 `IEEEtran`，两者都按引用顺序编号——所以扫描一次文档即可
//     精确复现 PDF 中最终的 [1] [2] ...。
//   * 再次引用同一 key 会复用它已有的编号（绝不分配新编号），
//   * 多重引用会对其编号排序并压缩（"[1, 3]"、"[1-3]"），
//   * 不在 bibliography 中的 key 解析为 "?"，且不占用编号
//     （未定义的 \cite 在 LaTeX 中同样不会使真实条目发生位移）。
//
// 若日后加入 author-year 模板（APA 等），该策略将移入 TemplateDefinition，
// 并由本类增加一个 policy 参数。

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bibliography/BibliographyService.h"
#include "document/Document.h"

namespace pf {

struct CitationDisplayInfo {
    int number = 0;        // 从 1 开始的 citation-order 编号；未解析时为 0
    bool resolved = false; // false = key 不在 bibliography 中 -> [?]
    std::string display;   // 单个 key 的紧凑 pill 文本，例如 "[2]"
};

class CitationNumberResolver {
public:
    // 按阅读顺序遍历 `document`，为 `bibliography` 中存在的每个 key 编号。
    // 未被任何位置引用的 key 完全不会获得条目。
    static CitationNumberResolver Build(const Document& document,
                                        const BibliographyDatabase& bibliography);

    // 单个 key 的编号。当 bibliography 知道该 key 但文档尚未引用它时返回
    // nullopt；被引用但未知的 key 以未解析状态返回，display 为 "[?]"
    // （绝不会悄然消失）。
    std::optional<CitationDisplayInfo> Find(const std::string& key) const;

    // 当 bibliography 包含 `key` 时返回 true（无论是否已被引用）。
    bool IsKnown(const std::string& key) const;

    // 单 key 或多 key 引用的 pill 文本：key 去重、编号升序排序、三个及以上
    // 的连续串压缩为 "a-c"（即 natbib 的 sort&compress 结果）、未解析的
    // key 显示为 "?"。输出示例："[1]"、"[1, 3]"、"[1-3]"、"[2, ?]"。
    std::string FormatPill(const std::vector<std::string>& keys) const;

    bool empty() const noexcept { return numbers_.empty(); }

    const std::map<std::string, int>& numbers() const noexcept {
        return numbers_;
    }

private:
    // key -> 从 1 开始的编号，按首次出现的引用顺序。它持有完整答案：
    // 不保留指向 bibliography 或 document 的回指，因此 GUI 可以长期持有
    // 该共享实例而不产生任何生命周期耦合。
    std::map<std::string, int> numbers_;
    std::set<std::string> known_keys_;
};

}  // namespace pf
