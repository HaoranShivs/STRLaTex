// 复现两个 GUI 报告的问题：
//   1. 点击格式按钮会把文本行撑成一大片空白区域
//      （随后按 Delete 又能「恢复」）；
//   2. 粗体/斜体在默认模板下有效，在 IEEE 下却无效。
//
// 两者都通过真实的 MainWindow、真实工具栏和真实 InlineEditor 驱动，
// 因为这两个症状在领域层都不存在。
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

// 某个节点 id 对应的可见富文本编辑器。
InlineEditor* FindRich(MainWindow& window, const QString& node_id) {
    for (InlineEditor* edit : window.findChildren<InlineEditor*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_node").toString() == node_id) return edit;
    }
    return nullptr;
}

// 拥有 `editor` 的那张卡片上格式栏中的每个 QToolButton。
std::vector<QToolButton*> ToolbarButtons(MainWindow& window) {
    std::vector<QToolButton*> out;
    for (QToolButton* button : window.findChildren<QToolButton*>()) {
        if (!button->isVisible()) continue;  // 来自某次重建的陈旧工具栏
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

// 构建一个项目，其中只有一个包含单个单词的 Text 行，可供格式化。
struct Fixture {
    MainWindow window;
    QString node_id;

    explicit Fixture(const QString& dir_name) {
        window.resize(1400, 900);
        window.show();
        auto dir = std::filesystem::temp_directory_path() /
                   dir_name.toStdString();
        std::filesystem::remove_all(dir);
        // 先创建项目，再保存并重新打开：正是「打开」这一操作把窗口从欢迎页
        // 切换到工作区，与真实会话完全一致。缺少这一步，编辑器行会一直隐藏。
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

// document 为单个段落存储的内容。
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

// ---- 问题 1：点击工具栏会不会把行高撑大？----

PF_TEST(ToolbarClickDoesNotGrowTheTextRow) {
    EnsureQApplication();
    Fixture fixture("pf-toolbar-height");
    InlineEditor* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor) return;

    const int before = editor->height();

    // 真实用户在操作工具栏之前会先点进段落：格式条属于 block 界面外壳的一部分，
    // 仅在悬停/聚焦时显示（UI 方案 §2），因此先聚焦，再查找按钮。
    editor->setFocus(Qt::MouseFocusReason);
    Spin(120);

    // 完全按照用户的操作点击斜体按钮。
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
    // 该行可能被重建，但绝不能变成一大块空白框。
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

    // 焦点必须留在该行内，否则点击按钮会在编辑过程中提交该行
    // 并重建每一张卡片。
    InlineEditor* after_editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(after_editor != nullptr && after_editor->hasFocus());
}

// ---- 问题 2：两种模板下的标记 ----

namespace {

// 通过真实工具栏给 Text 行的第一个单词加上粗体和斜体，
// 并回传 document 与 LaTeX 最终得到的结果。
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
    // 真实用户在操作工具栏之前会先点进段落。
    editor->setFocus(Qt::MouseFocusReason);
    Spin(120);

    // 选中整个单词。
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

    // 按用户的方式提交：离开该行（点击别处）。
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
    // 粗体+斜体嵌套为 \textbf{\emph{word}}，因此两种形式都接受。
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
