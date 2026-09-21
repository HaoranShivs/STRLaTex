#pragma once
// MathGenerator：构建 math 定界符与外层公式环境的唯一位置
// （math-输入设计 §6）。
//
// 用户控制 math 主体；STRTeX 控制环境。用户输入的任何内容都不会
// 参与外层结构的构建。
//
//   行内          \( body \)
//   带编号        \begin{equation}\label{...}\n body \n\end{equation}
//   不带编号      \[\n body \n\]

#include <string>

#include "math/MathExpression.h"

namespace pf {

// 在主体外包裹 `\(...\)`。
std::string GenerateInlineMath(const MathExpression& expression);

// display 公式。`label` 是编号公式的 LaTeX label；为空时不输出 \label。
// `numbered == false` 时使用 \[...\] 并忽略 label。
std::string GenerateDisplayMath(const MathExpression& expression, bool numbered,
                                const std::string& label);

}  // namespace pf
