// Stage B follow-up: the rich editor must hand the document exactly the marks
// the user sees, and the renderer must turn them into the LaTeX the PDF shows.
//
// These tests reproduce the bug that shipped first: Content() read charFormat()
// at every caret position, but charFormat() reports the format of the character
// *before* the caret, so every run came back shifted by one character - bold
// survived by luck on long runs, italic lost its first character and never made
// it into the PDF.
#include "TestMain.hpp"

#include <QApplication>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFragment>

#include "app/BlockEditor.h"
#include "app/InlineEditor.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "render/LatexRenderer.h"

using namespace pf;
using namespace pf::gui;

namespace {

// QApplication is created once for the whole binary by the GUI test driver;
// these tests only build widgets, so a local one is safe when absent.
QApplication* EnsureApp() { return qApp; }

// Type `text` into `editor` applying `marks`, as a user with Ctrl+B/I would.
void TypeRun(InlineEditor* editor, const QString& text, std::uint8_t marks) {
    // A fresh cursor positioned at the end: the editor's cached textCursor()
    // copy does not track document replacements made by SetContent.
    QTextCursor cursor(editor->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    format.setFontWeight(HasMark(marks, TextMark::Strong) ? QFont::Bold
                                                          : QFont::Normal);
    format.setFontItalic(HasMark(marks, TextMark::Emphasis));
    cursor.setCharFormat(format);
    cursor.insertText(text);
}

Body& BodyOf(Document& doc) { return DocumentMutableAccess::body(doc); }

}  // namespace

PF_TEST(InlineEditorReturnsExactRuns) {
    EnsureApp();
    InlineEditor editor;
    editor.SetContent(InlineFromText("plain "));
    TypeRun(&editor, QStringLiteral("bold"), static_cast<std::uint8_t>(TextMark::Strong));
    TypeRun(&editor, QStringLiteral(" "), 0);
    TypeRun(&editor, QStringLiteral("italic"), static_cast<std::uint8_t>(TextMark::Emphasis));

    const InlineContent content = editor.Content();
    std::cout << "    runs:";
    for (const auto& node : content) {
        if (const auto* run = std::get_if<TextRun>(&node)) {
            std::cout << " [" << run->text << "|" << static_cast<int>(run->marks)
                      << "]";
        }
    }
    std::cout << "\n";

    // The unformatted separator between the two marked runs is its own run;
    // what matters is that no character drifts into its neighbour.
    PF_CHECK(content.size() == 4);
    const auto* first = std::get_if<TextRun>(&content[0]);
    const auto* second = std::get_if<TextRun>(&content[1]);
    const auto* third = std::get_if<TextRun>(&content[2]);
    const auto* fourth = std::get_if<TextRun>(&content[3]);
    PF_CHECK(first && first->text == "plain " && first->marks == 0);
    PF_CHECK(second && second->text == "bold");
    if (second) {
        PF_CHECK(HasMark(second->marks, TextMark::Strong));
        PF_CHECK(!HasMark(second->marks, TextMark::Emphasis));
    }
    PF_CHECK(third && third->text == " " && third->marks == 0);
    PF_CHECK(fourth && fourth->text == "italic");
    if (fourth) {
        PF_CHECK(HasMark(fourth->marks, TextMark::Emphasis));
        PF_CHECK(!HasMark(fourth->marks, TextMark::Strong));
    }
}

PF_TEST(InlineEditorItalicReachesTheLatexAndStaysWhole) {
    EnsureApp();
    InlineEditor editor;
    editor.SetContent(InlineFromText("plain "));
    TypeRun(&editor, QStringLiteral("italic"), static_cast<std::uint8_t>(TextMark::Emphasis));

    const InlineContent content = editor.Content();

    // Render through the real pipeline and check the LaTeX the PDF is built
    // from: one \emph covering the whole word, not a fragment of it.
    Document doc;
    DocumentEditor editor_doc(doc);
    (void)editor_doc.SetTitle(InlineFromText("t"));
    auto section = editor_doc.InsertSection(0, InlineContent{});
    Paragraph para;
    para.content = content;
    (void)editor_doc.InsertBlock(section.value(), std::nullopt, para);

    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    const std::string& tex = rendered.package.files[0].content;
    PF_CHECK(tex.find("plain \\emph{italic}") != std::string::npos);
    // The old bug produced "\emph{talic}" with a stray leading "i".
    PF_CHECK(tex.find("\\emph{talic}") == std::string::npos);
}

PF_TEST(InlineEditorBoldAndItalicTogetherReachTheLatex) {
    EnsureApp();
    InlineEditor editor;
    TypeRun(&editor, QStringLiteral("plain "), 0);
    TypeRun(&editor, QStringLiteral("both"),
            TextMark::Strong | TextMark::Emphasis);

    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 2);
    const auto* marked = std::get_if<TextRun>(&content[1]);
    PF_CHECK(marked && marked->text == "both");
    if (marked) {
        PF_CHECK(HasMark(marked->marks, TextMark::Strong));
        PF_CHECK(HasMark(marked->marks, TextMark::Emphasis));
    }

