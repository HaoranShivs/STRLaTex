#include "document/InlineText.h"

#include <algorithm>
#include <cctype>

namespace pf {

std::string InlineToPlainText(const InlineContent& content) {
    std::string out;
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            out += run->text;
        } else if (const auto* eq = std::get_if<InlineEquation>(&node)) {
            out += eq->math_source;
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

}  // namespace pf
