#include "numbering/CitationNumberResolver.h"

#include <cctype>

#include "document/DocumentTraversal.h"

namespace pf {

namespace {

std::string Trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n{}");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n{}");
    return value.substr(first, last - first + 1);
}

// 从 BibTeX author 条目推导展示用的家族名：
//   "Ashish Vaswani"    -> "Vaswani"
//   "Vaswani, Ashish"   -> "Vaswani"
//   "{The OpenAI Team}" -> "The OpenAI Team"
// 多作者时调用方补 " et al."。
std::string FamilyName(const std::string& author) {
    const std::string trimmed = Trim(author);
    if (trimmed.empty())
        return {};
    const size_t comma = trimmed.find(',');
    if (comma != std::string::npos)
        return Trim(trimmed.substr(0, comma));
    const size_t space = trimmed.find_last_of(" \t");
    if (space == std::string::npos)
        return trimmed;
    return Trim(trimmed.substr(space + 1));
}

std::string AuthorLabel(const BibEntry& entry) {
    if (entry.authors.empty())
        return {};
    std::string family = FamilyName(entry.authors.front());
    if (family.empty())
        return {};
    if (entry.authors.size() > 1)
        family += " et al.";
    return family;
}

} // namespace

CitationNumberResolver CitationNumberResolver::Build(const Document& document, const BibliographyDatabase& bibliography,
                                                     CitationStyle style) {
    CitationNumberResolver resolver;
    resolver.style_ = style;
    // 对 bibliography 已知内容的快照，使 resolver 保持为自包含的值
    // （GUI 会把同一个共享实例交给每一行）。
    for (const auto& entry : bibliography.Entries()) {
        resolver.known_keys_.insert(entry.key);
        CitationView view;
        view.key = entry.key;
        view.author_label = AuthorLabel(entry);
        view.title = entry.title;
        view.venue = entry.venue;
        view.year = entry.year;
        view.entry_type = entry.entry_type;
        view.known = true;
        resolver.views_[entry.key] = std::move(view);
    }

    // 编号由首次引用顺序决定。VisitInlineContent 遍历的正是渲染器所输出的
    // inline run（标题、摘要，然后按文档顺序的每个 block），因此这里算出的
    // 编号就是 \bibliographystyle{citation-order} 在 PDF 中产生的编号。
    VisitInlineContent(document, [&](const InlineContent& content, const NodeAddress&) {
        for (const auto& node : content) {
            const auto* citation = std::get_if<Citation>(&node);
            if (!citation)
                continue;
            for (const auto& key : citation->keys) {
                // 未知 key 渲染为 [?]，且从不占用编号。
                if (!resolver.known_keys_.count(key))
                    continue;
                if (resolver.numbers_.count(key))
                    continue;
                resolver.numbers_[key] = static_cast<int>(resolver.numbers_.size()) + 1;
            }
        }
    });

    // 把编号投影回条目视图。Views() 是 picker 与 References 面板的
    // 唯一数据源，因此 cited/number 在这里一次性确定。
    for (auto& entry : resolver.views_) {
        auto& view = entry.second;
        const auto it = resolver.numbers_.find(entry.first);
        if (it != resolver.numbers_.end()) {
            view.number = it->second;
            view.cited = true;
        }
    }
    return resolver;
}

std::optional<CitationDisplayInfo> CitationNumberResolver::Find(const std::string& key) const {
    const auto it = numbers_.find(key);
    const auto view = views_.find(key);
    const std::string author = view == views_.end() ? std::string{} : view->second.author_label;
    const std::string year = view == views_.end() ? std::string{} : view->second.year;
    if (it != numbers_.end()) {
        CitationDisplayInfo info;
        info.number = it->second;
        info.resolved = true;
        info.cited = true;
        info.author_label = author;
        info.year = year;
        if (style_ == CitationStyle::AuthorYear && !author.empty()) {
            info.display = author;
            if (!year.empty())
                info.display += " (" + year + ")";
        } else {
            info.display = "[" + std::to_string(it->second) + "]";
        }
        return info;
    }
    if (!known_keys_.count(key)) {
        // 被引用但不在 bibliography 中：LaTeX 同样显示 [?]，
        // 且不占用编号。
        return CitationDisplayInfo{0, false, false, "[?]", {}, {}};
    }
    // 已知但文档尚未引用的 key——未分配编号。
    return std::nullopt;
}

namespace {

// 对一串 key 去重并保持首次出现顺序。Narrative 输出不能像数字样式那样
// 按编号排序：作者名必须与用户选择的 key 顺序对应。
std::vector<std::string> UniqueInOrder(const std::vector<std::string>& keys) {
    std::vector<std::string> unique;
    unique.reserve(keys.size());
    for (const auto& key : keys) {
        if (std::find(unique.begin(), unique.end(), key) == unique.end())
            unique.push_back(key);
    }
    return unique;
}

} // namespace

