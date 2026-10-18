#pragma once
// MathCommandCatalog：数学面板、命令补全与宏包诊断的唯一目录（WP-02）。
//
// 面板按钮、`\` 补全、`\mathbb` 等命令所需宏包都来自这里。之前这些信息
// 散落在 UI 代码与文档中，导致“预览能画出来、PDF 却缺包”的坑；现在
// 命令、插入模板、分类与依赖只有一个来源。
//
// 纯 C++（Qt-free），可独立单测。

#include <string>
#include <vector>

namespace pf {

enum class MathCategory {
    Structure,
    Font,
    Greek,
    Operator,
    Relation,
    Arrow,
    SetLogic,
    Misc,
    Function,
};

const char* ToString(MathCategory category);

struct MathCommand {
    std::string command;     // 不含反斜杠，例如 "frac"
    std::string snippet;     // "$" 为光标/选区占位；没有占位符时是纯命令
    std::string label;       // 按钮/补全显示，例如 "a/b"、"ℝ"
    std::string description; // 悬停说明
    MathCategory category = MathCategory::Misc;
    std::vector<std::string> packages; // 空 = 基础 LaTeX
    // 环境/语法类按钮（\begin{...}、^{} 等）只出现在面板中；补全列表
    // 不应把它们显示成 \matrix、\power 之类不存在的命令。
    bool completable = true;
};

class MathCommandCatalog {
  public:
    static const MathCommandCatalog& Instance();

    const std::vector<MathCommand>& All() const noexcept {
        return commands_;
    }
    std::vector<MathCommand> InCategory(MathCategory category) const;
    // 命令名、显示文本或说明包含 query（大小写不敏感）。
    std::vector<MathCommand> Search(const std::string& query) const;
    const MathCommand* Find(const std::string& command) const;

    // 扫描 LaTeX body 中出现的命令，返回它们所需宏包（去重、稳定顺序）。
    std::vector<std::string> RequiredPackages(const std::string& latex) const;

  private:
    MathCommandCatalog();
    std::vector<MathCommand> commands_;
};

} // namespace pf
