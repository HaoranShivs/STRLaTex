#include "document/InlineText.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <sstream>

namespace pf {

std::string InlineToPlainText(const InlineContent& content) {
    std::string out;
    int footnote = 0;
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            out += run->text;
        } else if (const auto* eq = std::get_if<InlineMath>(&node)) {
            out += eq->expression.latex;
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            // Narrative 使用 [citet:]，Parenthetical 保持旧的 [cite:] 写法：
            // 旧文本仍然可读，模式也不会在纯文本投影里丢失。
            out += cit->mode == CitationMode::Narrative ? "[citet:" : "[cite:";
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i)
                    out += ",";
                out += cit->keys[i];
            }
            out += "]";
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            out += "[ref:" + ref->target.value() + "]";
        } else if (std::holds_alternative<Footnote>(node)) {
            out += "[^" + std::to_string(++footnote) + "]";
        }
    }
    return out;
}

bool InlineIsBlank(const InlineContent& content) {
    for (const auto& node : content) {
        const auto* run = std::get_if<TextRun>(&node);
        if (!run)
            return false; // 公式/引文/引用都算作内容
        for (char c : run->text) {
            if (!std::isspace(static_cast<unsigned char>(c)))
                return false;
        }
    }
    return true;
}

InlineContent InlineFromText(std::string text, std::uint8_t marks) {
    InlineContent content;
    if (text.empty() && marks == 0)
        return content;
    TextRun run;
    run.text = std::move(text);
    run.marks = marks;
    content.push_back(std::move(run));
    return content;
}

InlineContent InlineFromEditorText(std::string text) {
    InlineContent content;
    auto append_text = [&](std::string value) {
        if (value.empty())
            return;
        if (!content.empty()) {
            if (auto* previous = std::get_if<TextRun>(&content.back())) {
                previous->text += value;
                return;
            }
        }
        content.push_back(TextRun{std::move(value), 0});
    };

    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t cite = text.find("[cite:", cursor);
        const size_t citet = text.find("[citet:", cursor);
        const size_t ref = text.find("[ref:", cursor);
        const size_t token = std::min({cite, citet, ref});
        if (token == std::string::npos) {
            append_text(text.substr(cursor));
            break;
        }
        append_text(text.substr(cursor, token - cursor));
        const size_t close = text.find(']', token);
        if (close == std::string::npos) {
            append_text(text.substr(token));
            break;
        }

        const bool narrative = token == citet;
        if (token == cite || narrative) {
            const size_t open_length = narrative ? 7 : 6; // "[citet:" / "[cite:"
            std::string keys = text.substr(token + open_length, close - token - open_length);
            Citation citation;
            citation.mode = narrative ? CitationMode::Narrative : CitationMode::Parenthetical;
            size_t start = 0;
            while (start <= keys.size()) {
                size_t comma = keys.find(',', start);
                std::string key = keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const size_t first = key.find_first_not_of(" \t");
                const size_t last = key.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    citation.keys.push_back(key.substr(first, last - first + 1));
                }
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            if (citation.keys.empty()) {
                append_text(text.substr(token, close - token + 1));
            } else {
                content.push_back(std::move(citation));
            }
        } else {
            std::string target = text.substr(token + 5, close - token - 5);
            if (target.empty()) {
                append_text(text.substr(token, close - token + 1));
            } else {
                content.push_back(CrossReference{NodeId(std::move(target))});
            }
        }
        cursor = close + 1;
    }
    return content;
}

// ---- 富文本（阶段 B）----

namespace {

// 在编辑器表示形式中开关 `marks` 的围栏标记。生成与解析保持对称，
// 因此往返无损。
std::string FenceFor(std::uint8_t marks) {
    if (marks == (TextMark::Strong | TextMark::Emphasis))
        return "***";
    if (marks == TextMark::Strong)
        return "**";
    if (marks == TextMark::Emphasis)
        return "*";
    return {};
}

// 当最后一个字符与围栏之间需要词边界时为真。
bool NeedsSpaceBeforeFence(const std::string& out) {
    if (out.empty())
        return false;
    const char last = out.back();
    return !(last == ' ' || last == '\n' || last == '*');
}

} // namespace

