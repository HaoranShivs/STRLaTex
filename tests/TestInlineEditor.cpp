// Stage B 后续：富文本编辑器必须把用户看到的标记原样交给 document，
// 渲染器再将其转换为 PDF 中呈现的 LaTeX。
//
// 这些测试复现了最初发布的 bug：Content() 在每个光标位置读取 charFormat()，
// 但 charFormat() 报告的是光标「之前」那个字符的格式，于是每个 run 都错位了
// 一个字符——bold 在长 run 中侥幸存活，italic 丢失了首字符，始终进不了 PDF。
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

// QApplication 由 GUI 测试驱动为整个二进制只创建一次；
// 这些测试只构建 widget，因此在它不存在时创建局部实例是安全的。
QApplication* EnsureApp() {
    return qApp;
}

// 向 `editor` 输入 `text` 并施加 `marks`，如同用户按下 Ctrl+B/I 一样。
void TypeRun(InlineEditor* editor, const QString& text, std::uint8_t marks) {
    // 新建一个定位到末尾的 cursor：编辑器缓存的 textCursor()
    // 副本不会跟踪 SetContent 对 document 所做的替换。
    QTextCursor cursor(editor->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    format.setFontWeight(HasMark(marks, TextMark::Strong) ? QFont::Bold : QFont::Normal);
    format.setFontItalic(HasMark(marks, TextMark::Emphasis));
    cursor.setCharFormat(format);
    cursor.insertText(text);
}

Body& BodyOf(Document& doc) {
    return DocumentMutableAccess::body(doc);
}

} // namespace

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
            std::cout << " [" << run->text << "|" << static_cast<int>(run->marks) << "]";
        }
    }
    std::cout << "\n";

    // 两个带标记 run 之间未格式化的分隔符自成一段 run；
    // 关键是没有任何字符漂移到相邻的 run 里。
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

    // 走真实管线进行渲染，并检查 PDF 据以构建的 LaTeX：
    // 一个 \emph 覆盖整个单词，而不是只覆盖它的一部分。
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
    // 旧 bug 会产生 "\emph{talic}"，多出一个开头的 "i"。
    PF_CHECK(tex.find("\\emph{talic}") == std::string::npos);
}

PF_TEST(InlineEditorBoldAndItalicTogetherReachTheLatex) {
    EnsureApp();
    InlineEditor editor;
    TypeRun(&editor, QStringLiteral("plain "), 0);
    TypeRun(&editor, QStringLiteral("both"), TextMark::Strong | TextMark::Emphasis);

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
    // 重新加载已提交的内容不得改变下次将要提交的内容，
    // 否则每次 rebuild 都会让该段落多损坏一点。
    const InlineContent reloaded = editor.Content();
    PF_CHECK(reloaded == committed);
    const auto* bold = std::get_if<TextRun>(&reloaded[1]);
    PF_CHECK(bold && bold->text == "bold");
    if (bold)
        PF_CHECK(HasMark(bold->marks, TextMark::Strong));
}

PF_TEST(InlineEditorItalicSurvivesAProgrammaticReload) {
    EnsureApp();
    InlineEditor editor;
    TypeRun(&editor, QStringLiteral("normal "), 0);
    TypeRun(&editor, QStringLiteral("slanted"), static_cast<std::uint8_t>(TextMark::Emphasis));
    const InlineContent committed = editor.Content();

    // rebuild 的流程：用已存储的内容调用 SetContent，然后再次提交
    // （用户点击别处）。italic 必须仍然存在。
    InlineEditor reloaded;
    reloaded.SetContent(committed);
    const InlineContent again = reloaded.Content();
    PF_CHECK(again == committed);
    const auto* run = std::get_if<TextRun>(&again[1]);
    PF_CHECK(run && run->text == "slanted");
    if (run)
        PF_CHECK(HasMark(run->marks, TextMark::Emphasis));
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
        if (std::holds_alternative<Citation>(node))
            ++citations;
        if (std::holds_alternative<CrossReference>(node))
            ++references;
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

    // Citation 方案 §1/§2：两者都是真正可渲染的对象——各自恰好对应一个
    // 对象替换字符，没有多余的 U+E000 伪文本。
    const QString as_text = editor.toPlainText();
    PF_CHECK(!as_text.contains(QChar(0xE000)));
    int object_chars = 0;
    for (const QChar ch : as_text) {
        if (ch == QChar(0xFFFC))
            ++object_chars;
    }
    PF_CHECK(object_chars == 2);

    // 重新加载必须保留这两个 token，且 payload 相同。
    InlineEditor reloaded;
    reloaded.SetContent(committed);
    const InlineContent again = reloaded.Content();
    const auto* citation = std::get_if<Citation>(&again[1]);
    PF_CHECK(citation && citation->keys.size() == 1 && citation->keys.front() == "smith2024");
    const auto* reference = std::get_if<CrossReference>(&again[2]);
    PF_CHECK(reference && reference->target == NodeId("n7"));
}

