#include "document/InlineText.h"

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

}  // namespace pf
