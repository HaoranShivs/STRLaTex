#include "math/MathValidator.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace pf {

namespace {

const std::set<std::string>& ForbiddenCommands() {
    static const std::set<std::string> commands = {
        // Document class / packages / preamble
        "documentclass", "documentstyle", "usepackage", "RequirePackage",
        "LoadClass", "PassOptionsToPackage", "geometry", "hypersetup",
        // Document structure. (\begin / \end are handled explicitly by the
        // scanner and never reach this set.)
        "part", "chapter", "section", "subsection", "subsubsection",
        "paragraph", "subparagraph", "appendix", "tableofcontents",
        "listoffigures", "listoftables", "frontmatter", "mainmatter",
        "backmatter", "maketitle", "title", "author", "date", "thanks",
        "affiliation", "institute", "keywords", "abstract", "bibliography",
        "bibliographystyle", "include", "includeonly", "input", "import",
        "subimport", "pagestyle", "thispagestyle", "pagenumbering",
        // Macro / environment definition - user macros are out of scope
        "newcommand", "renewcommand", "providecommand", "DeclareMathOperator",
        "def", "edef", "gdef", "xdef", "let", "newenvironment",
        "renewenvironment", "newcounter", "setcounter", "addtocounter",
        "newlength", "setlength", "newsavebox", "sbox",
        // Formula environment / numbering controls: owned by STRTeX
        "label", "tag", "numberwithin", "nonumber", "notag",
    };
    return commands;
}

const std::set<std::string>& ForbiddenEnvironments() {
    // Outer formula environments and every non-math document environment.
    // Math-internal ones (aligned, gathered, split, cases, matrix,
    // smallmatrix, array, ...) are deliberately absent.
    static const std::set<std::string> environments = {
        "document", "equation", "equation*", "displaymath", "math",
        "eqnarray", "eqnarray*", "align", "align*", "alignat", "alignat*",
        "flalign", "flalign*", "gather", "gather*", "multline", "multline*",
        "subequations", "figure", "figure*", "table", "table*", "tabular",
        "tabular*", "tabbing", "verbatim", "verbatim*", "lstlisting",
        "minted", "tikzpicture", "pgfpicture", "picture", "minipage",
        "center", "flushleft", "flushright", "itemize", "enumerate",
        "description", "quote", "quotation", "verse", "thebibliography",
        "abstract", "titlepage", "frame", "columns", "column",
    };
    return environments;
}

bool IsAsciiAlpha(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

// Read a LaTeX control word / control symbol starting at the backslash.
// Returns the command name (without the backslash) and advances `i` past it.
std::string ReadCommand(const std::string& text, size_t* i) {
    size_t pos = *i + 1;  // skip backslash
    if (pos >= text.size()) {
        *i = pos;
        return {};
    }
    if (!IsAsciiAlpha(text[pos])) {
        // Control symbol: exactly one character (e.g. \\ \{ \, \) )
        const std::string name(1, text[pos]);
        *i = pos + 1;
        return name;
    }
    const size_t start = pos;
    while (pos < text.size() && IsAsciiAlpha(text[pos])) ++pos;
    *i = pos;
    return text.substr(start, pos - start);
}

// After \begin / \end, read the {environment} argument. Empty when malformed.
std::string ReadEnvironmentName(const std::string& text, size_t* i) {
    size_t pos = *i;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != '{') return {};
    ++pos;
    std::string name;
    while (pos < text.size() && text[pos] != '}') {
        name.push_back(text[pos]);
        ++pos;
    }
    if (pos >= text.size()) return {};
    *i = pos + 1;  // past '}'
    // Trim surrounding whitespace.
    const size_t first = name.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
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

}  // namespace

bool IsForbiddenMathCommand(const std::string& command) {
    return ForbiddenCommands().count(command) > 0;
}

bool IsForbiddenMathEnvironment(const std::string& environment) {
    return ForbiddenEnvironments().count(environment) > 0;
}

MathValidation ValidateMath(const std::string& latex, MathFlavor /*flavor*/) {
    MathValidation result;

    // Empty source is not an error - it is an expression the user has not
    // finished typing yet (design §8: Pending).
    const size_t first = latex.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        result.state = MathState::Pending;
        result.code = "E-MATH-EMPTY";
        result.error = "math expression is empty";
        return result;
    }

    int brace_depth = 0;
    int left_right = 0;               // \left ... \right balance
    std::vector<std::string> envs;    // open \begin{...} stack

    size_t i = 0;
    while (i < latex.size()) {
        const char c = latex[i];

        if (c == '$') {
            return Invalid("E-MATH-DELIMITER",
                           "math source must not contain '$': inline delimiters "
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
                return Invalid("E-MATH-UNBALANCED-BRACES",
                               "unbalanced '}' in math source");
            }
            ++i;
            continue;
        }

        if (c == '\\') {
            const size_t command_start = i;
            const std::string command = ReadCommand(latex, &i);
            if (command.empty()) continue;  // a trailing backslash

            // \\ is a line break; \\[2pt] carries optional spacing, so the
            // '[' after it is not a display delimiter.
            if (command == "\\") continue;

            if (command == "(" || command == ")" || command == "[" ||
                command == "]") {
                return Invalid(
                    "E-MATH-DELIMITER",
                    "math source must not contain \\" + command +
                        ": delimiters are generated by STRTeX");
            }

            if (command == "left") {
                ++left_right;
                continue;
            }
            if (command == "right") {
                --left_right;
                if (left_right < 0) {
                    return Invalid("E-MATH-UNBALANCED-DELIMITER",
                                   "\\right without a matching \\left");
                }
                continue;
            }

            if (command == "begin" || command == "end") {
                const std::string environment =
                    ReadEnvironmentName(latex, &i);
                if (environment.empty()) {
                    return Invalid("E-MATH-MALFORMED-ENV",
                                   "malformed \\" + command +
                                       "{...} in math source");
                }
                if (IsForbiddenMathEnvironment(environment)) {
                    return Invalid("E-MATH-FORBIDDEN-ENV",
                                   "environment is not allowed in a math "
                                   "expression: " + environment);
                }
                if (command == "begin") {
                    envs.push_back(environment);
                } else {
                    if (envs.empty() || envs.back() != environment) {
                        return Invalid("E-MATH-UNBALANCED-ENV",
                                       "\\end{" + environment +
                                           "} without a matching \\begin{" +
                                           environment + "}");
                    }
                    envs.pop_back();
                }
                continue;
            }

            if (IsForbiddenMathCommand(command)) {
                return Invalid("E-MATH-FORBIDDEN-COMMAND",
                               "command is not allowed in a math expression: "
                               "\\" + command);
            }
            (void)command_start;
            continue;
        }

        ++i;
    }

    if (brace_depth != 0) {
        return Invalid("E-MATH-UNBALANCED-BRACES",
                       "unbalanced braces in math source");
    }
    if (!envs.empty()) {
        return Invalid("E-MATH-UNBALANCED-ENV",
                       "\\begin{" + envs.back() + "} has no matching \\end{" +
                           envs.back() + "}");
    }
    if (left_right != 0) {
        return Invalid("E-MATH-UNBALANCED-DELIMITER",
                       "\\left has no matching \\right");
    }

    result.state = MathState::Valid;
    return result;
}

}  // namespace pf