std::string InlineToRichText(const InlineContent& content) {
    std::string out;
    int footnote = 0;
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            const std::string fence = FenceFor(run->marks);
            if (fence.empty()) {
                out += run->text;
                continue;
            }
            if (NeedsSpaceBeforeFence(out))
                out += ' ';
            out += fence + run->text + fence;
        } else if (const auto* eq = std::get_if<InlineMath>(&node)) {
            // 采用 GenerateInlineMath 的分隔符，使可读的编辑器写法
            // 与生成器输出的 LaTeX 一致。
            out += "\\(" + eq->expression.latex + "\\)";
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            // Narrative 使用 [citet:]，Parenthetical 保持旧的 [cite:] 写法：
            // 旧文本仍然可读，模式也不会在纯文本投影里丢失。
            out += cit->mode == CitationMode::Narrative ? "[citet:" : "[cite:";
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i)
                    out += ",";
                out += cit->keys[i];
            }
            out += "]";
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            out += "[ref:" + ref->target.value() + "]";
        } else if (std::holds_alternative<Footnote>(node)) {
            out += "[^" + std::to_string(++footnote) + "]";
        }
    }
    return out;
}

namespace {

std::string HexEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        out += kHex[c >> 4];
        out += kHex[c & 0x0F];
    }
    return out;
}

std::optional<std::string> HexDecode(const std::string& value) {
    if (value.size() % 2 != 0)
        return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int hi = digit(value[i]);
        const int lo = digit(value[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

// 当 `pos` 处开始的、恰好由 `count` 个星号组成的围栏被解读为标记而非
// 行文中的字面星号时为真。
//
// 让 "a * b" 或 "2***3" 保持为文本的规则：
//   * 不属于更长的星号连续段，
//   * 前面要么是文本起始/空白（开启），要么是文本（闭合），
//   * 且*内容*一侧的字符不是空格，这样开启围栏才真正紧贴它所标记的词。
bool IsFence(const std::string& text, size_t pos, size_t count, bool opening) {
    if (pos + count > text.size())
        return false;
    for (size_t i = 0; i < count; ++i) {
        if (text[pos + i] != '*')
            return false;
    }
    if (pos + count < text.size() && text[pos + count] == '*')
        return false;
    const bool space_before = pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1]));
    if (opening && !space_before)
        return false;
    if (!opening && space_before)
        return false; // 闭合标记紧贴文本
    const bool content_side_space =
        pos + count >= text.size() || std::isspace(static_cast<unsigned char>(text[pos + count]));
    if (opening && content_side_space)
        return false;
    return true;
}

} // namespace

