#pragma once
// MathExpression：每一项 math 功能都构建于其上的唯一内容类型
// （math-输入设计 §2）。
//
// STRTex 隐藏文档级 LaTeX。用户唯一需要输入 LaTeX 的地方是数学表达式的
// 主体，而且即便在那里，定界符与外层环境也由 generator 掌管：
//
//     用户输入          \frac{a}{b}
//     generator 输出    \(\frac{a}{b}\)            （行内）
//                       \begin{equation}...\end{equation}  （带编号块）
//
// 本头文件对 Qt、block、文件或渲染一无所知。

#include <cstdint>
#include <string>

namespace pf {

// 与用户输入完全一致的 math 主体。定界符从不存储。
struct MathExpression {
    std::string latex;
    bool operator==(const MathExpression&) const = default;
};

// math 表达式的使用场合。flavor 决定 generator 在主体外包裹何种外层结构。
enum class MathFlavor : std::uint8_t {
    Inline,
    Display,
};

// 表达式生命周期（设计 §8）。
//   Valid   - 源码被接受；可以显示 preview。
//   Pending - 用户仍在输入；尚未视为完成。
//   Invalid - 解析/校验失败。源码原样保留，不作任何改动。
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

// 校验 math 主体的结果。`error` 是供人阅读的消息，`code` 是稳定的
// 诊断 id（"E-MATH-..."）。
struct MathValidation {
    MathState state = MathState::Valid;
    std::string code;
    std::string error;

    bool valid() const noexcept { return state == MathState::Valid; }
    bool pending() const noexcept { return state == MathState::Pending; }
    bool invalid() const noexcept { return state == MathState::Invalid; }
};

}  // namespace pf
