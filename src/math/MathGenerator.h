#pragma once
// MathGenerator: the only place that builds math delimiters and the outer
// formula environment (math-input design §6).
//
// The user controls the math body; STRTeX controls the environment. Nothing
// the user types takes part in constructing the outer construct.
//
//   inline        \( body \)
//   numbered      \begin{equation}\label{...}\n body \n\end{equation}
//   unnumbered    \[\n body \n\]

#include <string>

#include "math/MathExpression.h"

namespace pf {

// `\(...\)` around the body.
std::string GenerateInlineMath(const MathExpression& expression);

// A display formula. `label` is the LaTeX label of a numbered formula; when it
// is empty no \label is emitted. `numbered == false` uses \[...\] and ignores
// the label.
std::string GenerateDisplayMath(const MathExpression& expression, bool numbered,
                                const std::string& label);

}  // namespace pf
