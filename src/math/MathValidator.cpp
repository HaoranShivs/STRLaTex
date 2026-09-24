#include "math/MathValidator.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace pf {

namespace {

const std::set<std::string>& ForbiddenCommands() {
    static const std::set<std::string> commands = {
        // 文档类 / 宏包 / 导言区
        "documentclass",
        "documentstyle",
        "usepackage",
        "RequirePackage",
        "LoadClass",
        "PassOptionsToPackage",
        "geometry",
        "hypersetup",
        // 文档结构。（\begin / \end 由扫描器显式处理，
        // 永远不会进入此集合。）
        "part",
        "chapter",
        "section",
        "subsection",
        "subsubsection",
        "paragraph",
        "subparagraph",
        "appendix",
        "tableofcontents",
        "listoffigures",
        "listoftables",
        "frontmatter",
        "mainmatter",
        "backmatter",
        "maketitle",
        "title",
        "author",
        "date",
        "thanks",
        "affiliation",
        "institute",
        "keywords",
        "abstract",
        "bibliography",
        "bibliographystyle",
        "include",
        "includeonly",
        "input",
        "import",
        "subimport",
        "pagestyle",
        "thispagestyle",
        "pagenumbering",
        // 宏 / 环境定义 - 用户自定义宏不在范围内
        "newcommand",
        "renewcommand",
        "providecommand",
        "DeclareMathOperator",
        "def",
        "edef",
        "gdef",
        "xdef",
        "let",
        "newenvironment",
        "renewenvironment",
        "newcounter",
        "setcounter",
        "addtocounter",
        "newlength",
        "setlength",
        "newsavebox",
        "sbox",
        // 公式环境 / 编号控制：由 STRTeX 掌管
        "label",
        "tag",
        "numberwithin",
        "nonumber",
        "notag",
    };
    return commands;
}

const std::set<std::string>& ForbiddenEnvironments() {
    // 外层公式环境以及每一种非 math 的文档环境。
    // math 内部环境（aligned、gathered、split、cases、matrix、
    // smallmatrix、array 等）有意不列入。
    static const std::set<std::string> environments = {
        "document",        "equation",    "equation*",  "displaymath",  "math",     "eqnarray",  "eqnarray*",
        "align",           "align*",      "alignat",    "alignat*",     "flalign",  "flalign*",  "gather",
        "gather*",         "multline",    "multline*",  "subequations", "figure",   "figure*",   "table",
        "table*",          "tabular",     "tabular*",   "tabbing",      "verbatim", "verbatim*", "lstlisting",
        "minted",          "tikzpicture", "pgfpicture", "picture",      "minipage", "center",    "flushleft",
        "flushright",      "itemize",     "enumerate",  "description",  "quote",    "quotation", "verse",
        "thebibliography", "abstract",    "titlepage",  "frame",        "columns",  "column",
    };
    return environments;
}

bool IsAsciiAlpha(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

// 从反斜杠开始读取一个 LaTeX 控制字 / 控制符号。
// 返回命令名（不含反斜杠），并将 `i` 推进到其后。
std::string ReadCommand(const std::string& text, size_t* i) {
    size_t pos = *i + 1; // 跳过反斜杠
    if (pos >= text.size()) {
        *i = pos;
        return {};
    }
    if (!IsAsciiAlpha(text[pos])) {
        // 控制符号：恰好一个字符（例如 \\ \{ \, \) ）
        const std::string name(1, text[pos]);
        *i = pos + 1;
        return name;
    }
    const size_t start = pos;
    while (pos < text.size() && IsAsciiAlpha(text[pos]))
        ++pos;
    *i = pos;
    return text.substr(start, pos - start);
}

// 在 \begin / \end 之后读取 {environment} 参数。格式错误时返回空。
std::string ReadEnvironmentName(const std::string& text, size_t* i) {
    size_t pos = *i;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != '{')
        return {};
    ++pos;
    std::string name;
    while (pos < text.size() && text[pos] != '}') {
        name.push_back(text[pos]);
        ++pos;
    }
    if (pos >= text.size())
        return {};
    *i = pos + 1; // 越过 '}'
    // 去除首尾空白。
    const size_t first = name.find_first_not_of(" \t");
    if (first == std::string::npos)
        return {};
    const size_t last = name.find_last_not_of(" \t");
    return name.substr(first, last - first + 1);
}

MathValidation Invalid(const char* code, std::string message) {
    MathValidation v;
    v.state = MathState::Invalid;
    v.code = code;
    v.error = std::move(message);
    return v;
}

} // namespace

