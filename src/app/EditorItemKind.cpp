#include "app/EditorItemKind.h"

namespace pf::gui {

namespace {
struct KindNames {
    EditorItemKind kind;
    const char* label;
    const char* name;
};
constexpr KindNames kKinds[] = {
    {EditorItemKind::PaperTitle, "Paper Title", "paper-title"},
    {EditorItemKind::Authors, "Authors", "authors"},
    {EditorItemKind::Affiliations, "Affiliations", "affiliations"},
    {EditorItemKind::Abstract, "Abstract", "abstract"},
    {EditorItemKind::Keywords, "Keywords", "keywords"},
    {EditorItemKind::SectionTitle, "Section Title", "section"},
    {EditorItemKind::SubsectionTitle, "Subsection Title", "subsection"},
    {EditorItemKind::SubsubsectionTitle, "Subsubsection Title", "subsubsection"},
    {EditorItemKind::Text, "Text", "text"},
    {EditorItemKind::Equation, "Equation", "equation"},
    {EditorItemKind::Figure, "Figure", "figure"},
    {EditorItemKind::Table, "Table", "table"},
};
}  // namespace

QString EditorItemLabel(EditorItemKind kind) {
    for (const auto& entry : kKinds) {
        if (entry.kind == kind) return QString::fromUtf8(entry.label);
    }
    return QStringLiteral("Unknown");
}

QString EditorItemKindName(EditorItemKind kind) {
    for (const auto& entry : kKinds) {
        if (entry.kind == kind) return QString::fromUtf8(entry.name);
    }
    return QStringLiteral("unknown");
}

bool EditorItemKindFromName(const QString& name, EditorItemKind* out) {
    for (const auto& entry : kKinds) {
        if (name == QLatin1String(entry.name)) {
            if (out) *out = entry.kind;
            return true;
        }
    }
    return false;
}

EditorItemKind EditorItemKindOfNode(NodeKind kind) {
    switch (kind) {
        case NodeKind::Section: return EditorItemKind::SectionTitle;
        case NodeKind::Subsection: return EditorItemKind::SubsectionTitle;
        case NodeKind::Subsubsection: return EditorItemKind::SubsubsectionTitle;
        case NodeKind::Paragraph: return EditorItemKind::Text;
        case NodeKind::Figure: return EditorItemKind::Figure;
        case NodeKind::Table: return EditorItemKind::Table;
        case NodeKind::Equation: return EditorItemKind::Equation;
    }
    return EditorItemKind::Text;
}

}  // namespace pf::gui
