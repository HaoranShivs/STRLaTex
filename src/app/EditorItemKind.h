#pragma once
// EditorItemKind: how the GUI classifies a row in the editor (plan §2.2).
//
// The domain model keeps the Structure/Block distinction it always had
// (Section/Subsection/Subsubsection are structure nodes; Paragraph/Figure/
// Table/EquationBlock are blocks). The GUI used to flatten both into the
// same "kind" string, which is why a heading row and a text row were hard to
// tell apart and why the insert menu could not be filtered by where you are.

#include <QString>

#include "document/Document.h"

namespace pf::gui {

enum class EditorItemKind {
    // Front matter
    PaperTitle,
    Authors,
    Affiliations,
    Abstract,
    Keywords,

    // Structure
    SectionTitle,
    SubsectionTitle,
    SubsubsectionTitle,

    // Content
    Text,
    Equation,
    Figure,
    Table,
};

// Human-facing label, e.g. "Section Title", "Text".
QString EditorItemLabel(EditorItemKind kind);

// Stable machine name used by the insert menu / "/" command payload.
QString EditorItemKindName(EditorItemKind kind);

// Parse back. Returns false for unknown names.
bool EditorItemKindFromName(const QString& name, EditorItemKind* out);

// Classify a document node. Front matter has its own kinds and is not derived
// from NodeKind.
EditorItemKind EditorItemKindOfNode(NodeKind kind);

}  // namespace pf::gui
