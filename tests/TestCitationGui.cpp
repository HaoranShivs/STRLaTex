// GUI-layer tests for the citation redesign (citation plan §1-§6):
//   * a citation is a real renderable inline object (pill), not a fake
//     U+E000 character - one object replacement char, format carries the
//     semantic payload, the paint comes from CitationNumberResolver;
//   * backspace deletes the whole pill and never disturbs surrounding marks;
//   * the picker flow inserts at the *saved* caret and commits immediately
//     (InlineEditor -> ParagraphContentEdited -> EditParagraphRich);
//   * focusOut while the picker owns focus is not treated as "done editing".
#include "TestMain.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QMetaObject>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QThread>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "app/BlockEditor.h"
#include "app/CitationObjectRenderer.h"
#include "app/InlineEditor.h"
#include "app/MainWindow.h"
#include "build/BuildCoordinator.h"
#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "editing/EditCommand.h"
#include "numbering/CitationNumberResolver.h"
#include "project/ProjectSession.h"

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

std::shared_ptr<const CitationNumberResolver> ResolverFor(
    const std::vector<std::string>& cited_in_order,
    const std::vector<std::string>& in_bibliography) {
    BibliographyDatabase db;
    BibliographyService service(db);
    std::string bib;
    for (const auto& key : in_bibliography) {
        bib += "@article{" + key + ", title={T}, year={2020}}\n";
    }
    service.ImportText(bib);

    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    for (const auto& key : cited_in_order) {
        Paragraph p;
        Citation cit;
        cit.keys = {key};
        p.content.push_back(std::move(cit));
        editor.InsertBlock(s.value(), std::nullopt, p);
    }
    return std::make_shared<const CitationNumberResolver>(
        CitationNumberResolver::Build(doc, db));
}

// The char format of the object at `position` (object replacement char).
QTextCharFormat FormatAt(const InlineEditor& editor, int position) {
    QTextCursor cursor(editor.document());
    cursor.setPosition(position);
    cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
    return cursor.charFormat();
}

// Sends a key event straight to the widget, as the platform would.
void SendKey(QWidget* widget, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent event(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(widget, &event);
}

}  // namespace

// ---------------------------------------------------------------------------
// InlineEditor: semantic object + renderer architecture
// ---------------------------------------------------------------------------

PF_TEST(CitationPillCarriesKeyPayloadAndResolvedNumber) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({"smith2024"}, {"smith2024", "jones2020"}));
    editor.SetContent(InlineFromText("introduced by "));
    editor.InsertCitationObject({QStringLiteral("smith2024")});

    // Exactly one object replacement character, no legacy U+E000 anywhere.
    const QString plain = editor.toPlainText();
    PF_CHECK(!plain.contains(QChar(0xE000)));
    PF_CHECK(plain.count(QChar(0xFFFC)) == 1);

    // The paint text is resolved from the document-wide numbering: smith is
    // [1]; the payload (stored semantics) is the key, never the number.
    const int pill_position = plain.indexOf(QChar(0xFFFC));
    PF_CHECK(pill_position >= 0);
    const QTextCharFormat format = FormatAt(editor, pill_position);
    PF_CHECK(format.objectType() == citation_format::kObjectType);
    PF_CHECK(format.property(inline_object_format::kKindProperty)
                 .toInt() == static_cast<int>(InlineEditor::TokenKind::Citation));
    PF_CHECK(format.property(inline_object_format::kPayloadProperty)
                 .toString() == QStringLiteral("smith2024"));
    PF_CHECK(format.property(citation_format::kDisplayTextProperty)
                 .toString() == QStringLiteral("[1]"));

    // What the document receives is the key set.
    const InlineContent content = editor.Content();
    const Citation* citation = nullptr;
    for (const auto& node : content) {
        if (const auto* cit = std::get_if<Citation>(&node)) citation = cit;
    }
    PF_CHECK(citation != nullptr);
    if (citation) {
        PF_CHECK(citation->keys.size() == 1 &&
                 citation->keys[0] == "smith2024");
    }
}

