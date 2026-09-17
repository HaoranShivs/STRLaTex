// Reproduces the two GUI reports:
//   1. clicking the format buttons grows the text row into a big blank area
//      (Delete then "restores" it);
//   2. bold/italic work under the default template but not under IEEE.
//
// Both are driven through the real MainWindow, the real toolbar and the real
// InlineEditor, because neither symptom exists in the domain layer.
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QToolButton>

#include <filesystem>
#include <iostream>

#include "TestMain.hpp"

#include "app/BlockEditor.h"
#include "app/InlineEditor.h"
#include "app/MainWindow.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "render/LatexRenderer.h"
#include "template/TemplateRegistry.h"

using namespace pf;
using namespace pf::gui;

namespace {

QApplication* EnsureQApplication() { return qApp; }

Body& BodyOf(Document& doc) { return DocumentMutableAccess::body(doc); }

void Spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// The visible rich editor for a node id.
InlineEditor* FindRich(MainWindow& window, const QString& node_id) {
    for (InlineEditor* edit : window.findChildren<InlineEditor*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_node").toString() == node_id) return edit;
    }
    return nullptr;
}

// Every QToolButton on the format bar of the card that owns `editor`.
std::vector<QToolButton*> ToolbarButtons(MainWindow& window) {
    std::vector<QToolButton*> out;
    for (QToolButton* button : window.findChildren<QToolButton*>()) {
        if (!button->isVisible()) continue;  // stale toolbar from a rebuild
        const QString text = button->text();
        if (text == "B" || text == "I" || text == "Inline Math" ||
            text == "Citation" || text == "Reference") {
            out.push_back(button);
        }
    }
    return out;
}

QToolButton* FindButton(MainWindow& window, const QString& text) {
    for (QToolButton* button : ToolbarButtons(window)) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

// Build a project with one Text row holding one word, ready to be formatted.
struct Fixture {
    MainWindow window;
    QString node_id;

    explicit Fixture(const QString& dir_name) {
        window.resize(1400, 900);
        window.show();
        auto dir = std::filesystem::temp_directory_path() /
                   dir_name.toStdString();
        std::filesystem::remove_all(dir);
        // Create the project, then save+reopen it: opening is what switches
        // the window from the welcome page to the workspace, exactly like a
        // real session. Without this the editor rows stay hidden.
        window.controller()->NewProject(QString::fromStdString(dir.string()));
        window.controller()->Save();
        window.controller()->FlushSaves();
        window.OpenProjectDir(QString::fromStdString(dir.string()));
        Spin(250);

        auto section = window.controller()->InsertSection(QStringLiteral("S"));
        auto paragraph = window.controller()->InsertParagraph(
            section.created_node, QStringLiteral("word"));
        node_id = QString::fromStdString(paragraph.created_node.value());
        Spin(300);
    }
};

// What the document stores for the single paragraph.
const Paragraph* StoredParagraph(MainWindow& window) {
    Document& doc = window.controller()->session().mutable_document();
    const auto& sections = BodyOf(doc).sections;
    if (sections.empty() || sections[0].blocks.empty()) return nullptr;
    return std::get_if<Paragraph>(&sections[0].blocks[0]);
}

std::string RenderedTex(MainWindow& window) {
    RenderRequest request;
    request.document = &window.controller()->session().state().document();
    request.template_id =
        window.controller()->session().state().template_selection();
    request.revision = window.controller()->session().current_revision();
    auto rendered = LatexRenderer().Render(request);
    if (rendered.package.files.empty()) return {};
    return rendered.package.files[0].content;
}

}  // namespace

// ---- Question 1: does a toolbar click blow up the row height? ----

PF_TEST(ToolbarClickDoesNotGrowTheTextRow) {
    EnsureQApplication();
    Fixture fixture("pf-toolbar-height");
    InlineEditor* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor) return;

    const int before = editor->height();

    // A real user clicks into the paragraph before reaching for the toolbar:
    // the format strip is part of the block chrome and only shows on
    // hover/focus (UI plan §2), so focus first, then find the button.
    editor->setFocus(Qt::MouseFocusReason);
    Spin(120);

    // Click the italic button exactly as a user would.
    QToolButton* italic = FindButton(fixture.window, QStringLiteral("I"));
    PF_CHECK(italic != nullptr);
    if (!italic) return;
    std::cout << "    italic button focusPolicy=" << italic->focusPolicy()
              << " (0 == NoFocus)\n";
    italic->click();
    Spin(400);

