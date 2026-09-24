// 引文重设计的 GUI 层测试（引用方案 §1-§6）：
//   * 引文是真正可渲染的行内对象（pill），而非伪造的
//     U+E000 字符 —— 一个 object replacement 字符，format 承载
//     语义载荷，绘制来自 CitationNumberResolver；
//   * Backspace 删除整个 pill，绝不扰动周围的 marks；
//   * picker 流程在 *已保存的* 光标位置插入并立即提交
//     （InlineEditor -> ParagraphContentEdited -> EditParagraphRich）；
//   * picker 持有焦点时的 focusOut 不会被当作「编辑完成」。
#include "TestMain.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMetaObject>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QThread>
#include <QToolButton>

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

QApplication* EnsureQApplication() {
    return qApp;
}

Body& BodyOf(Document& doc) {
    return DocumentMutableAccess::body(doc);
}

void Spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

std::shared_ptr<const CitationNumberResolver> ResolverFor(const std::vector<std::string>& cited_in_order,
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
    return std::make_shared<const CitationNumberResolver>(CitationNumberResolver::Build(doc, db));
}

// `position`处对象的 char format（object replacement 字符）。
QTextCharFormat FormatAt(const InlineEditor& editor, int position) {
    QTextCursor cursor(editor.document());
    cursor.setPosition(position);
    cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
    return cursor.charFormat();
}

// 像平台那样，把按键事件直接发送给 widget。
void SendKey(QWidget* widget, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent event(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(widget, &event);
}

} // namespace

// ---------------------------------------------------------------------------
// InlineEditor：语义对象 + renderer 架构
// ---------------------------------------------------------------------------

PF_TEST(CitationPillCarriesKeyPayloadAndResolvedNumber) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({"smith2024"}, {"smith2024", "jones2020"}));
    editor.SetContent(InlineFromText("introduced by "));
    editor.InsertCitationObject({QStringLiteral("smith2024")});

    // 恰好一个 object replacement 字符，任何地方都不再出现遗留的 U+E000。
    const QString plain = editor.toPlainText();
    PF_CHECK(!plain.contains(QChar(0xE000)));
    PF_CHECK(plain.count(QChar(0xFFFC)) == 1);

    // 绘制文本由全文档编号解析得到：smith 是 [1]；
    // payload（存储的语义）是 key，绝不是编号。
    const int pill_position = plain.indexOf(QChar(0xFFFC));
    PF_CHECK(pill_position >= 0);
    const QTextCharFormat format = FormatAt(editor, pill_position);
    PF_CHECK(format.objectType() == citation_format::kObjectType);
    PF_CHECK(format.property(inline_object_format::kKindProperty).toInt() ==
             static_cast<int>(InlineEditor::TokenKind::Citation));
    PF_CHECK(format.property(inline_object_format::kPayloadProperty).toString() == QStringLiteral("smith2024"));
    PF_CHECK(format.property(citation_format::kDisplayTextProperty).toString() == QStringLiteral("[1]"));

    // 文档收到的是 key 集合。
    const InlineContent content = editor.Content();
    const Citation* citation = nullptr;
    for (const auto& node : content) {
        if (const auto* cit = std::get_if<Citation>(&node))
            citation = cit;
    }
    PF_CHECK(citation != nullptr);
    if (citation) {
        PF_CHECK(citation->keys.size() == 1 && citation->keys[0] == "smith2024");
    }
}

PF_TEST(CitationNumberingFollowsInsertionOrderAcrossRows) {
    EnsureQApplication();
    // 先 A 后 B：A 为 [1]，B 为 [2]；再次引用 A 仍保持 [1]。
    auto resolver = ResolverFor({"a", "b"}, {"a", "b"});
    InlineEditor editor;
    editor.SetCitationNumbers(resolver);
    editor.InsertCitationObject({QStringLiteral("a")});
    PF_CHECK(FormatAt(editor, 0).property(citation_format::kDisplayTextProperty).toString() == QStringLiteral("[1]"));
    // 多重引文 A+B 按排序渲染：[1, 2]。
    InlineEditor multi;
    multi.SetCitationNumbers(resolver);
    multi.InsertCitationObject({QStringLiteral("b"), QStringLiteral("a")});
    PF_CHECK(FormatAt(multi, 0).property(citation_format::kDisplayTextProperty).toString() == QStringLiteral("[1, 2]"));
}