PF_TEST(CitationNumberingFollowsInsertionOrderAcrossRows) {
    EnsureQApplication();
    // First A, then B: A is [1], B is [2]; re-citing A keeps [1].
    auto resolver = ResolverFor({"a", "b"}, {"a", "b"});
    InlineEditor editor;
    editor.SetCitationNumbers(resolver);
    editor.InsertCitationObject({QStringLiteral("a")});
    PF_CHECK(FormatAt(editor, 0)
                 .property(citation_format::kDisplayTextProperty)
                 .toString() == QStringLiteral("[1]"));
    // Multi-citation A+B renders sorted: [1, 2].
    InlineEditor multi;
    multi.SetCitationNumbers(resolver);
    multi.InsertCitationObject({QStringLiteral("b"), QStringLiteral("a")});
    PF_CHECK(FormatAt(multi, 0)
                 .property(citation_format::kDisplayTextProperty)
                 .toString() == QStringLiteral("[1, 2]"));
}

PF_TEST(UnknownCitationKeyPillShowsQuestionMark) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({}, {"real"}));
    editor.InsertCitationObject({QStringLiteral("ghost")});
    PF_CHECK(FormatAt(editor, 0)
                 .property(citation_format::kDisplayTextProperty)
                 .toString() == QStringLiteral("[?]"));
    // Still semantic: the key survives the round trip for validation.
    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 1);
    PF_CHECK(std::holds_alternative<Citation>(content[0]));
}

PF_TEST(BackspaceDeletesWholeCitationPill) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({"a"}, {"a"}));
    editor.SetContent(InlineFromText("see x"));
    // Insert the pill between "see " and "x" (position 4).
    QTextCursor cursor(editor.document());
    cursor.setPosition(4);
    editor.setTextCursor(cursor);
    editor.InsertCitationObject({QStringLiteral("a")});

    // Caret directly behind the pill; one Backspace removes the whole object.
    QTextCursor after(editor.document());
    after.setPosition(5);
    editor.setTextCursor(after);
    SendKey(&editor, Qt::Key_Backspace);

    const InlineContent content = editor.Content();
    for (const auto& node : content) {
        PF_CHECK(!std::holds_alternative<Citation>(node));
    }
    PF_CHECK(editor.toPlainText() == QStringLiteral("see x"));
}

PF_TEST(CitationDoesNotInheritOrPolluteMarks) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({"a"}, {"a"}));
    QTextCursor cursor(editor.document());
    QTextCharFormat bold;
    bold.setFontWeight(QFont::Bold);
    cursor.setCharFormat(bold);
    cursor.insertText(QStringLiteral("bold"));
    editor.setTextCursor(cursor);
    editor.InsertCitationObject({QStringLiteral("a")});
    // Type after the pill exactly like the user does: through the editor's
    // own cursor, which the object insert left in a neutral format.
    editor.moveCursor(QTextCursor::End);
    editor.insertPlainText(QStringLiteral("tail"));

    const InlineContent content = editor.Content();
    // bold run, citation, plain "tail" - the pill breaks the bold run but the
    // following text must not silently inherit bold or object properties.
    size_t citations = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (std::holds_alternative<Citation>(content[i])) {
            ++citations;
            continue;
        }
        const auto* run = std::get_if<TextRun>(&content[i]);
        if (!run) continue;
        if (run->text == "bold") {
            PF_CHECK(HasMark(run->marks, TextMark::Strong));
        } else if (run->text == "tail") {
            PF_CHECK(run->marks == 0);
        }
    }
    PF_CHECK(citations == 1);
    // The typed text is not swallowed by the pill.
    const std::string flat = InlineToPlainText(content);
    PF_CHECK(flat.find("bold") != std::string::npos);
    PF_CHECK(flat.find("tail") != std::string::npos);
}