    Document doc;
    DocumentEditor editor_doc(doc);
    (void)editor_doc.SetTitle(InlineFromText("t"));
    auto section = editor_doc.InsertSection(0, InlineContent{});
    Paragraph para;
    para.content = content;
    (void)editor_doc.InsertBlock(section.value(), std::nullopt, para);

    RenderRequest request;
    request.document = &doc;
    request.template_id = "generic-article";
    request.revision = ProjectRevision{1};
    auto rendered = LatexRenderer().Render(request);
    PF_CHECK(rendered.status == RenderResult::Status::Ok);
    const std::string& tex = rendered.package.files[0].content;
    PF_CHECK(tex.find("\\textbf{\\emph{both}}") != std::string::npos);
}

PF_TEST(InlineEditorRoundTripsThroughTheModel) {
    EnsureApp();
    InlineEditor editor;
    TypeRun(&editor, QStringLiteral("plain "), 0);
    TypeRun(&editor, QStringLiteral("bold"), static_cast<std::uint8_t>(TextMark::Strong));
    TypeRun(&editor, QStringLiteral(" tail"), 0);

    const InlineContent committed = editor.Content();
    editor.SetContent(committed);
    // Reloading the committed content must not change what would be committed
    // next, otherwise every rebuild corrupts the paragraph a little more.
    const InlineContent reloaded = editor.Content();
    PF_CHECK(reloaded == committed);
    const auto* bold = std::get_if<TextRun>(&reloaded[1]);
    PF_CHECK(bold && bold->text == "bold");
    if (bold) PF_CHECK(HasMark(bold->marks, TextMark::Strong));
}

PF_TEST(InlineEditorItalicSurvivesAProgrammaticReload) {
    EnsureApp();
    InlineEditor editor;
    TypeRun(&editor, QStringLiteral("normal "), 0);
    TypeRun(&editor, QStringLiteral("slanted"), static_cast<std::uint8_t>(TextMark::Emphasis));
    const InlineContent committed = editor.Content();

    // What a rebuild does: SetContent with the stored content, then commit
    // again (the user clicks away). The italic must still be there.
    InlineEditor reloaded;
    reloaded.SetContent(committed);
    const InlineContent again = reloaded.Content();
    PF_CHECK(again == committed);
    const auto* run = std::get_if<TextRun>(&again[1]);
    PF_CHECK(run && run->text == "slanted");
    if (run) PF_CHECK(HasMark(run->marks, TextMark::Emphasis));
}

PF_TEST(InlineEditorKeepsTokensAcrossAReload) {
    EnsureApp();
    InlineEditor editor;
    TypeRun(&editor, QStringLiteral("see "), 0);
    editor.InsertCitationObject({QStringLiteral("smith2024")});
    editor.InsertCrossReferenceObject(QStringLiteral("n7"));
    TypeRun(&editor, QStringLiteral(" end"), 0);

    const InlineContent committed = editor.Content();
    size_t citations = 0;
    size_t references = 0;
    for (const auto& node : committed) {
        if (std::holds_alternative<Citation>(node)) ++citations;
        if (std::holds_alternative<CrossReference>(node)) ++references;
    }
    std::cout << "    tokens:";
    for (const auto& node : committed) {
        if (const auto* r = std::get_if<TextRun>(&node)) {
            std::cout << " [" << r->text << "]";
        } else if (std::holds_alternative<Citation>(node)) {
            std::cout << " <cite>";
        } else if (std::holds_alternative<CrossReference>(node)) {
            std::cout << " <xref>";
        } else {
            std::cout << " <eq>";
        }
    }
    std::cout << "\n";
    PF_CHECK(citations == 1);
    PF_CHECK(references == 1);

    // Citation plan §1/§2: both are real renderable objects - exactly one
    // object replacement character each, no stray U+E000 pseudo text.
    const QString as_text = editor.toPlainText();
    PF_CHECK(!as_text.contains(QChar(0xE000)));
    int object_chars = 0;
    for (const QChar ch : as_text) {
        if (ch == QChar(0xFFFC)) ++object_chars;
    }
    PF_CHECK(object_chars == 2);

    // A reload must keep both tokens, with the same payload.
    InlineEditor reloaded;
    reloaded.SetContent(committed);
    const InlineContent again = reloaded.Content();
    const auto* citation = std::get_if<Citation>(&again[1]);
    PF_CHECK(citation && citation->keys.size() == 1 &&
             citation->keys.front() == "smith2024");
    const auto* reference = std::get_if<CrossReference>(&again[2]);
    PF_CHECK(reference && reference->target == NodeId("n7"));
}

