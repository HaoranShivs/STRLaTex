#include "math/MathGenerator.h"

namespace pf {

std::string GenerateInlineMath(const MathExpression& expression) {
    return "\\(" + expression.latex + "\\)";
}

std::string GenerateDisplayMath(const MathExpression& expression, bool numbered,
                                const std::string& label) {
    if (!numbered) {
        return "\\[\n" + expression.latex + "\n\\]\n";
    }
    std::string out = "\\begin{equation}";
    if (!label.empty()) {
        out += "\\label{" + label + "}";
    }
    out += "\n" + expression.latex + "\n\\end{equation}\n";
    return out;
}

}  // namespace pf