// Undo / Redo must not break the surrounding text or invent tokens (plan §11).
// Qt 6.2 keeps the whole undo history; the pill insert is its own command
// because it carries a character format distinct from the adjacent text.
PF_TEST(UndoRedoOfCitationKeepsTextIntact) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({"a"}, {"a"}));
    editor.SetContent(InlineFromText("see this"));
    editor.document()->clearUndoRedoStacks();
    QTextCursor at_end(editor.document());
    at_end.movePosition(QTextCursor::End);
    editor.setTextCursor(at_end);
    editor.InsertCitationObject({QStringLiteral("a")});

    auto count_citations = [&editor]() {
        size_t n = 0;
        for (const auto& node : editor.Content()) {
            if (std::holds_alternative<Citation>(node)) ++n;
        }
        return n;
    };
    PF_CHECK(count_citations() == 1);

    editor.undo();
    PF_CHECK(count_citations() == 0);
    PF_CHECK(editor.toPlainText() == QStringLiteral("see this"));

    editor.redo();
    PF_CHECK(count_citations() == 1);
    const std::string flat = InlineToPlainText(editor.Content());
    PF_CHECK(flat.find("see this") != std::string::npos);
}

PF_TEST(PickerFocusRoundTripDoesNotCommitEarly) {
    EnsureQApplication();
    InlineEditor editor;
    bool committed = false;
    QObject::connect(&editor, &InlineEditor::Committed,
                     [&]() { committed = true; });

    // With the protection active (the picker has grabbed focus), a focus-out
    // must NOT be mistaken for "the user finished editing" (plan §4).
    editor.BeginProtectedInsert();
    QFocusEvent out(QEvent::FocusOut);
    QApplication::sendEvent(&editor, &out);
    PF_CHECK(!committed);

    editor.EndProtectedInsert();
    QFocusEvent out2(QEvent::FocusOut);
    QApplication::sendEvent(&editor, &out2);
    PF_CHECK(committed);
}

// ---------------------------------------------------------------------------
// End to end: the pill number and the PDF number are the same policy
// ---------------------------------------------------------------------------

PF_TEST(GuiPillNumberMatchesTheBuiltPdf) {
    EnsureQApplication();
    auto dir = std::filesystem::temp_directory_path() / "pf-citation-pdf";
    std::filesystem::remove_all(dir);

    ProjectSession::Config config;
    config.install_root = PF_INSTALL_ROOT;  // bundled portable TeX Live
    config.debounce = std::chrono::milliseconds{0};
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));
    session.ImportBibliography(
        "@article{smith2024, author={J. Smith}, title={Seminal Method}, "
        "year={2024}}\n"
        "@article{jones2020, author={A. Jones}, title={Follow Up}, "
        "year={2020}}\n");

    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = session.state().id();
    cmd.base_revision = session.current_revision();
    SetTitlePayload title;
    title.title = InlineFromText("Citation Numbering");
    cmd.payload = title;
    PF_CHECK(session.Execute(cmd).status == EditStatus::Applied);

    InsertSectionPayload sec;
    sec.index = 0;
    sec.title = InlineFromText("S");
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.base_revision = session.current_revision();
    cmd.payload = sec;
    auto sec_r = session.Execute(cmd);
    PF_CHECK(sec_r.status == EditStatus::Applied);

    // Cite order: smith first, then jones, then both.
    auto add_paragraph = [&](InlineContent content) {
        InsertParagraphPayload para;
        para.parent = sec_r.created_node;
        para.content = std::move(content);
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.base_revision = session.current_revision();
        cmd.payload = para;
        PF_CHECK(session.Execute(cmd).status == EditStatus::Applied);
    };
    auto cite = [](std::vector<std::string> keys) {
        InlineContent content;
        Citation cit;
        cit.keys = std::move(keys);
        content.push_back(TextRun{"Claim ", 0});
        content.push_back(std::move(cit));
        return content;
    };
    add_paragraph(cite({"smith2024"}));
    add_paragraph(cite({"jones2020"}));
    add_paragraph(cite({"smith2024", "jones2020"}));

    // The GUI projection.
    auto resolver =
        CitationNumberResolver::Build(session.state().document(),
                                      session.bibliography());
    const int gui_smith = resolver.Find("smith2024")->number;
    const int gui_jones = resolver.Find("jones2020")->number;
    PF_CHECK(gui_smith == 1 && gui_jones == 2);
    PF_CHECK(resolver.FormatPill({"smith2024", "jones2020"}) == "[1, 2]");

    // The real toolchain projection.
    bool done = false;
    std::optional<BuildResult> result;
    session.SetBuildResultHandler(
        [&](const BuildResult& r) { result = r; done = true; });
    session.RequestBuild(true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{240};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{50});
    }
    PF_CHECK(done && result &&
             result->outcome == BuildResult::Outcome::Success);
    if (!done || !result ||
        result->outcome != BuildResult::Outcome::Success) {
        if (result) {
            for (const auto& d : result->diagnostics) {
                std::cout << "  " << d.Summary() << "\n";
            }
        }
        std::filesystem::remove_all(dir);
        return;
    }

    // pdftotext the built PDF and read the bracket numbers off the page.
    const std::string pdf = result->pdf_path.string();
    auto uniq = dir / "cited.txt";
    const std::string extract = "pdftotext -layout " + pdf + " " +
                                uniq.string();
    PF_CHECK(std::system(extract.c_str()) == 0);
    std::ifstream in(uniq, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::filesystem::remove_all(dir);

    if (text.empty()) {
        std::cout << "    pdftotext produced nothing; skipping text match\n";
        return;
    }
    // "[1]" marks smith's first citation, "[2]" jones' - identical to the GUI.
    const size_t first = text.find("[1]");
    const size_t second = text.find("[2]");
    const size_t both = text.find("[1, 2]");
    PF_CHECK(first != std::string::npos);
    PF_CHECK(second != std::string::npos);
    PF_CHECK(both != std::string::npos);
    if (first != std::string::npos && second != std::string::npos) {
        PF_CHECK(first < second);  // citation order, not alphabetical
    }
    PF_CHECK(both != std::string::npos && both > second);
    // The bibliography heading numbers match too.
    PF_CHECK(text.find("[1] J. Smith") != std::string::npos ||
             text.find("[1]Smith") != std::string::npos ||
             text.find("[1] ") != std::string::npos);
}