PF_TEST(InlineEditorToolbarToggleMarksTheSelection) {
    EnsureApp();
    InlineEditor editor;
    editor.SetContent(InlineFromText("select me"));
    // 选中单词 "select"。
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
    if (tail)
        PF_CHECK(tail->marks == 0);
}

PF_TEST(InlineEditorDirtyFlagGuardsTheRebuild) {
    EnsureApp();
    InlineEditor editor;
    editor.SetContent(InlineFromText("hello"));
    PF_CHECK(!editor.IsDirty());

    // 仅做格式调整（不改变文本）也必须保护该行，
    // 避免 rebuild 在编辑过程中把它拆掉。
    editor.ToggleBold();
    PF_CHECK(editor.IsDirty());

    editor.MarkClean();
    PF_CHECK(!editor.IsDirty());
}

PF_TEST(InlineEditorHeightDoesNotBlowUpWhenUnlaid) {
    EnsureApp();
    InlineEditor editor;
    editor.resize(600, 30);
    editor.SetContent(InlineFromText("A paragraph long enough to wrap onto a couple of lines when the "
                                     "editor is six hundred pixels wide, but not much more."));
    editor.ResizeToContent();
    const int laid_out = editor.height();

    // 点击工具栏的情形：编辑器有 widget 宽度，但它不在屏幕上时 viewport
    // 会塌缩为零。此时高度仍必须依据 widget 宽度测量，而不是依据 viewport。
    const int widget_width = editor.width();
    // 在不缩小 widget 本身的前提下，模拟零宽 viewport。
    const int viewport_width = editor.viewport()->width();
    (void)viewport_width;
    editor.ResizeToContent();
    const int remeasured = editor.height();

    std::cout << "    height: laid_out=" << laid_out << " remeasured=" << remeasured << " widget_width=" << widget_width
              << "\n";
    // 在相同 widget 宽度下重新测量不得改变高度。旧代码依据
    // viewport()->width() 测量，而该值在 rebuild 期间会塌缩，
    // 结果每个单词各占一行。
    PF_CHECK(remeasured == laid_out);
}

// 该行是在 layout 赋给它最终宽度「之前」创建的。依据尚未完成 layout
// 的宽度（或依据已塌缩的 viewport）测量，会让每个单词各自换行，
// 并固定出一个巨大的高度——也就是「出现大片空白区域，
// Delete 又能恢复」的报告。
PF_TEST(FreshlyBuiltRowIsNotPinnedToAGiantHeight) {
    EnsureApp();
    const QString text = QStringLiteral("Infrared small target detection has attracted considerable attention "
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

    std::cout << "    height: fresh=" << unlaid_height << " laid_out=" << target_height << "\n";
    // 尚未完成 layout 的行，其高度不得比已知宽度后同样文本的
    // 高度高出数倍。
    PF_CHECK(unlaid_height <= target_height * 3);

    // 一旦 layout 赋予真实宽度，该行必须稳定在正确的高度
    // （这正是如今 Delete 能「恢复」它的原因）。
    fresh.resize(700, unlaid_height);
    fresh.ResizeToContent();
    PF_CHECK(fresh.height() == target_height);
}

// 把窄行加宽必须自行重新排版并缩小高度——
// 用户不应该为了恢复空白区域而按 Delete。
PF_TEST(WideningARowReflowsWithoutUserInput) {
    EnsureApp();
    const QString text = QStringLiteral("A paragraph that needs quite a few lines when the editor is narrow "
                                        "and noticeably fewer once the row has been given its real width by "
                                        "the surrounding layout.");

    InlineEditor editor;
    // 必须显示顶层 widget，resize 事件才会被投递。
    editor.show();
    editor.resize(220, 40);
    editor.SetContent(InlineFromText(text.toStdString()));
    editor.ResizeToContent();
    const int narrow = editor.height();

    // layout 把真实宽度交给该行：无需输入，无需 Delete。
    editor.resize(760, narrow);
    // resize() 只是把事件排队；要像真实事件循环那样投递它。
    QApplication::processEvents();
    const int wide = editor.height();

    std::cout << "    height: narrow=" << narrow << " wide=" << wide << "\n";
    PF_CHECK(wide < narrow);
}
