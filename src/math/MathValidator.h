#pragma once
// MathValidator: the LaTeX input boundary (math-input design §5).
//
// A math body may use math commands and math-internal structures
// (\frac, \sqrt, \sum, \left...\right, \begin{aligned}, \begin{cases}, ...).
// It may NOT control the document or the outer formula environment:
//
//   forbidden commands     \documentclass \usepackage \section \input \def ...
//   forbidden environments \begin{document} \begin{equation} \begin{figure} ...
//   delimiters             $...$  \(...\)  \[...\]   (the generator adds them)
//
// The validator only inspects the source; it never rewrites it. An invalid
// expression keeps its original LaTeX (design §8).

#include <string>

#include "math/MathExpression.h"

namespace pf {

MathValidation ValidateMath(const std::string& latex, MathFlavor flavor);

// True when `command` (without the leading backslash, e.g. "section") is a
// document-level command that math input must not use.
bool IsForbiddenMathCommand(const std::string& command);

// True when `environment` (e.g. "equation", "figure") is an outer environment
// math input must not open. Math-internal ones ("aligned", "cases", "matrix")
// are allowed.
bool IsForbiddenMathEnvironment(const std::string& environment);

}  // namespace pf