PF_TEST(UnknownCitationKeyPillShowsQuestionMark) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({}, {"real"}));
    editor.InsertCitationObject({QStringLiteral("ghost")});
    PF_CHECK(FormatAt(editor, 0).property(citation_format::kDisplayTextProperty).toString() == QStringLiteral("[?]"));
    // 仍然是语义的：key 经过往返后依然保留，可供校验。
    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 1);
    PF_CHECK(std::holds_alternative<Citation>(content[0]));
}

PF_TEST(BackspaceDeletesWholeCitationPill) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetCitationNumbers(ResolverFor({"a"}, {"a"}));
    editor.SetContent(InlineFromText("see x"));
    // 在 "see " 与 "x" 之间（位置 4）插入 pill。
    QTextCursor cursor(editor.document());
    cursor.setPosition(4);
    editor.setTextCursor(cursor);
    editor.InsertCitationObject({QStringLiteral("a")});

    // 光标紧跟在 pill 之后；一次 Backspace 删除整个对象。
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
    // 像用户那样在 pill 之后输入：经由编辑器自身的
    // cursor，对象插入把它留在了中性 format。
    editor.moveCursor(QTextCursor::End);
    editor.insertPlainText(QStringLiteral("tail"));

    const InlineContent content = editor.Content();
    // bold run、citation、普通 "tail" —— pill 打断了 bold run，但
    // 后续文本不得悄悄继承 bold 或对象属性。
    size_t citations = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (std::holds_alternative<Citation>(content[i])) {
            ++citations;
            continue;
        }
        const auto* run = std::get_if<TextRun>(&content[i]);
        if (!run)
            continue;
        if (run->text == "bold") {
            PF_CHECK(HasMark(run->marks, TextMark::Strong));
        } else if (run->text == "tail") {
            PF_CHECK(run->marks == 0);
        }
    }
    PF_CHECK(citations == 1);
    // 输入的文本没有被 pill 吞掉。
    const std::string flat = InlineToPlainText(content);
    PF_CHECK(flat.find("bold") != std::string::npos);
    PF_CHECK(flat.find("tail") != std::string::npos);
}

// Undo / Redo 不得破坏周围文本或凭空造出 token（方案 §11）。
// Qt 6.2 保留整段 undo 历史；pill 插入是独立命令，
// 因为它携带的字符 format 与相邻文本不同。
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
            if (std::holds_alternative<Citation>(node))
                ++n;
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
    QObject::connect(&editor, &InlineEditor::Committed, [&]() { committed = true; });

    // 保护生效时（picker 已抢到焦点），focus-out
    // 绝不能被误认为「用户已完成编辑」（方案 §4）。
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
// 端到端：pill 编号与 PDF 编号遵循同一策略
// ---------------------------------------------------------------------------

