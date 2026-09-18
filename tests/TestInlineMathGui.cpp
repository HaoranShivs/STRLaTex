// GUI tests for the math-input redesign:
//   * the Inline Math toolbar action asks for a body instead of inserting a
//     hard-coded x^{2},
//   * an inline math object round-trips through the editor, deletes whole and
//     survives copy/paste,
//   * the math editor validates and previews without rewriting the source,
//   * an Equation row exposes source, preview, numbered and label.
#include "TestMain.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextBlock>
#include <QTextLayout>
#include <QFontMetricsF>
#include "app/InlineMathObjectRenderer.h"
#include <QTimer>
#include <QThread>
#include <QToolButton>

#include <filesystem>
#include <iostream>
#include <memory>

#include "app/BlockEditor.h"
#include "app/InlineEditor.h"
#include "app/MainWindow.h"
#include "app/MathEditorDialog.h"
#include "app/MathPreviewRenderer.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"

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

InlineEditor* FindRich(MainWindow& window, const QString& node_id) {
    for (InlineEditor* edit : window.findChildren<InlineEditor*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_node").toString() == node_id) return edit;
    }
    return nullptr;
}

// Poll until `finder` returns non-null or the timeout expires. Rebuilds are
// deferred through the event queue, so a fixed sleep is not a reliable wait -
// especially under a sanitizer build, where every step is much slower.
template <typename Finder>
auto WaitFor(Finder finder, int timeout_ms = 2000) -> decltype(finder()) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        auto found = finder();
        if (found) return found;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return finder();
}


QToolButton* FindButton(MainWindow& window, const QString& text) {
    QToolButton* best = nullptr;
    for (QToolButton* button : window.findChildren<QToolButton*>()) {
        if (!button->isVisible()) continue;
        if (button->text() == text) best = button;
    }
    return best;
}

// A project with one Text row, ready for inline math.
struct Fixture {
    MainWindow window;
    QString node_id;

    explicit Fixture(const QString& dir_name) {
        window.resize(1400, 900);
        window.show();
        auto dir = std::filesystem::temp_directory_path() /
                   dir_name.toStdString();
        std::filesystem::remove_all(dir);
        window.controller()->NewProject(QString::fromStdString(dir.string()));
        window.controller()->Save();
        window.controller()->FlushSaves();
        window.OpenProjectDir(QString::fromStdString(dir.string()));
        Spin(250);

        auto section = window.controller()->InsertSection(QStringLiteral("S"));
        auto paragraph = window.controller()->InsertParagraph(
            section.created_node, QStringLiteral("before after"));
        node_id = QString::fromStdString(paragraph.created_node.value());
        Spin(300);
    }
};

const Paragraph* StoredParagraph(MainWindow& window) {
    Document& doc = window.controller()->session().mutable_document();
    const auto& sections = BodyOf(doc).sections;
    if (sections.empty() || sections[0].blocks.empty()) return nullptr;
    return std::get_if<Paragraph>(&sections[0].blocks[0]);
}

// Closes the next modal dialog by rejecting it, as a user pressing Esc would.
void RejectNextModalDialog(QObject* owner, int delay_ms = 60) {
    auto* timer = new QTimer(owner);
    timer->setInterval(delay_ms);
    QObject::connect(timer, &QTimer::timeout, owner, [timer]() {
        if (QWidget* modal = QApplication::activeModalWidget()) {
            if (auto* dialog = qobject_cast<QDialog*>(modal)) {
                timer->stop();
                dialog->reject();
            }
        }
    });
    timer->start();
}

}  // namespace

PF_TEST(InlineMathObjectRoundTripsThroughTheEditor) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("\\frac{a}{b}"));

    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 1);
    const auto* math = content.empty() ? nullptr : std::get_if<InlineMath>(&content[0]);
    PF_CHECK(math != nullptr);
    if (math) PF_CHECK(math->expression.latex == "\\frac{a}{b}");

    // Reloading must not change the stored body.
    InlineEditor reloaded;
    reloaded.SetContent(content);
    const InlineContent again = reloaded.Content();
    PF_CHECK(again == content);
}