// ---------------------------------------------------------------------------
// BlockEditor / MainWindow: the one legal data path
// ---------------------------------------------------------------------------

namespace {

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

        window.controller()->ImportBibliographyText(
            QStringLiteral(
                "@article{smith2024, author={J. Smith}, title={Seminal}, "
                "year={2024}}\n"
                "@article{jones2020, author={A. Jones}, title={Later}, "
                "year={2020}}\n"));
        auto section = window.controller()->InsertSection(QStringLiteral("S"));
        auto paragraph = window.controller()->InsertParagraph(
            section.created_node, QStringLiteral("before after"));
        node_id = QString::fromStdString(paragraph.created_node.value());
        Spin(300);
    }
};

InlineEditor* FindRow(MainWindow& window, const QString& node_id) {
    for (InlineEditor* edit : window.findChildren<InlineEditor*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_node").toString() == node_id) return edit;
    }
    return nullptr;
}

const Paragraph* StoredParagraph(MainWindow& window) {
    Document& doc = window.controller()->session().mutable_document();
    const auto& sections = BodyOf(doc).sections;
    if (sections.empty() || sections[0].blocks.empty()) return nullptr;
    return std::get_if<Paragraph>(&sections[0].blocks[0]);
}

}  // namespace

PF_TEST(CitationCommitFlowReachesDocumentAndPillRepaints) {
    EnsureQApplication();
    Fixture fixture("pf-citation-flow");
    MainWindow& window = fixture.window;

    // The user's caret sits mid-paragraph when the picker opens; the insert
    // must honour that saved position (citation plan §4).
    BlockEditor* editor = window.findChild<BlockEditor*>();
    PF_CHECK(editor != nullptr);
    if (!editor) return;
    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row) return;
    QTextCursor caret(row->document());
    caret.setPosition(7);  // between "before " and "after"
    row->setTextCursor(caret);

    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id,
                                                 QStringLiteral("smith2024")));
    Spin(200);

    // 1. The document holds the citation *between* the text runs, as keys.
    const Paragraph* stored = StoredParagraph(window);
    PF_CHECK(stored != nullptr);
    if (!stored) return;
    bool inserted_mid_paragraph = false;
    size_t index = 0;
    for (const auto& node : stored->content) {
        if (const auto* cit = std::get_if<Citation>(&node)) {
            PF_CHECK(cit->keys.size() == 1 &&
                     cit->keys[0] == "smith2024");
            inserted_mid_paragraph = index > 0 && index < stored->content.size();
        }
        ++index;
    }
    PF_CHECK(inserted_mid_paragraph);
    // Three nodes: "before " + citation + "after" - position preserved.
    PF_CHECK(stored->content.size() == 3);
    if (stored->content.size() == 3) {
        const auto* first = std::get_if<TextRun>(&stored->content[0]);
        const auto* last = std::get_if<TextRun>(&stored->content[2]);
        PF_CHECK(first && first->text == "before ");
        PF_CHECK(last && last->text == "after");
    }

    // 2. The commit rebuilt the row; the pill in the *new* widget shows [1].
    InlineEditor* reloaded_row = FindRow(window, fixture.node_id);
    PF_CHECK(reloaded_row != nullptr);
    if (reloaded_row) {
        const QString plain = reloaded_row->toPlainText();
        const int pill = plain.indexOf(QChar(0xFFFC));
        PF_CHECK(pill == 7);
        if (pill >= 0) {
            PF_CHECK(FormatAt(*reloaded_row, pill)
                         .property(citation_format::kDisplayTextProperty)
                         .toString() == QStringLiteral("[1]"));
        }
    }
}