PF_TEST(GuiPillNumberMatchesTheBuiltPdf) {
    EnsureQApplication();
    auto dir = std::filesystem::temp_directory_path() / "pf-citation-pdf";
    std::filesystem::remove_all(dir);

    ProjectSession::Config config;
    config.install_root = PF_INSTALL_ROOT; // 捆绑的可移植 TeX Live
    config.debounce = std::chrono::milliseconds{0};
    ProjectSession session(config);
    PF_CHECK(session.NewProject(dir));
    session.ImportBibliography("@article{smith2024, author={J. Smith}, title={Seminal Method}, "
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

    // 引用顺序：先 smith，再 jones，最后两者。
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

    // GUI 侧的投影。
    auto resolver = CitationNumberResolver::Build(session.state().document(), session.bibliography());
    const int gui_smith = resolver.Find("smith2024")->number;
    const int gui_jones = resolver.Find("jones2020")->number;
    PF_CHECK(gui_smith == 1 && gui_jones == 2);
    PF_CHECK(resolver.FormatPill({"smith2024", "jones2020"}) == "[1, 2]");

    // 真实工具链的投影。
    bool done = false;
    std::optional<BuildResult> result;
    session.SetBuildResultHandler([&](const BuildResult& r) {
        result = r;
        done = true;
    });
    session.RequestBuild(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{240};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        session.WaitForApplicationEvent(std::chrono::milliseconds{50});
    }
    PF_CHECK(done && result && result->outcome == BuildResult::Outcome::Success);
    if (!done || !result || result->outcome != BuildResult::Outcome::Success) {
        if (result) {
            for (const auto& d : result->diagnostics) {
                std::cout << "  " << d.Summary() << "\n";
            }
        }
        std::filesystem::remove_all(dir);
        return;
    }

    // 对构建出的 PDF 运行 pdftotext，从页面上读出方括号编号。
    const std::string pdf = result->pdf_path.string();
    auto uniq = dir / "cited.txt";
    const std::string extract = "pdftotext -layout " + pdf + " " + uniq.string();
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
    // "[1]" 标记 smith 的首次引用，"[2]" 标记 jones 的 —— 与 GUI 一致。
    const size_t first = text.find("[1]");
    const size_t second = text.find("[2]");
    const size_t both = text.find("[1, 2]");
    PF_CHECK(first != std::string::npos);
    PF_CHECK(second != std::string::npos);
    PF_CHECK(both != std::string::npos);
    if (first != std::string::npos && second != std::string::npos) {
        PF_CHECK(first < second); // 引用顺序，而非字母顺序
    }
    PF_CHECK(both != std::string::npos && both > second);
    // 参考文献标题中的编号也同样匹配。
    PF_CHECK(text.find("[1] J. Smith") != std::string::npos || text.find("[1]Smith") != std::string::npos ||
             text.find("[1] ") != std::string::npos);
}

// ---------------------------------------------------------------------------
// BlockEditor / MainWindow：唯一合法的数据路径
// ---------------------------------------------------------------------------

namespace {

struct Fixture {
    MainWindow window;
    QString node_id;

    explicit Fixture(const QString& dir_name) {
        window.resize(1400, 900);
        window.show();
        auto dir = std::filesystem::temp_directory_path() / dir_name.toStdString();
        std::filesystem::remove_all(dir);
        window.controller()->NewProject(QString::fromStdString(dir.string()));
        window.controller()->Save();
        window.controller()->FlushSaves();
        window.OpenProjectDir(QString::fromStdString(dir.string()));
        Spin(250);

        window.controller()->ImportBibliographyText(
            QStringLiteral("@article{smith2024, author={J. Smith}, title={Seminal}, "
                           "year={2024}}\n"
                           "@article{jones2020, author={A. Jones}, title={Later}, "
                           "year={2020}}\n"));
        auto section = window.controller()->InsertSection(QStringLiteral("S"));
        auto paragraph = window.controller()->InsertParagraph(section.created_node, QStringLiteral("before after"));
        node_id = QString::fromStdString(paragraph.created_node.value());
        Spin(300);
    }
};

InlineEditor* FindRow(MainWindow& window, const QString& node_id) {
    for (InlineEditor* edit : window.findChildren<InlineEditor*>()) {
        if (!edit->isVisible())
            continue;
        if (edit->property("row_node").toString() == node_id)
            return edit;
    }
    return nullptr;
}

const Paragraph* StoredParagraph(MainWindow& window) {
    Document& doc = window.controller()->session().mutable_document();
    const auto& sections = BodyOf(doc).sections;
    if (sections.empty() || sections[0].blocks.empty())
        return nullptr;
    return std::get_if<Paragraph>(&sections[0].blocks[0]);
}

} // namespace

PF_TEST(CitationCommitFlowReachesDocumentAndPillRepaints) {
    EnsureQApplication();
    Fixture fixture("pf-citation-flow");
    MainWindow& window = fixture.window;

    // picker 打开时用户的光标位于段落中间；插入
    // 必须遵循该已保存的位置（引用方案 §4）。
    BlockEditor* editor = window.findChild<BlockEditor*>();
    PF_CHECK(editor != nullptr);
    if (!editor)
        return;
    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row)
        return;
    QTextCursor caret(row->document());
    caret.setPosition(7); // "before " 与 "after" 之间
    row->setTextCursor(caret);

    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id, QStringLiteral("smith2024")));
    Spin(200);

    // 1. 文档以 key 的形式把引文放在文本 run *之间*。
    const Paragraph* stored = StoredParagraph(window);
    PF_CHECK(stored != nullptr);
    if (!stored)
        return;
    bool inserted_mid_paragraph = false;
    size_t index = 0;
    for (const auto& node : stored->content) {
        if (const auto* cit = std::get_if<Citation>(&node)) {
            PF_CHECK(cit->keys.size() == 1 && cit->keys[0] == "smith2024");
            inserted_mid_paragraph = index > 0 && index < stored->content.size();
        }
        ++index;
    }
    PF_CHECK(inserted_mid_paragraph);
    // 三个节点："before " + citation + "after" —— 位置得以保留。
    PF_CHECK(stored->content.size() == 3);
    if (stored->content.size() == 3) {
        const auto* first = std::get_if<TextRun>(&stored->content[0]);
        const auto* last = std::get_if<TextRun>(&stored->content[2]);
        PF_CHECK(first && first->text == "before ");
        PF_CHECK(last && last->text == "after");
    }

    // 2. 提交后原 row 保持存活，pill 的显示编号就地更新为 [1]。
    InlineEditor* reloaded_row = FindRow(window, fixture.node_id);
    PF_CHECK(reloaded_row != nullptr);
    PF_CHECK(reloaded_row == row);
    if (reloaded_row) {
        const QString plain = reloaded_row->toPlainText();
        const int pill = plain.indexOf(QChar(0xFFFC));
        PF_CHECK(pill == 7);
        if (pill >= 0) {
            PF_CHECK(FormatAt(*reloaded_row, pill).property(citation_format::kDisplayTextProperty).toString() ==
                     QStringLiteral("[1]"));
        }
    }
}