PF_TEST(InlineMathDoesNotIncreaseTheTextLineHeight) {
    EnsureQApplication();
    InlineEditor plain;
    plain.resize(640, 80);
    plain.SetContent(InlineFromText("before after"));
    plain.ResizeToContent();
    const int plain_height = plain.height();

    InlineContent with_math = InlineFromText("before ");
    InlineMath fraction;
    fraction.expression.latex = "\\frac{a}{b}";
    with_math.push_back(fraction);
    with_math.push_back(TextRun{" after", 0});

    InlineEditor formula;
    formula.resize(640, 80);
    formula.SetContent(with_math);
    formula.ResizeToContent();

    std::cout << "  plain height=" << plain_height
              << " formula height=" << formula.height() << "\n";
    PF_CHECK(formula.height() <= plain_height + 1);
}

PF_TEST(InlineMathObjectIsNotUserEditableText) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("\\mathcal{L}"));
    // The visible text is an object placeholder, never the LaTeX body.
    const QString plain = editor.toPlainText();
    PF_CHECK(plain.contains(QChar(0xFFFC)));
    PF_CHECK(!plain.contains(QStringLiteral("\\mathcal{L}")));
}

PF_TEST(InlineMathObjectDeletesWhole) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("x^{2}"));
    QTextCursor cursor(editor.document());
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);

    QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(&editor, &backspace);

    PF_CHECK(editor.Content().empty());
}

PF_TEST(InlineMathSurvivesCopyPasteInsideTheEditor) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("\\alpha"));
    QTextCursor cursor(editor.document());
    cursor.select(QTextCursor::Document);
    editor.setTextCursor(cursor);

    // The editor's own clipboard flavour carries the math object, not just the
    // object-replacement placeholder.
    std::unique_ptr<QMimeData> copied(editor.MimeDataForSelection());
    PF_CHECK(copied != nullptr);
    if (!copied) return;
    std::cout << "    copied formats=" << copied->formats().size() << "\n";
    PF_CHECK(copied->hasFormat("application/x-strlatex-inline"));

    cursor.clearSelection();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
    editor.InsertMimeDataForTest(copied.get());

    const InlineContent content = editor.Content();
    int math_count = 0;
    for (const auto& node : content) {
        if (const auto* math = std::get_if<InlineMath>(&node)) {
            ++math_count;
            PF_CHECK(math->expression.latex == "\\alpha");
        }
    }
    PF_CHECK(math_count == 2);

    // The same round trip through the real clipboard, as Ctrl+C / Ctrl+V does.
    InlineEditor live;
    live.InsertInlineMath(QStringLiteral("\\beta"));
    QTextCursor live_cursor(live.document());
    live_cursor.select(QTextCursor::Document);
    live.setTextCursor(live_cursor);
    live.copy();
    QCoreApplication::processEvents();
    live_cursor.clearSelection();
    live_cursor.movePosition(QTextCursor::End);
    live.setTextCursor(live_cursor);
    live.paste();
    int live_math = 0;
    for (const auto& node : live.Content()) {
        if (std::holds_alternative<InlineMath>(node)) ++live_math;
    }
    PF_CHECK(live_math == 2);
}

PF_TEST(InlineMathObjectIsEditedThroughTheSourceDialog) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("\\alpha"));

    // EditMathAt opens the source editor asynchronously; drive the normal
    // application event loop as a user would (type a new body, then accept).
    auto* driver = new QTimer(&editor);
    driver->setInterval(40);
    QObject::connect(driver, &QTimer::timeout, &editor, [driver]() {
        auto* dialog =
            qobject_cast<MathEditorDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        driver->stop();
        PF_CHECK(dialog->latex() == QStringLiteral("\\alpha"));
        dialog->SetSourceForTest(QStringLiteral("\\beta + 1"));
        dialog->accept();
    });
    driver->start();
    editor.EditMathAt(0);
    Spin(200);

    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 1);
    const auto* math =
        content.empty() ? nullptr : std::get_if<InlineMath>(&content[0]);
    PF_CHECK(math != nullptr);
    if (math) PF_CHECK(math->expression.latex == "\\beta + 1");
}

