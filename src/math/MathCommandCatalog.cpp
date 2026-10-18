#include "math/MathCommandCatalog.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace pf {

namespace {

std::string LowerAscii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool IsAsciiLetter(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

} // namespace

const char* ToString(MathCategory category) {
    switch (category) {
    case MathCategory::Structure:
        return "Structure";
    case MathCategory::Font:
        return "Font";
    case MathCategory::Greek:
        return "Greek";
    case MathCategory::Operator:
        return "Operators";
    case MathCategory::Relation:
        return "Relations";
    case MathCategory::Arrow:
        return "Arrows";
    case MathCategory::SetLogic:
        return "Sets & Logic";
    case MathCategory::Misc:
        return "Misc";
    case MathCategory::Function:
        return "Functions";
    }
    return "Misc";
}

MathCommandCatalog::MathCommandCatalog() {
    auto add = [this](MathCategory category, const char* command, const char* snippet, const char* label,
                      const char* description, std::vector<std::string> packages = {}) {
        MathCommand entry;
        entry.command = command;
        entry.snippet = snippet;
        entry.label = label;
        entry.description = description;
        entry.category = category;
        entry.packages = std::move(packages);
        commands_.push_back(std::move(entry));
    };

    // ---------------- Structure ----------------
    add(MathCategory::Structure, "frac", "\\frac{$}{$}", "a/b", "Fraction");
    add(MathCategory::Structure, "dfrac", "\\dfrac{$}{$}", "a/b", "Display-style fraction", {"amsmath"});
    add(MathCategory::Structure, "tfrac", "\\tfrac{$}{$}", "a/b", "Text-style fraction", {"amsmath"});
    add(MathCategory::Structure, "sqrt", "\\sqrt{$}", "√", "Square root");
    add(MathCategory::Structure, "sum", "\\sum_{$}^{$}", "∑", "Sum with limits");
    add(MathCategory::Structure, "prod", "\\prod_{$}^{$}", "∏", "Product with limits");
    add(MathCategory::Structure, "int", "\\int_{$}^{$}", "∫", "Integral");
    add(MathCategory::Structure, "oint", "\\oint_{$}^{$}", "∮", "Contour integral");
    add(MathCategory::Structure, "lim", "\\lim_{$}", "lim", "Limit");
    add(MathCategory::Structure, "matrix", "\\begin{matrix}$\\end{matrix}", "matrix", "Matrix without delimiters");
    add(MathCategory::Structure, "pmatrix", "\\begin{pmatrix}$\\end{pmatrix}", "(matrix)", "Parenthesized matrix",
        {"amsmath"});
    add(MathCategory::Structure, "bmatrix", "\\begin{bmatrix}$\\end{bmatrix}", "[matrix]", "Bracketed matrix",
        {"amsmath"});
    add(MathCategory::Structure, "cases", "\\begin{cases}$\\end{cases}", "cases", "Piecewise cases", {"amsmath"});
    add(MathCategory::Structure, "aligned", "\\begin{aligned}$\\end{aligned}", "aligned", "Aligned equations",
        {"amsmath"});
    add(MathCategory::Structure, "array", "\\begin{array}{$}$\\end{array}", "array", "Array with column spec");
    add(MathCategory::Structure, "split", "\\begin{split}$\\end{split}", "split", "Split equation", {"amsmath"});
    add(MathCategory::Structure, "binom", "\\binom{$}{$}", "C(n,k)", "Binomial coefficient", {"amsmath"});
    add(MathCategory::Structure, "boxed", "\\boxed{$}", "box", "Boxed formula", {"amsmath"});
    add(MathCategory::Structure, "overbrace", "\\overbrace{$}^{$}", "⏞", "Overbrace with label", {"amsmath"});
    add(MathCategory::Structure, "underbrace", "\\underbrace{$}_{$}", "⏟", "Underbrace with label", {"amsmath"});
    add(MathCategory::Structure, "overset", "\\overset{$}{$}", "a over b", "Place a symbol above", {"amsmath"});
    add(MathCategory::Structure, "underset", "\\underset{$}{$}", "a under b", "Place a symbol below", {"amsmath"});
    add(MathCategory::Structure, "substack", "\\substack{$\\\\$}", "substack", "Stacked subscripts", {"amsmath"});
    add(MathCategory::Structure, "overrightarrow", "\\overrightarrow{$}", "v⃗", "Vector arrow", {"amsmath"});
    add(MathCategory::Structure, "pmod", "\\pmod{$}", "(mod n)", "Parenthesized modulus");
    add(MathCategory::Structure, "power", "^{$}", "x²", "Superscript / power");
    add(MathCategory::Structure, "subscript", "_{$}", "xₙ", "Subscript");
    add(MathCategory::Structure, "paren", "\\left($\\right)", "( )", "Auto-sized parentheses");
    add(MathCategory::Structure, "abs", "\\left|$\\right|", "|x|", "Absolute value");
    add(MathCategory::Structure, "norm", "\\left\\|$\\right\\|", "‖x‖", "Norm");
    add(MathCategory::Structure, "derivative", "\\frac{d $}{d $}", "d/dx", "Derivative");
    add(MathCategory::Structure, "partial_derivative", "\\frac{\\partial $}{\\partial $}", "∂/∂x",
        "Partial derivative");
    add(MathCategory::Structure, "overline", "\\overline{$}", "x̄", "Overline");
    add(MathCategory::Structure, "hat", "\\hat{$}", "x̂", "Hat accent");
    add(MathCategory::Structure, "bar", "\\bar{$}", "x̄", "Bar accent");
    add(MathCategory::Structure, "tilde", "\\tilde{$}", "x̃", "Tilde accent");
    add(MathCategory::Structure, "vec", "\\vec{$}", "x⃗", "Vector accent");
    add(MathCategory::Structure, "vmatrix", "\\begin{vmatrix}$\\end{vmatrix}", "|matrix|",
        "Determinant matrix", {"amsmath"});
    add(MathCategory::Structure, "Bmatrix", "\\begin{Bmatrix}$\\end{Bmatrix}", "{matrix}",
        "Brace matrix", {"amsmath"});
    add(MathCategory::Structure, "Vmatrix", "\\begin{Vmatrix}$\\end{Vmatrix}", "‖matrix‖",
        "Norm matrix", {"amsmath"});
    add(MathCategory::Structure, "gathered", "\\begin{gathered}$\\end{gathered}", "gathered",
        "Centered equation rows", {"amsmath"});
    add(MathCategory::Structure, "smallmatrix", "\\begin{smallmatrix}$\\end{smallmatrix}", "small matrix",
        "Inline matrix", {"amsmath"});
    add(MathCategory::Structure, "iint", "\\iint_{$}", "∬", "Double integral");
    add(MathCategory::Structure, "iiint", "\\iiint_{$}", "∭", "Triple integral");
    add(MathCategory::Structure, "coprod", "\\coprod_{$}^{$}", "∐", "Coproduct");
    add(MathCategory::Structure, "nthroot", "\\sqrt[$]{$}", "ⁿ√", "Nth root");
    add(MathCategory::Structure, "brackets", "\\left[$\\right]", "[ ]", "Auto-sized brackets");
    add(MathCategory::Structure, "floor", "\\left\\lfloor $\\right\\rfloor", "⌊x⌋", "Floor");
    add(MathCategory::Structure, "ceil", "\\left\\lceil $\\right\\rceil", "⌈x⌉", "Ceiling");
    add(MathCategory::Structure, "anglebrackets", "\\left\\langle $\\right\\rangle", "⟨x⟩",
        "Angle brackets");
    add(MathCategory::Structure, "underline", "\\underline{$}", "x̲", "Underline");
    add(MathCategory::Structure, "dot", "\\dot{$}", "ẋ", "Dot accent");
    add(MathCategory::Structure, "ddot", "\\ddot{$}", "ẍ", "Double-dot accent");
    add(MathCategory::Structure, "widehat", "\\widehat{$}", "x̂", "Wide hat");
    add(MathCategory::Structure, "widetilde", "\\widetilde{$}", "x̃", "Wide tilde");

    // ---------------- Font ----------------
    add(MathCategory::Font, "mathcal", "\\mathcal{$}", "𝒜", "Calligraphic letters");
    add(MathCategory::Font, "mathbb", "\\mathbb{$}", "ℝ", "Double-struck letters (reals, expectation)", {"amssymb"});
    add(MathCategory::Font, "mathfrak", "\\mathfrak{$}", "𝔤", "Fraktur letters (Lie algebras)", {"amssymb"});
    add(MathCategory::Font, "mathscr", "\\mathscr{$}", "ℱ", "Script letters", {"mathrsfs"});
    add(MathCategory::Font, "mathbf", "\\mathbf{$}", "𝐱", "Bold upright letters");
    add(MathCategory::Font, "boldsymbol", "\\boldsymbol{$}", "𝜶", "Bold italic symbols", {"amsmath"});
    add(MathCategory::Font, "mathrm", "\\mathrm{$}", "x", "Upright letters");
    add(MathCategory::Font, "mathit", "\\mathit{$}", "x", "Italic letters");
    add(MathCategory::Font, "mathsf", "\\mathsf{$}", "x", "Sans-serif letters");
    add(MathCategory::Font, "mathtt", "\\mathtt{$}", "x", "Monospace letters");
    add(MathCategory::Font, "operatorname", "\\operatorname{$}", "op", "Named operator", {"amsmath"});
    add(MathCategory::Font, "text", "\\text{$}", "text", "Text inside math");
    add(MathCategory::Font, "textrm", "\\textrm{$}", "rm text", "Upright text");
    add(MathCategory::Font, "textnormal", "\\textnormal{$}", "text", "Normal text");
    add(MathCategory::Font, "mbox", "\\mbox{$}", "box", "Unbreakable text box");

    // ---------------- Greek ----------------
    add(MathCategory::Greek, "alpha", "\\alpha", "α", "Greek alpha");
    add(MathCategory::Greek, "beta", "\\beta", "β", "Greek beta");
    add(MathCategory::Greek, "gamma", "\\gamma", "γ", "Greek gamma");
    add(MathCategory::Greek, "delta", "\\delta", "δ", "Greek delta");
    add(MathCategory::Greek, "epsilon", "\\epsilon", "ϵ", "Greek epsilon");
    add(MathCategory::Greek, "varepsilon", "\\varepsilon", "ε", "Greek epsilon variant");
    add(MathCategory::Greek, "zeta", "\\zeta", "ζ", "Greek zeta");
    add(MathCategory::Greek, "eta", "\\eta", "η", "Greek eta");
    add(MathCategory::Greek, "theta", "\\theta", "θ", "Greek theta");
    add(MathCategory::Greek, "vartheta", "\\vartheta", "ϑ", "Greek theta variant");
    add(MathCategory::Greek, "iota", "\\iota", "ι", "Greek iota");
    add(MathCategory::Greek, "kappa", "\\kappa", "κ", "Greek kappa");
    add(MathCategory::Greek, "lambda", "\\lambda", "λ", "Greek lambda");
    add(MathCategory::Greek, "mu", "\\mu", "μ", "Greek mu");
    add(MathCategory::Greek, "nu", "\\nu", "ν", "Greek nu");
    add(MathCategory::Greek, "xi", "\\xi", "ξ", "Greek xi");
    add(MathCategory::Greek, "pi", "\\pi", "π", "Greek pi");
    add(MathCategory::Greek, "varpi", "\\varpi", "ϖ", "Greek pi variant");
    add(MathCategory::Greek, "rho", "\\rho", "ρ", "Greek rho");
    add(MathCategory::Greek, "varrho", "\\varrho", "ϱ", "Greek rho variant");
    add(MathCategory::Greek, "sigma", "\\sigma", "σ", "Greek sigma");
    add(MathCategory::Greek, "varsigma", "\\varsigma", "ς", "Greek sigma variant");
    add(MathCategory::Greek, "tau", "\\tau", "τ", "Greek tau");
    add(MathCategory::Greek, "upsilon", "\\upsilon", "υ", "Greek upsilon");
    add(MathCategory::Greek, "phi", "\\phi", "ϕ", "Greek phi");
    add(MathCategory::Greek, "varphi", "\\varphi", "φ", "Greek phi variant");
    add(MathCategory::Greek, "chi", "\\chi", "χ", "Greek chi");
    add(MathCategory::Greek, "psi", "\\psi", "ψ", "Greek psi");
    add(MathCategory::Greek, "omega", "\\omega", "ω", "Greek omega");
    add(MathCategory::Greek, "Gamma", "\\Gamma", "Γ", "Greek capital gamma");
    add(MathCategory::Greek, "Delta", "\\Delta", "Δ", "Greek capital delta");
    add(MathCategory::Greek, "Theta", "\\Theta", "Θ", "Greek capital theta");
    add(MathCategory::Greek, "Lambda", "\\Lambda", "Λ", "Greek capital lambda");
    add(MathCategory::Greek, "Xi", "\\Xi", "Ξ", "Greek capital xi");
    add(MathCategory::Greek, "Pi", "\\Pi", "Π", "Greek capital pi");
    add(MathCategory::Greek, "Sigma", "\\Sigma", "Σ", "Greek capital sigma");
    add(MathCategory::Greek, "Upsilon", "\\Upsilon", "Υ", "Greek capital upsilon");
    add(MathCategory::Greek, "Phi", "\\Phi", "Φ", "Greek capital phi");
    add(MathCategory::Greek, "Psi", "\\Psi", "Ψ", "Greek capital psi");
    add(MathCategory::Greek, "Omega", "\\Omega", "Ω", "Greek capital omega");

    // ---------------- Operators ----------------
    add(MathCategory::Operator, "pm", "\\pm", "±", "Plus-minus");
    add(MathCategory::Operator, "mp", "\\mp", "∓", "Minus-plus");
    add(MathCategory::Operator, "times", "\\times", "×", "Times");
    add(MathCategory::Operator, "div", "\\div", "÷", "Division");
    add(MathCategory::Operator, "cdot", "\\cdot", "·", "Dot product");
    add(MathCategory::Operator, "ast", "\\ast", "∗", "Asterisk");
    add(MathCategory::Operator, "star", "\\star", "⋆", "Star");
    add(MathCategory::Operator, "circ", "\\circ", "∘", "Composition");
    add(MathCategory::Operator, "oplus", "\\oplus", "⊕", "Direct sum");
    add(MathCategory::Operator, "otimes", "\\otimes", "⊗", "Tensor product");
    add(MathCategory::Operator, "wedge", "\\wedge", "∧", "Wedge / and");
    add(MathCategory::Operator, "vee", "\\vee", "∨", "Vee / or");
    add(MathCategory::Operator, "land", "\\land", "∧", "Logical and");
    add(MathCategory::Operator, "lor", "\\lor", "∨", "Logical or");
    add(MathCategory::Operator, "smallsetminus", "\\smallsetminus", "∖", "Small set difference", {"amssymb"});
    add(MathCategory::Operator, "bigcup", "\\bigcup_{$}", "⋃", "Big union");
    add(MathCategory::Operator, "bigcap", "\\bigcap_{$}", "⋂", "Big intersection");
    add(MathCategory::Operator, "bigoplus", "\\bigoplus_{$}", "⨁", "Big direct sum");
    add(MathCategory::Operator, "bigotimes", "\\bigotimes_{$}", "⨂", "Big tensor product");

    // ---------------- Relations ----------------
    add(MathCategory::Relation, "leq", "\\leq", "≤", "Less than or equal");
    add(MathCategory::Relation, "geq", "\\geq", "≥", "Greater than or equal");
    add(MathCategory::Relation, "neq", "\\neq", "≠", "Not equal");
    add(MathCategory::Relation, "approx", "\\approx", "≈", "Approximately");
    add(MathCategory::Relation, "equiv", "\\equiv", "≡", "Equivalent");
    add(MathCategory::Relation, "propto", "\\propto", "∝", "Proportional to");
    add(MathCategory::Relation, "sim", "\\sim", "∼", "Similar to");
    add(MathCategory::Relation, "simeq", "\\simeq", "≃", "Asymptotically equal");
    add(MathCategory::Relation, "cong", "\\cong", "≅", "Congruent");
    add(MathCategory::Relation, "ll", "\\ll", "≪", "Much less than");
    add(MathCategory::Relation, "gg", "\\gg", "≫", "Much greater than");
    add(MathCategory::Relation, "lesssim", "\\lesssim", "≲", "Less or similar", {"amssymb"});
    add(MathCategory::Relation, "gtrsim", "\\gtrsim", "≳", "Greater or similar", {"amssymb"});
    add(MathCategory::Relation, "leqslant", "\\leqslant", "⩽", "Less than or equal (slanted)", {"amssymb"});
    add(MathCategory::Relation, "geqslant", "\\geqslant", "⩾", "Greater than or equal (slanted)", {"amssymb"});
    add(MathCategory::Relation, "nleq", "\\nleq", "≰", "Not less than or equal", {"amssymb"});
    add(MathCategory::Relation, "ngeq", "\\ngeq", "≱", "Not greater than or equal", {"amssymb"});
    add(MathCategory::Relation, "prec", "\\prec", "≺", "Precedes");
    add(MathCategory::Relation, "succ", "\\succ", "≻", "Succeeds");
    add(MathCategory::Relation, "preceq", "\\preceq", "⪯", "Precedes or equal");
    add(MathCategory::Relation, "succeq", "\\succeq", "⪰", "Succeeds or equal");
    add(MathCategory::Relation, "parallel", "\\parallel", "∥", "Parallel");
    add(MathCategory::Relation, "perp", "\\perp", "⊥", "Perpendicular");
    add(MathCategory::Relation, "mid", "\\mid", "∣", "Divides / such that");
    add(MathCategory::Relation, "nmid", "\\nmid", "∤", "Does not divide", {"amssymb"});

    // ---------------- Arrows ----------------
    add(MathCategory::Arrow, "to", "\\to", "→", "To");
    add(MathCategory::Arrow, "rightarrow", "\\rightarrow", "→", "Right arrow");
    add(MathCategory::Arrow, "leftarrow", "\\leftarrow", "←", "Left arrow");
    add(MathCategory::Arrow, "Rightarrow", "\\Rightarrow", "⇒", "Right implication");
    add(MathCategory::Arrow, "Leftarrow", "\\Leftarrow", "⇐", "Left implication");
    add(MathCategory::Arrow, "leftrightarrow", "\\leftrightarrow", "↔", "Bidirectional");
    add(MathCategory::Arrow, "mapsto", "\\mapsto", "↦", "Maps to");
    add(MathCategory::Arrow, "uparrow", "\\uparrow", "↑", "Up arrow");
    add(MathCategory::Arrow, "downarrow", "\\downarrow", "↓", "Down arrow");
    add(MathCategory::Arrow, "longrightarrow", "\\longrightarrow", "⟶", "Long right arrow");
    add(MathCategory::Arrow, "Longrightarrow", "\\Longrightarrow", "⟹", "Long implication");
    add(MathCategory::Arrow, "longleftarrow", "\\longleftarrow", "⟵", "Long left arrow");
    add(MathCategory::Arrow, "Longleftarrow", "\\Longleftarrow", "⟸", "Long left implication");
    add(MathCategory::Arrow, "hookrightarrow", "\\hookrightarrow", "↪", "Hook right arrow");
    add(MathCategory::Arrow, "hookleftarrow", "\\hookleftarrow", "↩", "Hook left arrow");
    add(MathCategory::Arrow, "rightleftharpoons", "\\rightleftharpoons", "⇌", "Equilibrium arrows");
    add(MathCategory::Arrow, "rightharpoonup", "\\rightharpoonup", "⇀", "Right harpoon");
    add(MathCategory::Arrow, "updownarrow", "\\updownarrow", "↕", "Up-down arrow");
    add(MathCategory::Arrow, "Uparrow", "\\Uparrow", "⇑", "Double up arrow");
    add(MathCategory::Arrow, "Downarrow", "\\Downarrow", "⇓", "Double down arrow");
    add(MathCategory::Arrow, "nearrow", "\\nearrow", "↗", "North-east arrow");
    add(MathCategory::Arrow, "searrow", "\\searrow", "↘", "South-east arrow");
    add(MathCategory::Arrow, "swarrow", "\\swarrow", "↙", "South-west arrow");
    add(MathCategory::Arrow, "nwarrow", "\\nwarrow", "↖", "North-west arrow");

    // ---------------- Sets & logic ----------------
    add(MathCategory::SetLogic, "in", "\\in", "∈", "Element of");
    add(MathCategory::SetLogic, "notin", "\\notin", "∉", "Not element of");
    add(MathCategory::SetLogic, "subset", "\\subset", "⊂", "Subset");
    add(MathCategory::SetLogic, "supset", "\\supset", "⊃", "Superset");
    add(MathCategory::SetLogic, "subseteq", "\\subseteq", "⊆", "Subset or equal");
    add(MathCategory::SetLogic, "cup", "\\cup", "∪", "Union");
    add(MathCategory::SetLogic, "cap", "\\cap", "∩", "Intersection");
    add(MathCategory::SetLogic, "forall", "\\forall", "∀", "For all");
    add(MathCategory::SetLogic, "exists", "\\exists", "∃", "Exists");
    add(MathCategory::SetLogic, "emptyset", "\\emptyset", "∅", "Empty set");
    add(MathCategory::SetLogic, "neg", "\\neg", "¬", "Not");
    add(MathCategory::SetLogic, "setminus", "\\setminus", "∖", "Set difference");
    add(MathCategory::SetLogic, "sqsubset", "\\sqsubset", "⊏", "Square subset", {"amssymb"});
    add(MathCategory::SetLogic, "sqsubseteq", "\\sqsubseteq", "⊑", "Square subset or equal", {"amssymb"});
    add(MathCategory::SetLogic, "sqsupset", "\\sqsupset", "⊐", "Square superset", {"amssymb"});
    add(MathCategory::SetLogic, "sqsupseteq", "\\sqsupseteq", "⊒", "Square superset or equal", {"amssymb"});
    add(MathCategory::SetLogic, "varnothing", "\\varnothing", "∅", "Empty set (variant)", {"amssymb"});
    add(MathCategory::SetLogic, "top", "\\top", "⊤", "Top / true");
    add(MathCategory::SetLogic, "bot", "\\bot", "⊥", "Bottom / false");
    add(MathCategory::SetLogic, "vdash", "\\vdash", "⊢", "Proves");
    add(MathCategory::SetLogic, "models", "\\models", "⊨", "Models");
    add(MathCategory::SetLogic, "therefore", "\\therefore", "∴", "Therefore", {"amssymb"});
    add(MathCategory::SetLogic, "because", "\\because", "∵", "Because", {"amssymb"});

    // ---------------- Functions ----------------
    add(MathCategory::Function, "log", "\\log", "log", "Logarithm");
    add(MathCategory::Function, "ln", "\\ln", "ln", "Natural logarithm");
    add(MathCategory::Function, "exp", "\\exp", "exp", "Exponential");
    add(MathCategory::Function, "sin", "\\sin", "sin", "Sine");
    add(MathCategory::Function, "cos", "\\cos", "cos", "Cosine");
    add(MathCategory::Function, "tan", "\\tan", "tan", "Tangent");
    add(MathCategory::Function, "cot", "\\cot", "cot", "Cotangent");
    add(MathCategory::Function, "sec", "\\sec", "sec", "Secant");
    add(MathCategory::Function, "csc", "\\csc", "csc", "Cosecant");
    add(MathCategory::Function, "arcsin", "\\arcsin", "arcsin", "Arcsine");
    add(MathCategory::Function, "arccos", "\\arccos", "arccos", "Arccosine");
    add(MathCategory::Function, "arctan", "\\arctan", "arctan", "Arctangent");
    add(MathCategory::Function, "sinh", "\\sinh", "sinh", "Hyperbolic sine");
    add(MathCategory::Function, "cosh", "\\cosh", "cosh", "Hyperbolic cosine");
    add(MathCategory::Function, "tanh", "\\tanh", "tanh", "Hyperbolic tangent");
    add(MathCategory::Function, "min", "\\min_{$}", "min", "Minimum");
    add(MathCategory::Function, "max", "\\max_{$}", "max", "Maximum");
    add(MathCategory::Function, "sup", "\\sup_{$}", "sup", "Supremum");
    add(MathCategory::Function, "inf", "\\inf_{$}", "inf", "Infimum");
    add(MathCategory::Function, "liminf", "\\liminf_{$}", "lim inf", "Limit inferior");
    add(MathCategory::Function, "limsup", "\\limsup_{$}", "lim sup", "Limit superior");
    add(MathCategory::Function, "det", "\\det", "det", "Determinant");
    add(MathCategory::Function, "dim", "\\dim", "dim", "Dimension");
    add(MathCategory::Function, "ker", "\\ker", "ker", "Kernel");
    add(MathCategory::Function, "deg", "\\deg", "deg", "Degree");
    add(MathCategory::Function, "gcd", "\\gcd", "gcd", "Greatest common divisor");
    add(MathCategory::Function, "arg", "\\arg", "arg", "Argument");
    add(MathCategory::Function, "mod", "\\bmod", "mod", "Binary modulo");
    add(MathCategory::Function, "bmod", "\\bmod", "bmod", "Binary modulo");
    add(MathCategory::Function, "Pr", "\\Pr", "Pr", "Probability");
    add(MathCategory::Function, "hom", "\\hom", "hom", "Homomorphism");

    // ---------------- Misc ----------------
    add(MathCategory::Misc, "infty", "\\infty", "∞", "Infinity");
    add(MathCategory::Misc, "partial", "\\partial", "∂", "Partial derivative");
    add(MathCategory::Misc, "nabla", "\\nabla", "∇", "Nabla");
    add(MathCategory::Misc, "ell", "\\ell", "ℓ", "Script l");
    add(MathCategory::Misc, "hbar", "\\hbar", "ℏ", "Reduced Planck constant");
    add(MathCategory::Misc, "Re", "\\Re", "ℜ", "Real part");
    add(MathCategory::Misc, "Im", "\\Im", "ℑ", "Imaginary part");
    add(MathCategory::Misc, "angle", "\\angle", "∠", "Angle");
    add(MathCategory::Misc, "triangle", "\\triangle", "△", "Triangle");
    add(MathCategory::Misc, "ldots", "\\ldots", "…", "Ellipsis");
    add(MathCategory::Misc, "cdots", "\\cdots", "⋯", "Centered ellipsis");
    add(MathCategory::Misc, "vdots", "\\vdots", "⋮", "Vertical ellipsis");
    add(MathCategory::Misc, "ddots", "\\ddots", "⋱", "Diagonal ellipsis");
    add(MathCategory::Misc, "aleph", "\\aleph", "ℵ", "Aleph");
    add(MathCategory::Misc, "wp", "\\wp", "℘", "Weierstrass p");
    add(MathCategory::Misc, "prime", "\\prime", "′", "Prime");
    add(MathCategory::Misc, "surd", "\\surd", "√", "Surd");
    add(MathCategory::Misc, "flat", "\\flat", "♭", "Flat");
    add(MathCategory::Misc, "natural", "\\natural", "♮", "Natural");
    add(MathCategory::Misc, "sharp", "\\sharp", "♯", "Sharp");
    add(MathCategory::Misc, "dagger", "\\dagger", "†", "Dagger");
    add(MathCategory::Misc, "ddagger", "\\ddagger", "‡", "Double dagger");

    // 面板专用的语法/环境入口：它们没有可直接输入的同名 LaTeX command。
    for (const char* syntax : {"power", "subscript", "paren", "abs", "norm", "derivative", "partial_derivative",
                               "matrix", "pmatrix", "bmatrix", "cases", "aligned", "array", "split", "vmatrix",
                               "Bmatrix", "Vmatrix", "gathered", "smallmatrix", "nthroot", "brackets", "floor",
                               "ceil", "anglebrackets"}) {
        for (auto& command : commands_) {
            if (command.command == syntax)
                command.completable = false;
        }
    }
}

const MathCommandCatalog& MathCommandCatalog::Instance() {
    static const MathCommandCatalog catalog;
    return catalog;
}

std::vector<MathCommand> MathCommandCatalog::InCategory(MathCategory category) const {
    std::vector<MathCommand> out;
    for (const auto& command : commands_) {
        if (command.category == category)
            out.push_back(command);
    }
    return out;
}

std::vector<MathCommand> MathCommandCatalog::Search(const std::string& query) const {
    const std::string needle = LowerAscii(query);
    if (needle.empty())
        return commands_;
    std::vector<MathCommand> out;
    for (const auto& command : commands_) {
        if (LowerAscii(command.command).find(needle) != std::string::npos ||
            LowerAscii(command.label).find(needle) != std::string::npos ||
            LowerAscii(command.description).find(needle) != std::string::npos) {
            out.push_back(command);
        }
    }
    return out;
}

const MathCommand* MathCommandCatalog::Find(const std::string& command) const {
    for (const auto& entry : commands_) {
        if (entry.command == command)
            return &entry;
    }
    return nullptr;
}

std::vector<std::string> MathCommandCatalog::RequiredPackages(const std::string& latex) const {
    std::set<std::string> packages;
    for (size_t i = 0; i < latex.size(); ++i) {
        if (latex[i] != '\\' || i + 1 >= latex.size() || !IsAsciiLetter(latex[i + 1]))
            continue;
        size_t end = i + 1;
        while (end < latex.size() && IsAsciiLetter(latex[end]))
            ++end;
        const std::string name = latex.substr(i + 1, end - i - 1);
        if (const MathCommand* command = Find(name)) {
            for (const auto& package : command->packages)
                packages.insert(package);
        }
        i = end - 1;
    }
    return {packages.begin(), packages.end()};
}

} // namespace pf