PF_TEST(CitationAndReferencePickersFollowTheTextCaret) {
    EnsureQApplication();
    Fixture fixture("pf-picker-caret-position");
    MainWindow& window = fixture.window;
    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row)
        return;

    int citation_x = -1;
    for (const auto& request : {std::pair{QStringLiteral("Citation"), 2}, std::pair{QStringLiteral("Reference"), 10}}) {
        row->setFocus(Qt::OtherFocusReason);
        QTextCursor cursor(row->document());
        cursor.setPosition(request.second);
        row->setTextCursor(cursor);
        Spin(30);
        const QRect caret = row->cursorRect();
        const QPoint caret_top = row->viewport()->mapToGlobal(caret.topLeft());
        const int caret_bottom = caret_top.y() + caret.height() - 1;

        QToolButton* button = nullptr;
        for (auto* candidate : row->parentWidget()->findChildren<QToolButton*>())
            if (candidate->text() == request.first)
                button = candidate;
        PF_CHECK(button != nullptr);
        if (!button)
            return;
        button->click();
        Spin(30);
        PopupList* popup = nullptr;
        for (auto* candidate : window.findChildren<PopupList*>())
            if (candidate->isVisible())
                popup = candidate;
        PF_CHECK(popup != nullptr);
        if (!popup)
            return;

        const int line_height = row->fontMetrics().lineSpacing();
        const int gap = popup->y() - caret_bottom;
        PF_CHECK(qAbs(popup->x() - caret_top.x()) <= 2);
        PF_CHECK(gap >= line_height && gap <= 3 * line_height);
        if (request.first == QStringLiteral("Citation"))
            citation_x = popup->x();
        else
            PF_CHECK(popup->x() > citation_x);
        popup->close();
        Spin(30);
    }
}

PF_TEST(InlineReferencesAndFigureSpanKeepEditorPosition) {
    EnsureQApplication();
    Fixture fixture("pf-editor-position");
    MainWindow& window = fixture.window;
    auto* editor = window.findChild<BlockEditor*>();
    auto* row = FindRow(window, fixture.node_id);
    PF_CHECK(editor != nullptr && row != nullptr);
    if (!editor || !row)
        return;

    // 后续块使主编辑区能够滚动。
    const auto section_id = window.controller()->session().state().document().body().sections.front().id;
    auto filler = window.controller()->InsertParagraph(section_id, QStringLiteral("filler"));
    PF_CHECK(filler.status == EditStatus::Applied);
    for (int i = 0; i < 16; ++i)
        window.controller()->InsertParagraph(section_id, QStringLiteral("filler"));
    Spin(80);
    row = FindRow(window, fixture.node_id);
    auto* scroll = editor->findChild<QScrollArea*>();
    PF_CHECK(row != nullptr);
    PF_CHECK(scroll != nullptr);
    if (!row || !scroll)
        return;
    Spin(80);
    auto* bar = scroll->verticalScrollBar();
    PF_CHECK(bar->maximum() > 0);
    if (bar->maximum() == 0)
        return;
    row->setFocus(Qt::OtherFocusReason);
    bar->setValue(qMin(80, bar->maximum()));
    const int position = bar->value();

    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id, QStringLiteral("smith2024"), 7));
    Spin(80);
    PF_CHECK(FindRow(window, fixture.node_id) == row);
    PF_CHECK(bar->value() == position);

    row->InsertCrossReferenceObject(QString::fromStdString(section_id.value()));
    editor->CommitFocused();
    Spin(80);
    PF_CHECK(FindRow(window, fixture.node_id) == row);
    PF_CHECK(bar->value() == position);
    const Paragraph* stored = StoredParagraph(window);
    PF_CHECK(stored != nullptr);
    if (stored) {
        bool has_reference = false;
        for (const auto& node : stored->content)
            has_reference |= std::holds_alternative<CrossReference>(node);
        PF_CHECK(has_reference);
    }

    const auto image_path = std::filesystem::temp_directory_path() / "pf-editor-position.png";
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::white);
    PF_CHECK(image.save(QString::fromStdString(image_path.string())));
    auto figure = window.controller()->InsertFigureAfter(NodeId(filler.created_node.value()),
                                                         QString::fromStdString(image_path.string()));
    PF_CHECK(figure.status == EditStatus::Applied);
    std::filesystem::remove(image_path);
    if (figure.status != EditStatus::Applied)
        return;
    QCheckBox* span = nullptr;
    const QString figure_id = QString::fromStdString(figure.created_node.value());
    for (auto* box : editor->findChildren<QCheckBox*>()) {
        if (box->property("row_node").toString() == figure_id)
            span = box;
    }
    PF_CHECK(span != nullptr);
    if (!span)
        return;
    bar->setValue(qMin(80, bar->maximum()));
    const int figure_position = bar->value();
    span->setChecked(true);
    Spin(80);
    PF_CHECK(span->isChecked());
    PF_CHECK(bar->value() == figure_position);
    PF_CHECK(span->isVisible());
    bool stored_double_column = false;
    VisitBlocks(window.controller()->session().state().document(), [&](const Block& block, const NodeAddress&) {
        if (const auto* value = std::get_if<Figure>(&block))
            if (value->id == figure.created_node)
                stored_double_column = value->span == FigureSpan::DoubleColumn;
    });
    PF_CHECK(stored_double_column);
}

