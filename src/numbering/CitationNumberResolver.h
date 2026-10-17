#pragma once
// CitationNumberResolver（引用方案 §3）：citation key -> 显示编号 / 展示元数据
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
// Citation picker v2（WP-01）把「编号 + 展示元数据」继续收敛在这里，
// 避免 GUI 里出现第二个并行缓存：Views() 是选择器与 References 面板共用的
// 唯一条目视图。author-year 样式（WP-10）只增加样式分支，不改变数据流。

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bibliography/BibliographyService.h"
#include "document/Document.h"

namespace pf {

enum class CitationStyle {
    NumericFirstUse, // 当前：unsrtnat/IEEEtranN + numbers,sort&compress
    AuthorYear,      // WP-10：plainnat/biblatex author-year
};

struct CitationDisplayInfo {
    int number = 0;           // 从 1 开始的 citation-order 编号；未解析时为 0
    bool resolved = false;    // false = key 不在 bibliography 中 -> [?]
    bool cited = false;       // 本文是否引用过（number > 0）
    std::string display;      // 单个 key 的默认 pill 文本，例如 "[2]"
    std::string author_label; // 第一作者家族名（多人时带 " et al."）
    std::string year;
};

// picker / References 面板的条目视图。它不是 document 的投影，而是
// bibliography + 当前编号结果的合并投影。
struct CitationView {
    std::string key;
    std::string author_label;
    std::string title;
    std::string venue;
    std::string year;
    std::string entry_type;
    int number = 0; // 0 = 本文尚未引用
    bool cited = false;
    bool known = false; // key 是否在 bibliography 中
};

class CitationNumberResolver {
  public:
    // 按阅读顺序遍历 `document`，为 `bibliography` 中存在的每个 key 编号。
    // 未被任何位置引用的 key 完全不会获得编号。
    static CitationNumberResolver Build(const Document& document, const BibliographyDatabase& bibliography,
                                        CitationStyle style = CitationStyle::NumericFirstUse);

    // 单个 key 的编号。当 bibliography 知道该 key 但文档尚未引用它时返回
    // nullopt；被引用但未知的 key 以未解析状态返回，display 为 "[?]"
    // （绝不会悄然消失）。
    std::optional<CitationDisplayInfo> Find(const std::string& key) const;

    // 单 key 或多 key 引用的 pill 文本：key 去重、编号升序排序、三个及以上
    // 的连续串压缩为 "a-c"（即 natbib 的 sort&compress 结果）、未解析的
    // key 显示为 "?"。输出示例："[1]"、"[1, 3]"、"[1-3]"、"[2, ?]"。
    std::string FormatPill(const std::vector<std::string>& keys) const;

    // mode-aware 版本（WP-01）。数字样式下：
    //   Parenthetical -> "[1]" / "[1, 2]"（与旧行为完全一致）
    //   Narrative     -> "Vaswani et al. [1]"
    // author-year 样式下：
    //   Parenthetical -> "(Vaswani et al., 2017)"
    //   Narrative     -> "Vaswani et al. (2017)"
    std::string FormatPill(const std::vector<std::string>& keys, CitationMode mode) const;

    bool IsCited(const std::string& key) const {
        return numbers_.count(key) != 0;
    }

    CitationStyle style() const noexcept {
        return style_;
    }

    // 全部 *已知* 条目（bibliography 中存在），按作者 / 年份 / key 排序。
    // 未在 bibliography 中的 citation key 不会出现在这里：picker 展示的是
    // 可选文献，而不是悬空引用。
    std::vector<CitationView> Views() const;

    bool empty() const noexcept {
        return numbers_.empty();
    }

    const std::map<std::string, int>& numbers() const noexcept {
        return numbers_;
    }

  private:
    std::string AuthorLabelFor(const std::string& key) const;
    std::string YearFor(const std::string& key) const;
    // Narrative 样式下单个 key 的文本；未知 key 返回 "[?]"。
    std::string NarrativeEntry(const std::string& key) const;
    std::string ParentheticalAuthorYearEntry(const std::string& key) const;
    std::string NarrativeAuthorYearEntry(const std::string& key) const;

    CitationStyle style_ = CitationStyle::NumericFirstUse;
    // key -> 从 1 开始的编号，按首次出现的引用顺序。它持有完整答案：
    // 不保留指向 bibliography 或 document 的回指，因此 GUI 可以长期持有
    // 该共享实例而不产生任何生命周期耦合。
    std::map<std::string, int> numbers_;
    std::set<std::string> known_keys_;
    std::map<std::string, CitationView> views_; // key -> 展示元数据
};

} // namespace pf
