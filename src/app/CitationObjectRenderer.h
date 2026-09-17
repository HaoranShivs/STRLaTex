#pragma once
// CitationObjectRenderer (citation plan §1): draws semantic inline objects -
// citations and cross references - as compact pills inside the QTextDocument.
//
// Design mirrors InlineMathObjectRenderer: the document reserves exactly one
// object replacement character (U+FFFC) per object; everything needed to
// paint it (kind, payload, display text) lives in the QTextCharFormat. The
// pill never participates in ordinary text editing, and Backspace/Delete
// removes the whole object because it is a single character.
//
// The displayed "[1]" / "[1, 3]" / "[?]" text is a pure projection of the
// citation-order numbering policy (CitationNumberResolver) - the document
// itself only ever stores the keys, and LaTeX/BibTeX produce the final
// numbers in the PDF.

#include <QAbstractTextDocumentLayout>
#include <QObject>
#include <QTextFormat>

namespace pf::gui {

namespace citation_format {
// Object type registered with the document layout. UserObject + 1 belongs to
// inline math; citations and cross references share this one.
inline constexpr int kObjectType = QTextFormat::UserObject + 2;
// The pill text to paint, e.g. "[1, 3]". Resolved at insert/load time.
inline constexpr int kDisplayTextProperty = QTextFormat::UserProperty + 7;
}  // namespace citation_format

// Format properties shared by every semantic inline object. InlineEditor
// writes them, ContentInRange() reads them back, and the pill renderer uses
// the kind for its tint. Declared here so both sides agree on the ids.
namespace inline_object_format {
inline constexpr int kKindProperty = QTextFormat::UserProperty + 1;
inline constexpr int kPayloadProperty = QTextFormat::UserProperty + 2;
}  // namespace inline_object_format

class CitationObjectRenderer final : public QObject,
                                     public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    explicit CitationObjectRenderer(QObject* parent = nullptr)
        : QObject(parent) {}

    QSizeF intrinsicSize(QTextDocument* document, int positionInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect,
                    QTextDocument* document, int positionInDocument,
                    const QTextFormat& format) override;
};

}  // namespace pf::gui
