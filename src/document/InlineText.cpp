#include "document/InlineText.h"

#include <algorithm>
#include <cctype>

namespace pf {

std::string InlineToPlainText(const InlineContent& content) {
    std::string out;
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            out += run->text;
        } else if (const auto* eq = std::get_if<InlineMath>(&node)) {
            out += eq->expression.latex;
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            out += "[cite:";
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i) out += ",";
                out += cit->keys[i];
            }
            out += "]";
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            out += "[ref:" + ref->target.value() + "]";
        }
    }
    return out;
}

bool InlineIsBlank(const InlineContent& content) {
    for (const auto& node : content) {
        const auto* run = std::get_if<TextRun>(&node);
        if (!run) return false;  // equations/citations/refs count as content
        for (char c : run->text) {
            if (!std::isspace(static_cast<unsigned char>(c))) return false;
        }
    }
    return true;
}

InlineContent InlineFromText(std::string text, std::uint8_t marks) {
    InlineContent content;
    if (text.empty() && marks == 0) return content;
    TextRun run;
    run.text = std::move(text);
    run.marks = marks;
    content.push_back(std::move(run));
    return content;
}

InlineContent InlineFromEditorText(std::string text) {
    InlineContent content;
    auto append_text = [&](std::string value) {
        if (value.empty()) return;
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
        const size_t ref = text.find("[ref:", cursor);
        size_t token = std::min(cite, ref);
        if (cite == std::string::npos) token = ref;
        if (ref == std::string::npos) token = cite;
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

        if (token == cite) {
            std::string keys = text.substr(token + 6, close - token - 6);
            Citation citation;
            size_t start = 0;
            while (start <= keys.size()) {
                size_t comma = keys.find(',', start);
                std::string key = keys.substr(
                    start, comma == std::string::npos
                               ? std::string::npos
                               : comma - start);
                const size_t first = key.find_first_not_of(" \t");
                const size_t last = key.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    citation.keys.push_back(
                        key.substr(first, last - first + 1));
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (citation.keys.empty()) {
                append_text(text.substr(token, close - token + 1));
            } else {
                content.push_back(std::move(citation));
            }
        } else {
            std::string target =
                text.substr(token + 5, close - token - 5);
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

// ---- Rich text (Stage B) ----

namespace {

// The fence that turns `marks` on/off in the editor representation. Emitted
// and parsed symmetrically so the round trip is lossless.
std::string FenceFor(std::uint8_t marks) {
    if (marks == (TextMark::Strong | TextMark::Emphasis)) return "***";
    if (marks == TextMark::Strong) return "**";
    if (marks == TextMark::Emphasis) return "*";
    return {};
}

// True when a word boundary is needed between the last character and a fence.
bool NeedsSpaceBeforeFence(const std::string& out) {
    if (out.empty()) return false;
    const char last = out.back();
    return !(last == ' ' || last == '\n' || last == '*');
}

}  // namespace

std::string InlineToRichText(const InlineContent& content) {
    std::string out;
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            const std::string fence = FenceFor(run->marks);
            if (fence.empty()) {
                out += run->text;
                continue;
            }
            if (NeedsSpaceBeforeFence(out)) out += ' ';
            out += fence + run->text + fence;
            // A fenced run followed by a letter would swallow the next word
            // when parsed, so close with a boundary as well.
            if (!out.empty() && out.back() == fence.back()) {
                // nothing extra: the closing fence already terminates the run
            }
        } else if (const auto* eq = std::get_if<InlineMath>(&node)) {
            // GenerateInlineMath's delimiter, so the readable editor spelling
            // matches the LaTeX the generator emits.
            out += "\\(" + eq->expression.latex + "\\)";
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            out += "[cite:";
            for (size_t i = 0; i < cit->keys.size(); ++i) {
                if (i) out += ",";
                out += cit->keys[i];
            }
            out += "]";
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            out += "[ref:" + ref->target.value() + "]";
        }
    }
    return out;
}

namespace {

// True when `pos` starts a fence of exactly `count` asterisks that reads as
// markup rather than as a literal asterisk in prose.
//
// Rules that keep "a * b" or "2***3" as text:
//   * not part of a longer asterisk run,
//   * either preceded by start-of-text/whitespace (opening) or preceded by
//     text (closing),
//   * and the character on the *content* side is not a space, so an opening
//     fence actually hugs the words it marks.
bool IsFence(const std::string& text, size_t pos, size_t count, bool opening) {
    if (pos + count > text.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        if (text[pos + i] != '*') return false;
    }
    if (pos + count < text.size() && text[pos + count] == '*') return false;
    const bool space_before =
        pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1]));
    if (opening && !space_before) return false;
    if (!opening && space_before) return false;  // a closer hugs the text
    const bool content_side_space =
        pos + count >= text.size() ||
        std::isspace(static_cast<unsigned char>(text[pos + count]));
    if (opening && content_side_space) return false;
    return true;
}

}  // namespace