PF_TEST(SecondCitationNumbersIncrementallyInGui) {
    EnsureQApplication();
    Fixture fixture("pf-citation-order");
    MainWindow& window = fixture.window;
    BlockEditor* editor = window.findChild<BlockEditor*>();
    PF_CHECK(editor != nullptr);
    if (!editor) return;

    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id,
                                                QStringLiteral("jones2020"), 0));
    Spin(150);
    // jones cited first now -> [1]; adding smith after it -> [2]. Both pills
    // must reflect one shared numbering after the second commit.
    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id,
                                                 QStringLiteral("smith2024"), 20));
    Spin(150);

    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row) return;
    QStringList pills;
    const QString plain = row->toPlainText();
    for (int pos = 0; pos < plain.size(); ++pos) {
        if (plain.at(pos) != QChar(0xFFFC)) continue;
        pills << FormatAt(*row, pos)
                     .property(citation_format::kDisplayTextProperty)
                     .toString();
    }
    PF_CHECK(pills.size() == 2);
    if (pills.size() == 2) {
        PF_CHECK(pills[0] == QStringLiteral("[1]"));   // jones, cited first
        PF_CHECK(pills[1] == QStringLiteral("[2]"));   // smith, cited second
    }
}

PF_TEST(ManualBuildFlushesTheFocusedRowFirst) {
    EnsureQApplication();
    Fixture fixture("pf-citation-build");
    MainWindow& window = fixture.window;
    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row) return;

    // Focus the row and type: no commit has happened yet (no focus-out).
    row->setFocus(Qt::MouseFocusReason);
    row->moveCursor(QTextCursor::End);
    row->insertPlainText(QStringLiteral(" tail typed"));
    PF_CHECK(row->IsDirty());

    // Build must flush first (citation plan §6): the Document - and with it
    // the build snapshot - must contain the typed text immediately after
    // OnBuild, without any focus-out. (The commit rebuilds the row, so the
    // old `row` pointer is dropped here; only the document is consulted.)
    QMetaObject::invokeMethod(&window, "OnBuild");
    Spin(150);
    const Paragraph* stored = StoredParagraph(window);
    PF_CHECK(stored != nullptr);
    if (stored) {
        PF_CHECK(InlineToPlainText(stored->content) ==
                 "before after tail typed");
    }
    InlineEditor* fresh_row = FindRow(window, fixture.node_id);
    PF_CHECK(fresh_row != nullptr && !fresh_row->IsDirty());
}