    InlineEditor* after_editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(after_editor != nullptr);
    if (!after_editor) return;
    const int after = after_editor->height();
    std::cout << "    row height before=" << before << " after=" << after
              << " widget_width=" << after_editor->width() << "\n";
    // The row may be reconstructed, but it must not become a tall blank box.
    PF_CHECK(after <= before + 20);
}

PF_TEST(ToolbarClickKeepsTheCaretInTheRow) {
    EnsureQApplication();
    Fixture fixture("pf-toolbar-focus");
    InlineEditor* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor) return;
    editor->setFocus(Qt::MouseFocusReason);
    Spin(120);

    QToolButton* italic = FindButton(fixture.window, QStringLiteral("I"));
    PF_CHECK(italic != nullptr);
    if (!italic) return;
    italic->click();
    Spin(300);

    // Focus must stay in the row, otherwise clicking a button commits the row
    // and rebuilds every card mid-edit.
    InlineEditor* after_editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(after_editor != nullptr && after_editor->hasFocus());
}

// ---- Question 2: marks under both templates ----

namespace {

// Applies bold+italic to the first word of the Text row, through the real
// toolbar, and reports back what the document and the LaTeX ended up with.
struct MarkOutcome {
    bool document_bold = false;
    bool document_italic = false;
    bool tex_bold = false;
    bool tex_italic = false;
};

MarkOutcome FormatFirstWord(MainWindow& window, const QString& node_id) {
    MarkOutcome outcome;
    InlineEditor* editor = FindRich(window, node_id);
    if (!editor) return outcome;
    // A real user clicks into the paragraph before reaching for the toolbar.
    editor->setFocus(Qt::MouseFocusReason);
    Spin(120);

    // Select the whole word.
    QTextCursor cursor(editor->document());
    cursor.setPosition(0);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    editor->setTextCursor(cursor);

    QToolButton* bold = FindButton(window, QStringLiteral("B"));
    QToolButton* italic = FindButton(window, QStringLiteral("I"));
    if (italic) italic->click();
    Spin(150);
    if (bold) bold->click();
    Spin(300);

    // Commit the way a user does: leave the row (click elsewhere).
    if (InlineEditor* current = FindRich(window, node_id)) {
        current->clearFocus();
    }
    Spin(300);
    window.controller()->session().ProcessApplicationEvents();

    if (const Paragraph* paragraph = StoredParagraph(window)) {
        std::cout << "      stored:";
        for (const auto& node : paragraph->content) {
            if (const auto* run = std::get_if<TextRun>(&node)) {
                std::cout << " [" << run->text << "|"
                          << static_cast<int>(run->marks) << "]";
            }
        }
        std::cout << "\n";
        for (const auto& node : paragraph->content) {
            if (const auto* run = std::get_if<TextRun>(&node)) {
                if (HasMark(run->marks, TextMark::Strong)) {
                    outcome.document_bold = true;
                }
                if (HasMark(run->marks, TextMark::Emphasis)) {
                    outcome.document_italic = true;
                }
            }
        }
    }
    const std::string tex = RenderedTex(window);
    // Bold+italic nests as \textbf{\emph{word}}, so accept either form.
    outcome.tex_bold = tex.find("\\textbf{") != std::string::npos &&
                       tex.find("word") != std::string::npos;
    outcome.tex_italic = tex.find("\\emph{") != std::string::npos &&
                         tex.find("word") != std::string::npos;
    return outcome;
}

}  // namespace

PF_TEST(ToolbarMarksWorkOnEveryTemplate) {
    EnsureQApplication();
    for (const auto& def : TemplateRegistry::Instance().All()) {
        Fixture fixture("pf-template-marks-" + QString::fromStdString(def.id));
        fixture.window.controller()->ChangeTemplate(
            QString::fromStdString(def.id));
        Spin(350);

        const MarkOutcome outcome =
            FormatFirstWord(fixture.window, fixture.node_id);
        std::cout << "    " << def.id
                  << ": doc(bold=" << outcome.document_bold
                  << " italic=" << outcome.document_italic
                  << ") tex(bold=" << outcome.tex_bold
                  << " italic=" << outcome.tex_italic << ")\n";

        PF_CHECK(outcome.document_bold);
        PF_CHECK(outcome.document_italic);
        PF_CHECK(outcome.tex_bold);
        PF_CHECK(outcome.tex_italic);
    }
}