InlineContent InlineFromRichText(const std::string& text) {
    InlineContent content;
    std::string plain;
    std::uint8_t marks = 0;

    // Close the pending run: merge into the previous run when marks match,
    // otherwise start a new one.
    auto flush = [&]() {
        if (plain.empty()) return;
        if (!content.empty()) {
            if (auto* previous = std::get_if<TextRun>(&content.back());
                previous && previous->marks == marks) {
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
        // Semantic tokens first: they are never markup.
        if (text.compare(i, 6, "[cite:") == 0 || text.compare(i, 5, "[ref:") == 0) {
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
        // Inline math. The canonical editor spelling is \(...\) because the
        // user never types the delimiters; $...$ is still accepted so files
        // written by the previous representation keep parsing.
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
        // Character marks. Longest fence wins, so *** is a single toggle.
        // A fence only counts as an opener/closer when it hugs the text, so
        // prose like "a * b" or "x * y * z" survives untouched.
        const std::uint8_t both = TextMark::Strong | TextMark::Emphasis;
        if (IsFence(text, i, 3, marks != both)) {
            flush();
            marks = (marks == both) ? 0 : both;
            i += 3;
            continue;
        }
        if (IsFence(text, i, 2, marks != TextMark::Strong)) {
            flush();
            marks = (marks == TextMark::Strong)
                        ? 0
                        : static_cast<uint8_t>(TextMark::Strong);
            i += 2;
            continue;
        }
        if (IsFence(text, i, 1, marks != TextMark::Emphasis)) {
            flush();
            marks = (marks == TextMark::Emphasis)
                        ? 0
                        : static_cast<uint8_t>(TextMark::Emphasis);
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
            if (run->marks != 0) return true;
        } else {
            return true;  // equation / citation / reference
        }
    }
    return false;
}

namespace {

// A line that starts like a list item, heading or quote keeps its own line.
bool StartsStructuredLine(const std::string& line) {
    if (line.empty()) return false;
    const char first = line[0];
    if (first == '-' || first == '*' || first == '+' || first == '>' ||
        first == '#' || first == '|') {
        return true;
    }
    // "1." / "1)" / "1、" style enumerations.
    size_t digits = 0;
    while (digits < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[digits]))) {
        ++digits;
    }
    if (digits > 0 && digits < line.size()) {
        const char next = line[digits];
        if (next == '.' || next == ')') return true;
        // "、" (U+3001) as an enumeration separator.
        if (line.compare(digits, 3, "\xe3\x80\x81") == 0) return true;
    }
    return false;
}

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool EndsWithBackslashBreak(const std::string& text) {
    return text.size() >= 2 && text.compare(text.size() - 2, 2, "\\\\") == 0;
}

}  // namespace

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
    if (lines.size() < 3) return std::string(text);  // not hard-wrapped

    // Anything carrying deliberate line structure is left alone.
    size_t short_lines = 0;
    for (const auto& raw : lines) {
        const std::string line = Trim(raw);
        if (line.empty()) continue;
        if (StartsStructuredLine(line)) return std::string(text);
        if (EndsWithBackslashBreak(line)) return std::string(text);
        if (line.size() < 20) ++short_lines;
    }
    if (short_lines * 2 > lines.size()) return std::string(text);

    // Merge single breaks into spaces; blank lines stay paragraph breaks.
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
            if (append_space && out.back() != ' ') out += ' ';
            out += line;
        }
        append_space = true;
        paragraph_break = false;
    }
    return out;
}

}  // namespace pf