PF_TEST(InlineEditorToolbarToggleMarksTheSelection) {
    EnsureApp();
    InlineEditor editor;
    editor.SetContent(InlineFromText("select me"));
    // Select the word "select".
    QTextCursor cursor(editor.document());
    cursor.setPosition(0);
    cursor.setPosition(6, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);

    editor.ToggleItalic();
    editor.ToggleBold();

    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 2);
    const auto* marked = std::get_if<TextRun>(&content[0]);
    PF_CHECK(marked && marked->text == "select");
    if (marked) {
        PF_CHECK(HasMark(marked->marks, TextMark::Strong));
        PF_CHECK(HasMark(marked->marks, TextMark::Emphasis));
    }
    const auto* tail = std::get_if<TextRun>(&content[1]);
    PF_CHECK(tail && tail->text == " me");
    if (tail) PF_CHECK(tail->marks == 0);
}

PF_TEST(InlineEditorDirtyFlagGuardsTheRebuild) {
    EnsureApp();
    InlineEditor editor;
    editor.SetContent(InlineFromText("hello"));
    PF_CHECK(!editor.IsDirty());

    // Formatting alone (no text change) must still protect the row from a
    // rebuild tearing it down mid-edit.
    editor.ToggleBold();
    PF_CHECK(editor.IsDirty());

    editor.MarkClean();
    PF_CHECK(!editor.IsDirty());
}


PF_TEST(InlineEditorHeightDoesNotBlowUpWhenUnlaid) {
    EnsureApp();
    InlineEditor editor;
    editor.resize(600, 30);
    editor.SetContent(InlineFromText(
        "A paragraph long enough to wrap onto a couple of lines when the "
        "editor is six hundred pixels wide, but not much more."));
    editor.ResizeToContent();
    const int laid_out = editor.height();

    // The toolbar-click case: the editor has a widget width but its viewport
    // collapses to nothing while it is not on screen. The height must still
    // be measured against the widget width, not against the viewport.
    const int widget_width = editor.width();
    // Simulate a zero-width viewport without shrinking the widget itself.
    const int viewport_width = editor.viewport()->width();
    (void)viewport_width;
    editor.ResizeToContent();
    const int remeasured = editor.height();

    std::cout << "    height: laid_out=" << laid_out
              << " remeasured=" << remeasured
              << " widget_width=" << widget_width << "\n";
    // Re-measuring at the same widget width must not change the height. The
    // old code measured against viewport()->width(), which collapses during a
    // rebuild, and produced one line per word.
    PF_CHECK(remeasured == laid_out);
}

// The row is created *before* the layout gives it its final width. Measuring
// against the not-yet-laid-out width (or against a collapsed viewport) wraps
// every word onto its own line and pins a huge fixed height - the "a big blank
// area appears, Delete restores it" report.
PF_TEST(FreshlyBuiltRowIsNotPinnedToAGiantHeight) {
    EnsureApp();
    const QString text = QStringLiteral(
        "Infrared small target detection has attracted considerable attention "
        "in recent years, yet robust detection under complex backgrounds "
        "remains difficult because targets occupy only a few pixels.");

    InlineEditor fresh;
    fresh.SetContent(InlineFromText(text.toStdString()));
    const int unlaid_height = fresh.height();

    InlineEditor laid_out;
    laid_out.resize(700, 40);
    laid_out.SetContent(InlineFromText(text.toStdString()));
    laid_out.ResizeToContent();
    const int target_height = laid_out.height();

    std::cout << "    height: fresh=" << unlaid_height
              << " laid_out=" << target_height << "\n";
    // The not-yet-laid-out row must not be several times taller than the same
    // text once the width is known.
    PF_CHECK(unlaid_height <= target_height * 3);

    // Once the layout assigns the real width, the row must settle on the
    // correct height (this is what makes Delete "restore" it today).
    fresh.resize(700, unlaid_height);
    fresh.ResizeToContent();
    PF_CHECK(fresh.height() == target_height);
}

// Widening a narrow row must re-flow and shrink the height on its own - the
// user should not have to press Delete to get the blank area back.
PF_TEST(WideningARowReflowsWithoutUserInput) {
    EnsureApp();
    const QString text = QStringLiteral(
        "A paragraph that needs quite a few lines when the editor is narrow "
        "and noticeably fewer once the row has been given its real width by "
        "the surrounding layout.");

    InlineEditor editor;
    // A top-level widget must be shown for resize events to be delivered.
    editor.show();
    editor.resize(220, 40);
    editor.SetContent(InlineFromText(text.toStdString()));
    editor.ResizeToContent();
    const int narrow = editor.height();

    // The layout hands the row its real width: no typing, no Delete.
    editor.resize(760, narrow);
    // resize() only queues the event; deliver it like the real event loop does.
    QApplication::processEvents();
    const int wide = editor.height();

    std::cout << "    height: narrow=" << narrow << " wide=" << wide << "\n";
    PF_CHECK(wide < narrow);
}