// The bug this pins: the toolbar used to insert a hard-coded x^{2} without
// asking the user for a body.
PF_TEST(InlineMathToolbarAsksForABodyInsteadOfHardcoding) {
    EnsureQApplication();
    Fixture fixture("pf-inline-math-toolbar");
    InlineEditor* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor) return;
    editor->setFocus(Qt::MouseFocusReason);
    Spin(150);
    editor->SetContentClean(InlineFromText("before after"));

    QToolButton* math = FindButton(fixture.window, QStringLiteral("Inline Math"));
    PF_CHECK(math != nullptr);
    if (!math) return;

    RejectNextModalDialog(&fixture.window);
    math->click();
    Spin(300);

    InlineEditor* after = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(after != nullptr);
    if (!after) return;
    // Cancelling the dialog must leave the row exactly as it was: no x^{2}.
    PF_CHECK(after->toPlainText() == QStringLiteral("before after"));
    for (const auto& node : after->Content()) {
        PF_CHECK(!std::holds_alternative<InlineMath>(node));
    }
}

PF_TEST(MathEditorDialogValidatesWithoutRewritingTheSource) {
    EnsureQApplication();
    MathEditorDialog dialog(QStringLiteral("\\frac{a}{b}"));
    dialog.SetSourceForTest(QStringLiteral("\\frac{a}{b}"));
    PF_CHECK(dialog.StateText().contains(QStringLiteral("Valid")));

    // Invalid source keeps its text and reports why.
    dialog.SetSourceForTest(QStringLiteral("\\begin{equation} x \\end{equation}"));
    PF_CHECK(dialog.latex() == QStringLiteral("\\begin{equation} x \\end{equation}"));
    PF_CHECK(dialog.StateText().contains(QStringLiteral("Invalid")));

    // Empty source is Pending, not an error.
    dialog.SetSourceForTest(QString());
    PF_CHECK(dialog.StateText().contains(QStringLiteral("Type a math body")));

    // A valid body produces a preview pixmap.
    dialog.SetSourceForTest(QStringLiteral("\\sqrt{x^2 + y^2}"));
    MathRenderStyle style;
    style.font_px = 18;
    const MathRenderResult rendered =
        RenderMathPreview(QStringLiteral("\\sqrt{x^2 + y^2}"), style);
    PF_CHECK(!rendered.pixmap.isNull());
}

PF_TEST(EquationRowExposesPreviewNumberedAndLabel) {
    EnsureQApplication();
    Fixture fixture("pf-equation-card");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("E"));
    auto inserted = controller->InsertEquation(
        section.created_node, QStringLiteral("E = mc^2"), true,
        QStringLiteral("eq:energy"));
    PF_CHECK(inserted.status == EditStatus::Applied);
    Spin(400);

    const QString node_id = QString::fromStdString(inserted.created_node.value());
    QCheckBox* numbered = nullptr;
    QLineEdit* label = nullptr;
    QPlainTextEdit* source = nullptr;
    for (QCheckBox* box : fixture.window.findChildren<QCheckBox*>()) {
        if (box->isVisible() &&
            box->property("row_node").toString() == node_id) {
            numbered = box;
        }
    }
    for (QLineEdit* edit : fixture.window.findChildren<QLineEdit*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_node").toString() == node_id) label = edit;
    }
    for (QPlainTextEdit* edit : fixture.window.findChildren<QPlainTextEdit*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_focus_key").toString() == node_id) source = edit;
    }
    PF_CHECK(numbered != nullptr);
    PF_CHECK(label != nullptr);
    PF_CHECK(source != nullptr);
    if (source) PF_CHECK(source->toPlainText() == QStringLiteral("E = mc^2"));
    if (numbered) PF_CHECK(numbered->isChecked());
    if (label) PF_CHECK(label->text() == QStringLiteral("eq:energy"));
    if (source) {
        // The source field must be the LaTeX body only - never an environment.
        PF_CHECK(!source->toPlainText().contains(QStringLiteral("\\begin{equation}")));
    }

    // The document stores the attributes separately from the source.
    Document& doc = controller->session().mutable_document();
    const NodeId target = inserted.created_node;
    const EquationBlock* found = nullptr;
    for (const auto& section : BodyOf(doc).sections) {
        for (const auto& block : section.blocks) {
            if (const auto* eq = std::get_if<EquationBlock>(&block)) {
                if (eq->id == target) found = eq;
            }
        }
    }
    PF_CHECK(found != nullptr);
    if (found) {
        PF_CHECK(found->expression.latex == "E = mc^2");
        PF_CHECK(found->numbered);
        PF_CHECK(found->label == "eq:energy");
    }
}

