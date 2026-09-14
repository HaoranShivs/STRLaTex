#pragma once
// InlineContent utilities: plain-text extraction, concatenation, splitting.
#include <string>
#include <vector>

#include "document/Document.h"

namespace pf {

// Extract plain text from inline content (for searching / outline).
std::string InlineToPlainText(const InlineContent& content);

// Check whether inline content is empty or whitespace-only.
bool InlineIsBlank(const InlineContent& content);

// Build single TextRun inline content.
InlineContent InlineFromText(std::string text, std::uint8_t marks = 0);

// Parse the stable editor representation produced by InlineToPlainText.
// Recognizes [cite:key,key2] and [ref:node-id], preserving semantic inline
// nodes when a user edits the surrounding paragraph text.
InlineContent InlineFromEditorText(std::string text);

}  // namespace pf
