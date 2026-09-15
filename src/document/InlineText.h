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

// ---- Rich text (Stage B) ----

// Editor representation that also carries character marks. Tokens stay
// readable ([cite:key] / [ref:node-id]) so the string stays diffable and the
// legacy plain-text path keeps working; marks are attached to text with the
// delimiters below, which never appear in normal prose:
//   **bold**, *italic*, ***bold italic***
// Equations become $math$. Round trip: InlineFromRichText(InlineToRichText(x))
// preserves every run's marks and every semantic node.
std::string InlineToRichText(const InlineContent& content);

// Inverse of InlineToRichText. Unknown markup is kept as literal text, so a
// document never loses content because of a parse miss.
InlineContent InlineFromRichText(const std::string& text);

// True when the content has any character marks, equations, citations or
// references - i.e. anything a plain text field would flatten.
bool InlineIsRich(const InlineContent& content);

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