std::string CitationNumberResolver::FormatPill(const std::vector<std::string>& keys) const {
    return FormatPill(keys, CitationMode::Parenthetical);
}

std::string CitationNumberResolver::FormatPill(const std::vector<std::string>& keys, CitationMode mode) const {
    if (keys.empty())
        return mode == CitationMode::Narrative ? std::string{} : std::string{"[]"};

    if (style_ == CitationStyle::AuthorYear) {
        const auto unique = UniqueInOrder(keys);
        std::string out;
        for (size_t i = 0; i < unique.size(); ++i) {
            if (i)
                out += "; ";
            out += mode == CitationMode::Narrative ? NarrativeAuthorYearEntry(unique[i])
                                                   : ParentheticalAuthorYearEntry(unique[i]);
        }
        if (mode == CitationMode::Narrative)
            return out;
        return "(" + out + ")";
    }

    if (mode == CitationMode::Narrative) {
        // natbib 的 \citet{a,b} 输出 "Author A [1]; Author B [2]"：
        // 每个作者保留自己的编号，绝不压缩成一个区间。
        const auto unique = UniqueInOrder(keys);
        std::string out;
        for (size_t i = 0; i < unique.size(); ++i) {
            if (i)
                out += "; ";
            out += NarrativeEntry(unique[i]);
        }
        return out;
    }

    // 每个 key 收集一个 token：其编号，无法解析时为 "?"。
    std::vector<int> numbers;
    bool has_unknown = false;
    for (const auto& key : keys) {
        const auto it = numbers_.find(key);
        if (it == numbers_.end()) {
            has_unknown = true;
            continue;
        }
        if (std::find(numbers.begin(), numbers.end(), it->second) == numbers.end()) {
            numbers.push_back(it->second);
        }
    }
    std::sort(numbers.begin(), numbers.end());

    // 将三个及以上的连续串压缩为 "a-c"（natbib 的 sort&compress 写法）；
    // 两个则保留为 "a, b"。
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < numbers.size()) {
        size_t j = i;
        while (j + 1 < numbers.size() && numbers[j + 1] == numbers[j] + 1) {
            ++j;
        }
        if (j - i >= 2) {
            parts.push_back(std::to_string(numbers[i]) + "-" + std::to_string(numbers[j]));
        } else {
            for (size_t k = i; k <= j; ++k) {
                parts.push_back(std::to_string(numbers[k]));
            }
        }
        i = j + 1;
    }
    if (has_unknown)
        parts.push_back("?");

    std::string out = "[";
    for (size_t p = 0; p < parts.size(); ++p) {
        if (p)
            out += ", ";
        out += parts[p];
    }
    out += "]";
    return out;
}

std::string CitationNumberResolver::AuthorLabelFor(const std::string& key) const {
    const auto it = views_.find(key);
    return it == views_.end() ? std::string{} : it->second.author_label;
}

std::string CitationNumberResolver::YearFor(const std::string& key) const {
    const auto it = views_.find(key);
    return it == views_.end() ? std::string{} : it->second.year;
}

std::string CitationNumberResolver::NarrativeEntry(const std::string& key) const {
    std::string author = AuthorLabelFor(key);
    if (author.empty())
        author = key; // 未知 / 缺作者的条目仍要告诉用户引用的是谁
    const auto it = numbers_.find(key);
    std::string number = "[?]";
    if (it != numbers_.end())
        number = "[" + std::to_string(it->second) + "]";
    return author + " " + number;
}

std::string CitationNumberResolver::ParentheticalAuthorYearEntry(const std::string& key) const {
    const std::string author = AuthorLabelFor(key);
    const std::string year = YearFor(key);
    if (author.empty() && year.empty())
        return key;
    if (year.empty())
        return author;
    if (author.empty())
        return year;
    return author + ", " + year;
}

std::string CitationNumberResolver::NarrativeAuthorYearEntry(const std::string& key) const {
    const std::string author = AuthorLabelFor(key);
    const std::string year = YearFor(key);
    if (author.empty() && year.empty())
        return key;
    if (year.empty())
        return author;
    if (author.empty())
        return "(" + year + ")";
    return author + " (" + year + ")";
}

std::vector<CitationView> CitationNumberResolver::Views() const {
    std::vector<CitationView> views;
    views.reserve(views_.size());
    for (const auto& entry : views_)
        views.push_back(entry.second);
    std::sort(views.begin(), views.end(), [](const CitationView& a, const CitationView& b) {
        if (a.author_label != b.author_label)
            return a.author_label < b.author_label;
        if (a.year != b.year)
            return a.year < b.year;
        if (a.title != b.title)
            return a.title < b.title;
        return a.key < b.key;
    });
    return views;
}

} // namespace pf