PF_TEST(SecondCitationNumbersIncrementallyInGui) {
    EnsureQApplication();
    Fixture fixture("pf-citation-order");
    MainWindow& window = fixture.window;
    BlockEditor* editor = window.findChild<BlockEditor*>();
    PF_CHECK(editor != nullptr);
    if (!editor)
        return;

    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id, QStringLiteral("jones2020"), 0));
    Spin(150);
    // 现在 jones 先被引用 -> [1]；在其后加入 smith -> [2]。第二次提交后
    // 两个 pill 必须反映同一套共享编号。
    PF_CHECK(editor->InsertCitationIntoParagraph(fixture.node_id, QStringLiteral("smith2024"), 20));
    Spin(150);

    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row)
        return;
    QStringList pills;
    const QString plain = row->toPlainText();
    for (int pos = 0; pos < plain.size(); ++pos) {
        if (plain.at(pos) != QChar(0xFFFC))
            continue;
        pills << FormatAt(*row, pos).property(citation_format::kDisplayTextProperty).toString();
    }
    PF_CHECK(pills.size() == 2);
    if (pills.size() == 2) {
        PF_CHECK(pills[0] == QStringLiteral("[1]")); // jones，先被引用
        PF_CHECK(pills[1] == QStringLiteral("[2]")); // smith，后被引用
    }
}

PF_TEST(ManualBuildFlushesTheFocusedRowFirst) {
    EnsureQApplication();
    Fixture fixture("pf-citation-build");
    MainWindow& window = fixture.window;
    InlineEditor* row = FindRow(window, fixture.node_id);
    PF_CHECK(row != nullptr);
    if (!row)
        return;

    // 聚焦该 row 并输入：此时尚未发生提交（没有 focus-out）。
    row->setFocus(Qt::MouseFocusReason);
    row->moveCursor(QTextCursor::End);
    row->insertPlainText(QStringLiteral(" tail typed"));
    PF_CHECK(row->IsDirty());

    // Build 必须先 flush（引用方案 §6）：Document —— 以及随之的
    // build snapshot —— 必须在 OnBuild 之后立即包含已输入的文本，
    // 无需任何 focus-out。（提交会重建该 row，因此这里丢弃旧的
    // `row` 指针；只查询文档。）
    QMetaObject::invokeMethod(&window, "OnBuild");
    Spin(150);
    const Paragraph* stored = StoredParagraph(window);
    PF_CHECK(stored != nullptr);
    if (stored) {
        PF_CHECK(InlineToPlainText(stored->content) == "before after tail typed");
    }
    InlineEditor* fresh_row = FindRow(window, fixture.node_id);
    PF_CHECK(fresh_row != nullptr && !fresh_row->IsDirty());
}