InlineContent InlineFromRichText(const std::string& text) {
    InlineContent content;
    std::string plain;
    std::uint8_t marks = 0;

    // 结算待处理的 run：标记相同时并入上一个 run，否则新建一个。
    auto flush = [&]() {
        if (plain.empty())
            return;
        if (!content.empty()) {
            if (auto* previous = std::get_if<TextRun>(&content.back()); previous && previous->marks == marks) {
                previous->text += plain;
                plain.clear();
                return;
            }
        }
        content.push_back(TextRun{plain, marks});
        plain.clear();
    };

    size_t i = 0;
    while (i < text.size()) {
        // 优先处理语义 token：它们绝不是标记。
        if (text.compare(i, 7, "[citet:") == 0 || text.compare(i, 6, "[cite:") == 0 ||
            text.compare(i, 5, "[ref:") == 0) {
            const size_t close = text.find(']', i);
            if (close != std::string::npos) {
                flush();
                for (auto& node : InlineFromEditorText(text.substr(i, close - i + 1))) {
                    content.push_back(std::move(node));
                }
                i = close + 1;
                continue;
            }
        }
        // 内联公式。规范的编辑器写法是 \(...\)，因为用户从不输入分隔符；
        // $...$ 仍然接受，以便按旧表示形式写出的文件继续可解析。
        if (text.compare(i, 2, "\\(") == 0) {
            const size_t close = text.find("\\)", i + 2);
            if (close != std::string::npos && close > i + 2) {
                flush();
                InlineMath eq;
                eq.expression.latex = text.substr(i + 2, close - i - 2);
                content.push_back(std::move(eq));
                i = close + 2;
                continue;
            }
        }
        if (text[i] == '$') {
            const size_t close = text.find('$', i + 1);
            if (close != std::string::npos && close > i + 1) {
                flush();
                InlineMath eq;
                eq.expression.latex = text.substr(i + 1, close - i - 1);
                content.push_back(std::move(eq));
                i = close + 1;
                continue;
            }
        }
        // 字符标记。最长围栏优先，因此 *** 是一次整体开关。
        // 围栏只有在紧贴文本时才算作开启/闭合，因此
        // "a * b" 或 "x * y * z" 这类行文原样保留。
        const std::uint8_t both = TextMark::Strong | TextMark::Emphasis;
        if (IsFence(text, i, 3, marks != both)) {
            flush();
            marks = (marks == both) ? 0 : both;
            i += 3;
            continue;
        }
        if (IsFence(text, i, 2, marks != TextMark::Strong)) {
            flush();
            marks = (marks == TextMark::Strong) ? 0 : static_cast<uint8_t>(TextMark::Strong);
            i += 2;
            continue;
        }
        if (IsFence(text, i, 1, marks != TextMark::Emphasis)) {
            flush();
            marks = (marks == TextMark::Emphasis) ? 0 : static_cast<uint8_t>(TextMark::Emphasis);
            i += 1;
            continue;
        }
        plain.push_back(text[i]);
        ++i;
    }
    flush();
    return content;
}

bool InlineIsRich(const InlineContent& content) {
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            if (run->marks != 0)
                return true;
        } else {
            return true; // 公式/引文/引用
        }
    }
    return false;
}

namespace {

// 以列表项、标题或引用开头的那类行各自独占一行。
bool StartsStructuredLine(const std::string& line) {
    if (line.empty())
        return false;
    const char first = line[0];
    if (first == '-' || first == '*' || first == '+' || first == '>' || first == '#' || first == '|') {
        return true;
    }
    // "1." / "1)" / "1、" 形式的枚举。
    size_t digits = 0;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits]))) {
        ++digits;
    }
    if (digits > 0 && digits < line.size()) {
        const char next = line[digits];
        if (next == '.' || next == ')')
            return true;
        // 以 "、"（U+3001）作为枚举分隔符。
        if (line.compare(digits, 3, "\xe3\x80\x81") == 0)
            return true;
    }
    return false;
}

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool EndsWithBackslashBreak(const std::string& text) {
    return text.size() >= 2 && text.compare(text.size() - 2, 2, "\\\\") == 0;
}

} // namespace

std::string ReflowHardWrappedText(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(current);
    if (lines.size() < 3)
        return std::string(text); // 并非硬折行

    // 任何带有刻意行结构的文本都保持原样。
    size_t short_lines = 0;
    for (const auto& raw : lines) {
        const std::string line = Trim(raw);
        if (line.empty())
            continue;
        if (StartsStructuredLine(line))
            return std::string(text);
        if (EndsWithBackslashBreak(line))
            return std::string(text);
        if (line.size() < 20)
            ++short_lines;
    }
    if (short_lines * 2 > lines.size())
        return std::string(text);

    // 将单个换行合并为空格；空行仍保留为段落分隔。
    std::string out;
    bool append_space = false;
    bool paragraph_break = false;
    for (const auto& raw : lines) {
        const std::string line = Trim(raw);
        if (line.empty()) {
            append_space = false;
            paragraph_break = !out.empty();
            continue;
        }
        if (out.empty()) {
            out = line;
        } else if (paragraph_break) {
            out += "\n\n";
            out += line;
        } else {
            if (append_space && out.back() != ' ')
                out += ' ';
            out += line;
        }
        append_space = true;
        paragraph_break = false;
    }
    return out;
}