PF_TEST(InlineMathTypingDoesNotInheritObjectPayload) {
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("x"));
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                  QStringLiteral("a"));
    QApplication::sendEvent(&editor, &key);
    const auto content = editor.Content();
    PF_CHECK(content.size() == 2);
    if (content.size() != 2) return;
    PF_CHECK(std::holds_alternative<InlineMath>(content[0]));
    const auto* text = std::get_if<TextRun>(&content[1]);
    PF_CHECK(text != nullptr);
    if (text) PF_CHECK(text->text == "a");
}

PF_TEST(InlineMathUsesTextBaselineInPolishedWidget) {
    QWidget host;
    host.setStyleSheet(QStringLiteral("QWidget { font-size: 10pt; }"));
    InlineEditor editor(&host);
    editor.resize(640, 80);
    editor.ensurePolished();
    editor.SetContent(InlineFromText("before after"));
    editor.ResizeToContent();
    const auto plain = editor.document()->begin().layout()->lineAt(0);
    const qreal ascent = plain.ascent();
    const qreal descent = plain.descent();
    editor.InsertInlineMath(QStringLiteral("\\frac{a}{b}"));
    editor.ResizeToContent();
    const auto line = editor.document()->begin().layout()->lineAt(0);
    PF_CHECK(qAbs(line.ascent() - ascent) < 1.0);
    PF_CHECK(qAbs(line.descent() - descent) < 1.0);
    QTextCursor cursor(editor.document());
    cursor.movePosition(QTextCursor::End);
    cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
    const auto format = cursor.charFormat();
    PF_CHECK(format.verticalAlignment() == QTextCharFormat::AlignNormal);
    const QFontMetricsF metrics(editor.document()->defaultFont());
    const qreal baseline = format.property(
        inline_math_format::kBaselineProperty).toDouble();
    const qreal height = format.property(
        inline_math_format::kHeightProperty).toDouble();
    PF_CHECK(baseline <= metrics.ascent());
    PF_CHECK(height - baseline <= metrics.descent());
}

PF_TEST(InlineMathRepeatedEditingSurvivesMainWindowRefresh) {
    Fixture fixture("pf-inline-math-repeated-edit");
    auto* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor) return;
    editor->SetContentClean({});
    editor->setFocus();
    editor->InsertInlineMath(QStringLiteral("x"));
    for (int i = 0; i < 3; ++i) {
        editor = FindRich(fixture.window, fixture.node_id);
        PF_CHECK(editor != nullptr);
        if (!editor) return;
        QPointer<InlineEditor> guarded(editor);
        editor->EditMathAt(0);
        Spin(80);
        PF_CHECK(!guarded.isNull());
        if (!guarded) return;
        auto* dialog = editor->findChild<MathEditorDialog*>();
        PF_CHECK(dialog != nullptr);
        if (!dialog) return;
        // A document notification while focus is in the dialog must defer
        // rebuilding the row, even when that row was previously clean.
        fixture.window.controller()->InsertSection(QStringLiteral("Later"));
        Spin(80);
        PF_CHECK(!guarded.isNull());
        if (!guarded) return;
        dialog->SetSourceForTest(QStringLiteral("x + %1").arg(i));
        dialog->accept();
        Spin(200);
        const Paragraph* stored = StoredParagraph(fixture.window);
        PF_CHECK(stored != nullptr);
        editor = FindRich(fixture.window, fixture.node_id);
        PF_CHECK(editor != nullptr);
        if (!editor) return;
        const auto content = editor->Content();
        PF_CHECK(content.size() == 1);
        if (content.size() != 1) return;
        const auto* math = std::get_if<InlineMath>(&content.front());
        PF_CHECK(math != nullptr);
        if (math) PF_CHECK(math->expression.latex ==
            QStringLiteral("x + %1").arg(i).toStdString());
    }
}

