#pragma once
// MathExpression: the single content type every math feature is built on
// (math-input design §2).
//
// STRTex hides document-level LaTeX. The only place a user types LaTeX is the
// body of a mathematical expression, and even there the delimiters and the
// outer environment are owned by the generator:
//
//     user types        \frac{a}{b}
//     generator emits   \(\frac{a}{b}\)            (inline)
//                       \begin{equation}...\end{equation}  (numbered block)
//
// Nothing in this header knows about Qt, blocks, files or rendering.

#include <cstdint>
#include <string>

namespace pf {

// The math body exactly as the user typed it. Delimiters are never stored.
struct MathExpression {
    std::string latex;
    bool operator==(const MathExpression&) const = default;
};

// Where a math expression is used. The flavor decides which outer construct
// the generator wraps around the body.
enum class MathFlavor : std::uint8_t {
    Inline,
    Display,
};

// Expression lifecycle (design §8).
//   Valid   - source accepted; a preview may be shown.
//   Pending - the user is still typing; not yet considered finished.
//   Invalid - parsing/validation failed. The source is preserved untouched.
enum class MathState : std::uint8_t {
    Valid,
    Pending,
    Invalid,
};

inline const char* ToString(MathState state) {
    switch (state) {
        case MathState::Valid: return "Valid";
        case MathState::Pending: return "Pending";
        case MathState::Invalid: return "Invalid";
    }
    return "Invalid";
}

// Result of validating a math body. `error` is a human-readable message and
// `code` is a stable diagnostic id ("E-MATH-...").
struct MathValidation {
    MathState state = MathState::Valid;
    std::string code;
    std::string error;

    bool valid() const noexcept { return state == MathState::Valid; }
    bool pending() const noexcept { return state == MathState::Pending; }
    bool invalid() const noexcept { return state == MathState::Invalid; }
};

}  // namespace pf