bool IsForbiddenMathCommand(const std::string& command) {
    return ForbiddenCommands().count(command) > 0;
}

bool IsForbiddenMathEnvironment(const std::string& environment) {
    return ForbiddenEnvironments().count(environment) > 0;
}

MathValidation ValidateMath(const std::string& latex, MathFlavor /*flavor*/) {
    MathValidation result;

    // 空源码不是错误 - 它是用户尚未输入完成的表达式
    // （设计 §8：Pending）。
    const size_t first = latex.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        result.state = MathState::Pending;
        result.code = "E-MATH-EMPTY";
        result.error = "math expression is empty";
        return result;
    }

    int brace_depth = 0;
    int left_right = 0;            // \left ... \right 的配对平衡
    std::vector<std::string> envs; // 未闭合的 \begin{...} 栈

    size_t i = 0;
    while (i < latex.size()) {
        const char c = latex[i];

        if (c == '$') {
            return Invalid("E-MATH-DELIMITER", "math source must not contain '$': inline delimiters "
                                               "are generated by STRTeX");
        }

        if (c == '{') {
            ++brace_depth;
            ++i;
            continue;
        }
        if (c == '}') {
            --brace_depth;
            if (brace_depth < 0) {
                return Invalid("E-MATH-UNBALANCED-BRACES", "unbalanced '}' in math source");
            }
            ++i;
            continue;
        }

        if (c == '\\') {
            const size_t command_start = i;
            const std::string command = ReadCommand(latex, &i);
            if (command.empty())
                continue; // 末尾的反斜杠

            // \\ 是换行；\\[2pt] 携带可选间距，因此其后的
            // '[' 不是 display 定界符。
            if (command == "\\")
                continue;

            if (command == "(" || command == ")" || command == "[" || command == "]") {
                return Invalid("E-MATH-DELIMITER",
                               "math source must not contain \\" + command + ": delimiters are generated by STRTeX");
            }

            if (command == "left") {
                ++left_right;
                continue;
            }
            if (command == "right") {
                --left_right;
                if (left_right < 0) {
                    return Invalid("E-MATH-UNBALANCED-DELIMITER", "\\right without a matching \\left");
                }
                continue;
            }

            if (command == "begin" || command == "end") {
                const std::string environment = ReadEnvironmentName(latex, &i);
                if (environment.empty()) {
                    return Invalid("E-MATH-MALFORMED-ENV", "malformed \\" + command + "{...} in math source");
                }
                if (IsForbiddenMathEnvironment(environment)) {
                    return Invalid("E-MATH-FORBIDDEN-ENV", "environment is not allowed in a math "
                                                           "expression: " +
                                                               environment);
                }
                if (command == "begin") {
                    envs.push_back(environment);
                } else {
                    if (envs.empty() || envs.back() != environment) {
                        return Invalid("E-MATH-UNBALANCED-ENV",
                                       "\\end{" + environment + "} without a matching \\begin{" + environment + "}");
                    }
                    envs.pop_back();
                }
                continue;
            }

            if (IsForbiddenMathCommand(command)) {
                return Invalid("E-MATH-FORBIDDEN-COMMAND", "command is not allowed in a math expression: "
                                                           "\\" +
                                                               command);
            }
            (void)command_start;
            continue;
        }

        ++i;
    }

    if (brace_depth != 0) {
        return Invalid("E-MATH-UNBALANCED-BRACES", "unbalanced braces in math source");
    }
    if (!envs.empty()) {
        return Invalid("E-MATH-UNBALANCED-ENV",
                       "\\begin{" + envs.back() + "} has no matching \\end{" + envs.back() + "}");
    }
    if (left_right != 0) {
        return Invalid("E-MATH-UNBALANCED-DELIMITER", "\\left has no matching \\right");
    }

    result.state = MathState::Valid;
    return result;
}

} // namespace pf
