#pragma once
// MathValidator：LaTeX 输入边界（math-input 设计 §5）。
//
// 数学体可以使用数学命令与数学内部结构
// （\frac、\sqrt、\sum、\left...\right、\begin{aligned}、\begin{cases} 等）。
// 但它不得控制文档或外层公式环境：
//
//   禁止的命令     \documentclass \usepackage \section \input \def ...
//   禁止的环境     \begin{document} \begin{equation} \begin{figure} ...
//   定界符         $...$  \(...\)  \[...\]   （由生成器添加）
//
// 校验器只检查源码，从不改写它。无效表达式保留其原始 LaTeX（设计 §8）。

#include <string>

#include "math/MathExpression.h"

namespace pf {

MathValidation ValidateMath(const std::string& latex, MathFlavor flavor);

// 当 `command`（不含前导反斜杠，例如 "section"）是数学输入不得使用的
// 文档级命令时返回 true。
bool IsForbiddenMathCommand(const std::string& command);

// 当 `environment`（例如 "equation"、"figure"）是数学输入不得打开的外层
// 环境时返回 true。数学内部环境（"aligned"、"cases"、"matrix"）则允许使用。
bool IsForbiddenMathEnvironment(const std::string& environment);

} // namespace pf