std::string InlineToClipboard(const InlineContent& content) {
    std::string main;
    std::vector<std::pair<int, std::string>> footnotes;
    int next_id = 1;
    auto append = [&](const InlineContent& source, bool allow_footnotes) {
        for (const auto& node : source) {
            if (const auto* run = std::get_if<TextRun>(&node)) {
                main += InlineToRichText(InlineContent{*run});
            } else if (const auto* math = std::get_if<InlineMath>(&node)) {
                main += InlineToRichText(InlineContent{*math});
            } else if (const auto* cit = std::get_if<Citation>(&node)) {
                main += InlineToRichText(InlineContent{*cit});
            } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
                main += InlineToRichText(InlineContent{*ref});
            } else if (const auto* footnote = std::get_if<Footnote>(&node)) {
                if (!allow_footnotes || !footnote->body) {
                    main += "[^]";
                    continue;
                }
                const int id = next_id++;
                main += '\x01';
                main += std::to_string(id);
                main += '\x01';
                footnotes.emplace_back(id, InlineToRichText(footnote->body->content));
            }
        }
    };
    append(content, true);
    std::string out = "PFINLINE2\n";
    out += HexEncode(main);
    out += "\n";
    for (const auto& [id, body] : footnotes) {
        out += std::to_string(id);
        out += '\t';
        out += HexEncode(body);
        out += '\n';
    }
    return out;
}

std::optional<InlineContent> InlineFromClipboard(const std::string& text) {
    const std::string header = "PFINLINE2\n";
    if (text.rfind(header, 0) != 0)
        return std::nullopt;
    const size_t main_start = header.size();
    const size_t main_end = text.find('\n', main_start);
    if (main_end == std::string::npos)
        return std::nullopt;
    const auto main = HexDecode(text.substr(main_start, main_end - main_start));
    if (!main)
        return std::nullopt;
    std::map<int, InlineContent> bodies;
    size_t cursor = main_end + 1;
    while (cursor < text.size()) {
        const size_t line_end = text.find('\n', cursor);
        const std::string line = text.substr(cursor, line_end == std::string::npos ? std::string::npos
                                                                                 : line_end - cursor);
        if (!line.empty()) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos)
                return std::nullopt;
            int id = 0;
            const std::string id_text = line.substr(0, tab);
            if (id_text.empty() || id_text.find_first_not_of("0123456789") != std::string::npos)
                return std::nullopt;
            id = std::stoi(id_text);
            const auto body_text = HexDecode(line.substr(tab + 1));
            if (!body_text)
                return std::nullopt;
            bodies[id] = InlineFromRichText(*body_text);
        }
        if (line_end == std::string::npos)
            break;
        cursor = line_end + 1;
    }

    InlineContent parsed = InlineFromRichText(*main);
    InlineContent out;
    for (auto& node : parsed) {
        auto* run = std::get_if<TextRun>(&node);
        if (!run) {
            out.push_back(std::move(node));
            continue;
        }
        std::string text_value = run->text;
        size_t pos = 0;
        while (pos < text_value.size()) {
            const size_t open = text_value.find('\x01', pos);
            if (open == std::string::npos) {
                if (pos < text_value.size())
                    out.push_back(TextRun{text_value.substr(pos), run->marks});
                break;
            }
            if (open > pos)
                out.push_back(TextRun{text_value.substr(pos, open - pos), run->marks});
            const size_t close = text_value.find('\x01', open + 1);
            if (close == std::string::npos)
                return std::nullopt;
            const std::string id_text = text_value.substr(open + 1, close - open - 1);
            if (id_text.empty() || id_text.find_first_not_of("0123456789") != std::string::npos)
                return std::nullopt;
            const int id = std::stoi(id_text);
            const auto body = bodies.find(id);
            if (body == bodies.end())
                return std::nullopt;
            out.push_back(MakeFootnote(body->second));
            pos = close + 1;
        }
    }
    return out;
}

} // namespace pf
