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

}  // namespace pf
