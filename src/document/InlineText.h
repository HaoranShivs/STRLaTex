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

// Undo the hard line breaks of a text that was copied out of a PDF or another
// word processor. Such a paste arrives pre-wrapped at a fixed column, so the
// paragraph can never re-flow to the editor width.
//
// A single line break carries no meaning in the document model (LaTeX treats
// it as a space), so single breaks become spaces and blank lines stay as
// paragraph breaks. Text that looks structured - list items, explicit LaTeX
// breaks, or preformatted blocks - is returned unchanged.
std::string ReflowHardWrappedText(std::string_view text);

}  // namespace pf
