#pragma once
// MathPaletteWidget：数学结构/字体/符号面板（WP-02）。
//
// 面板直接作用于一个 QPlainTextEdit（对话框源码框或公式卡片源码框）：
//   * 有选区时，用命令包裹选区；
//   * 无选区时，在光标处插入模板，并把光标放到第一个空参数。
//
// `MakeMathInsertion` / `InsertMathCommand` 是纯逻辑/单点插入实现，
// 以便在 widget 测试之外验证占位符与光标定位。

#include <QPointer>
#include <QWidget>

#include <string>
#include <vector>

#include "math/MathCommandCatalog.h"

class QGridLayout;
class QPlainTextEdit;
class QTabBar;

namespace pf::gui {

class MathPaletteWidget : public QWidget {
    Q_OBJECT

  public:
    explicit MathPaletteWidget(QWidget* parent = nullptr);

    void SetTarget(QPlainTextEdit* target);
    // 模板实际加载的宏包。为空表示未知，所有按钮都可用。
    void SetAvailablePackages(std::vector<std::string> packages);
    void SetCategory(pf::MathCategory category);
    pf::MathCategory Category() const {
        return category_;
    }

  signals:
    void InsertRequested(const QString& command);

  private:
    void RebuildButtons();
    bool CanInsert(const pf::MathCommand& command) const;
    QPointer<QPlainTextEdit> target_;
    QTabBar* tabs_ = nullptr;
    QGridLayout* grid_ = nullptr;
    std::vector<std::string> packages_;
    pf::MathCategory category_ = pf::MathCategory::Structure;
    bool rebuilding_ = false;
};

struct MathInsertionResult {
    QString text;
    // 插入后光标应位于 text 内的位置（第一个未被选区占用的空参数）。
    int cursor = 0;
};

// 把 snippet 的 `$` 占位符替换为选区；多个占位符时其余留空。
MathInsertionResult BuildMathInsertion(const std::string& snippet, const QString& selection = {});
QString MakeMathInsertion(const std::string& snippet, const QString& selection);

// 在目标编辑器插入/包裹命令，返回插入起点；失败返回 -1。
int InsertMathCommand(QPlainTextEdit* target, const pf::MathCommand& command);

} // namespace pf::gui