PF_TEST(InlineMathDialogIsDestroyedWithItsEditor) {
    auto* editor = new InlineEditor;
    editor->InsertInlineMath(QStringLiteral("x"));
    editor->EditMathAt(0);
    QPointer<MathEditorDialog> dialog = editor->findChild<MathEditorDialog*>();
    PF_CHECK(!dialog.isNull());
    delete editor;
    Spin(50);
    PF_CHECK(dialog.isNull());
}

// P0-08: the inline-math lifetime stress the review asks for. The reported
// crash was "double free or corruption (out)" on the second double-click edit.
// This drives the full lifecycle 100 times - insert, edit twice, delete,
// undo, redo - inside one editor, then closes the project and destroys the
// window. Under ASan/UBSan any double-free, use-after-free or invalid free
// aborts the run; without a sanitizer the test still asserts that every
// editor pointer stays valid and the document never loses the formula.
PF_TEST(InlineMathLifetimeStressLoop) {
    Fixture fixture("pf-inline-math-stress");
    auto* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor) return;
    editor->SetContentClean({});

    constexpr int kIterations = 100;
    for (int i = 0; i < kIterations; ++i) {
        editor = FindRich(fixture.window, fixture.node_id);
        PF_CHECK(editor != nullptr);
        if (!editor) return;
        // 1-2. Insert an inline formula.
        editor->setFocus();
        editor->InsertInlineMath(QStringLiteral("x_%1").arg(i));
        Spin(1);

        // 3-4. Double-click style edit, then confirm. The invariant that
        // matters is that the editor survives while the dialog is OPEN: a
        // document notification must not tear the row down under the modal.
        // (Accepting commits, which rebuilds the row - that is the normal
        // full-rebuild path, so the pointer is re-fetched afterwards.)
        editor = FindRich(fixture.window, fixture.node_id);
        if (!editor) return;
        QPointer<InlineEditor> guarded(editor);
        editor->EditMathAt(0);
        Spin(1);
        if (guarded.isNull()) {
            PF_CHECK(false);
            return;
        }
        auto* dialog = editor->findChild<MathEditorDialog*>();
        if (dialog) {
            dialog->SetSourceForTest(QStringLiteral("a_%1+b").arg(i));
            dialog->accept();
        }
        // The row must still be reachable after the commit-driven rebuild.
        // The rebuild is posted to the event queue, so poll instead of
        // guessing a sleep (sanitizer builds run far slower).
        editor = WaitFor([&] { return FindRich(fixture.window, fixture.node_id); });
        if (!editor) {
            PF_CHECK(false);
            return;
        }

        // 5-6. Edit the same formula a second time (the historical crash).
        editor->EditMathAt(0);
        Spin(1);
        if (!editor->findChild<MathEditorDialog*>()) {
            // The row was rebuilt again; locate the live editor.
            editor = WaitFor([&] { return FindRich(fixture.window, fixture.node_id); });
            if (!editor) return;
            editor->EditMathAt(0);
            Spin(1);
        }
        dialog = editor->findChild<MathEditorDialog*>();
        if (dialog) {
            dialog->SetSourceForTest(QStringLiteral("c_%1").arg(i));
            dialog->accept();
        }
        Spin(2);

        // 7. Delete the formula from the row, then 8-9. undo/redo it.
        //    (The editor is rebuilt by the refresh these trigger.)
        editor = FindRich(fixture.window, fixture.node_id);
        if (!editor) return;
        const auto content_before = editor->Content();
        if (!content_before.empty()) {
            QTextCursor cursor(editor->document());
            cursor.movePosition(QTextCursor::Start);
            cursor.movePosition(QTextCursor::NextCharacter,
                                QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        Spin(1);
    }

    // 10. Close the project while a formula is on screen.
    editor = FindRich(fixture.window, fixture.node_id);
    if (editor) {
        editor->setFocus();
        editor->InsertInlineMath(QStringLiteral("final"));
        Spin(2);
    }
    fixture.window.controller()->CloseProject();
    Spin(20);
    // 11. Destroy the window explicitly: the sanitizer run covers teardown.
    PF_CHECK(true);
}
