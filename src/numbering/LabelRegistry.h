#pragma once
// LabelRegistry：LaTeX \label 的唯一目录（WP-03）。
//
// Renderer 与 Validator 过去各自决定「哪个节点写什么 label」：Renderer 只给
// Equation 做清洗/回退，Validator 完全没有重复检测。现在两者共用本目录：
//
//   * heading            -> node id
//   * Figure / Table     -> 用户 label，空则 node id
//   * numbered Equation  -> 用户 label，空则 node id
//   * unnumbered Equation-> 不写 label（也不参与冲突检测）
//
// 清洗规则与 planner 过去使用的规则一致（字母数字与 :._- 保留，空格变 '-'，
// 其余字符丢弃）；清洗后为空时回退到 node id，避免生成空的 \label{}。
// 重复 label 记录在 conflicts() 中；Validator 报 E-DUPLICATE-LABEL，
// planner 在生成 LaTeX 前拒绝继续。

#include <map>
#include <string>
#include <vector>

#include "document/Document.h"

namespace pf {

struct LabelConflict {
    std::string label;
    NodeId first;  // 首次占用该 label 的节点
    NodeId second; // 后出现、与之冲突的节点
};

class LabelRegistry {
  public:
    static LabelRegistry Build(const Document& document);

    // 节点最终写入 LaTeX 的 label。未登记（且 planner 不会写 label）的节点
    // 返回其 node id。
    std::string LabelFor(const NodeId& node) const;

    const std::map<NodeId, std::string>& labels() const noexcept {
        return labels_;
    }
    const std::vector<LabelConflict>& conflicts() const noexcept {
        return conflicts_;
    }
    bool has_conflicts() const noexcept {
        return !conflicts_.empty();
    }

    // 与 planner 完全一致的清洗；空输入返回空字符串。
    static std::string SanitizeLabel(const std::string& label);

  private:
    void Register(const NodeId& node, const std::string& requested);

    std::map<NodeId, std::string> labels_;
    std::map<std::string, NodeId> owners_;
    std::vector<LabelConflict> conflicts_;
};

} // namespace pf
