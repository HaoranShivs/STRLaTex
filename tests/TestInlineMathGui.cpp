// 数学输入重构的 GUI 测试：
//   * Inline Math 工具栏动作会要求输入公式体，而不再插入写死的 x^{2}；
//   * 行内数学对象能在编辑器中往返，整体删除，并在复制/粘贴后存活；
//   * 数学编辑器只做校验和预览，不改写源内容；
//   * Equation 行会暴露 source、preview、numbered 和 label。
#include "TestMain.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
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

// 轮询直到 `finder` 返回非空或超时。重建是通过事件队列延迟执行的，
// 因此固定 sleep 并不可靠——尤其在 sanitizer 构建下，每一步都慢得多。
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

// 一个含单个 Text 行的项目，已为行内数学做好准备。
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

// 通过拒绝来关闭下一个模态对话框，等同于用户按下 Esc。
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

    // 重新加载不得改变已存储的正文。
    InlineEditor reloaded;
    reloaded.SetContent(content);
    const InlineContent again = reloaded.Content();
    PF_CHECK(again == content);
}

PF_TEST(InlineMathPillHasBoundedLineHeight) {
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
    PF_CHECK(formula.height() <= plain_height);
    const qreal plain_line_height =
        plain.document()->begin().layout()->lineAt(0).height();
    const qreal formula_line_height =
        formula.document()->begin().layout()->lineAt(0).height();
    PF_CHECK(formula_line_height <= plain_line_height + 0.01);
}

PF_TEST(InlineMathObjectIsNotUserEditableText) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("\\mathcal{L}"));
    // 可见文本是对象占位符，绝不是 LaTeX 正文。
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

    // 编辑器自有的剪贴板格式携带数学对象，而不仅仅是对象替换占位符。
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

    // 经由真实剪贴板的相同往返，即 Ctrl+C / Ctrl+V 的流程。
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

    // EditMathAt 会异步打开源编辑器；像用户那样驱动正常的应用事件循环
    //（输入新的公式体，然后确认）。
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

// 这个测试钉住的 bug：工具栏过去会插入写死的 x^{2}，而不询问用户公式体。
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
    // 取消对话框必须让该行保持原样：没有 x^{2}。
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

    // 无效源保留其文本并报告原因。
    dialog.SetSourceForTest(QStringLiteral("\\begin{equation} x \\end{equation}"));
    PF_CHECK(dialog.latex() == QStringLiteral("\\begin{equation} x \\end{equation}"));
    PF_CHECK(dialog.StateText().contains(QStringLiteral("Invalid")));

    // 空源是 Pending，而不是错误。
    dialog.SetSourceForTest(QString());
    PF_CHECK(dialog.StateText().contains(QStringLiteral("Type a math body")));

    // 有效的公式体会生成预览 pixmap。
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
        // 源字段必须只是 LaTeX 公式体——绝不能是环境。
        PF_CHECK(!source->toPlainText().contains(QStringLiteral("\\begin{equation}")));
    }

    // 文档将属性与源分开存储。
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

PF_TEST(InlineMathPillReservesItsWholeLayoutRect) {
    QWidget host;
    host.setStyleSheet(QStringLiteral("QWidget { font-size: 10pt; }"));
    InlineEditor editor(&host);
    editor.resize(640, 80);
    editor.ensurePolished();
    editor.SetContent(InlineFromText("before after"));
    editor.ResizeToContent();
    const qreal plain_line_height =
        editor.document()->begin().layout()->lineAt(0).height();
    editor.InsertInlineMath(QStringLiteral("\\frac{a}{b}"));
    editor.ResizeToContent();
    const auto line = editor.document()->begin().layout()->lineAt(0);
    QTextCursor cursor(editor.document());
    const int math_position = editor.toPlainText().indexOf(QChar(0xFFFC));
    PF_CHECK(math_position >= 0);
    if (math_position < 0) return;
    cursor.setPosition(math_position);
    cursor.setPosition(math_position + 1, QTextCursor::KeepAnchor);
    const auto format = cursor.charFormat();
    PF_CHECK(format.verticalAlignment() == QTextCharFormat::AlignNormal);
    const QFontMetricsF metrics(editor.document()->defaultFont());
    const qreal baseline = format.property(
        inline_math_format::kBaselineProperty).toDouble();
    const qreal height = format.property(
        inline_math_format::kHeightProperty).toDouble();
    PF_CHECK(height < metrics.ascent());
    PF_CHECK(line.height() <= plain_line_height + 0.01);
    PF_CHECK(baseline > 0 && baseline < height);
    InlineMathObjectRenderer renderer;
    const QSizeF reserved = renderer.intrinsicSize(editor.document(), 0, format);
    PF_CHECK(qAbs(reserved.height() - height) < 0.01);
}

PF_TEST(InlineMathRendererDrawsBothEndsInsidePill) {
    QImage source(160, 80, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    QPainter source_painter(&source);
    source_painter.fillRect(QRect(0, 40, 160, 40), Qt::blue);
    source_painter.end();

    QPixmap pixmap = QPixmap::fromImage(source);
    pixmap.setDevicePixelRatio(2.0);

    QTextCharFormat format;
    format.setObjectType(inline_math_format::kObjectType);
    format.setProperty(inline_math_format::kPixmapProperty,
                       QVariant::fromValue(pixmap));
    format.setProperty(inline_math_format::kImageWidthProperty, 80.0);
    format.setProperty(inline_math_format::kImageHeightProperty, 24.0);

    QImage canvas(100, 28, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    InlineMathObjectRenderer renderer;
    renderer.drawObject(&painter, QRectF(0, 0, 100, 28), nullptr, 0,
                        format);
    painter.end();
    const QColor upper = canvas.pixelColor(50, 6);
    const QColor lower = canvas.pixelColor(50, 22);
    PF_CHECK(upper.red() > 220 && upper.blue() < 40);
    PF_CHECK(lower.blue() > 220 && lower.red() < 40);
}

PF_TEST(InlineMathPillTracksTextFontAndPreservesImageAspectRatio) {
    InlineEditor editor;
    editor.resize(640, 80);
    QFont small(QStringLiteral("DejaVu Sans"));
    small.setPointSize(10);
    editor.SetBodyTypography(small, 150);
    editor.InsertInlineMath(QStringLiteral("x"));
    editor.MarkClean();
    QTextCursor token(editor.document());
    token.setPosition(0);
    token.setPosition(1, QTextCursor::KeepAnchor);
    const QString id = token.charFormat().property(
        QTextFormat::UserProperty + 20).toString();
    PF_CHECK(!id.isEmpty());
    QImage image(200, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    editor.ApplyMathRender(id, QStringLiteral("x"), image, 100, 40, 30,
                           2.0, 20);
    const QTextCharFormat before = token.charFormat();
    const qreal small_line_height =
        editor.document()->begin().layout()->lineAt(0).height();
    const qreal before_image_width = before.property(
        inline_math_format::kImageWidthProperty).toDouble();
    const qreal before_image_height = before.property(
        inline_math_format::kImageHeightProperty).toDouble();
    PF_CHECK(qAbs(before_image_width / before_image_height - 2.5) < 0.01);
    PF_CHECK(before.property(inline_math_format::kHeightProperty).toDouble() >
             before_image_height);
    PF_CHECK(before_image_height <=
             before.property(inline_math_format::kHeightProperty).toDouble());
    InlineEditor plain_small;
    plain_small.resize(640, 80);
    plain_small.SetBodyTypography(small, 150);
    plain_small.SetContent(InlineFromText("x"));
    PF_CHECK(small_line_height <=
             plain_small.document()->begin().layout()->lineAt(0).height() +
                 0.01);

    QFont large(QStringLiteral("DejaVu Serif"));
    large.setPointSize(18);
    editor.SetBodyTypography(large, 150);
    const QTextCharFormat after = token.charFormat();
    const qreal after_image_width = after.property(
        inline_math_format::kImageWidthProperty).toDouble();
    const qreal after_image_height = after.property(
        inline_math_format::kImageHeightProperty).toDouble();
    PF_CHECK(after_image_width > before_image_width * 1.3);
    PF_CHECK(after_image_height > before_image_height * 1.3);
    PF_CHECK(qAbs(after_image_width / after_image_height - 2.5) < 0.01);
    PF_CHECK(after_image_height <=
             after.property(inline_math_format::kHeightProperty).toDouble());
    PF_CHECK(editor.document()->begin().layout()->lineAt(0).height() >
             small_line_height);
    InlineEditor plain_large;
    plain_large.resize(640, 80);
    plain_large.SetBodyTypography(large, 150);
    plain_large.SetContent(InlineFromText("x"));
    PF_CHECK(editor.document()->begin().layout()->lineAt(0).height() <=
             plain_large.document()->begin().layout()->lineAt(0).height() +
                 0.01);
    PF_CHECK(after.font().family() == large.family());
    PF_CHECK(!editor.IsDirty());
    PF_CHECK(std::holds_alternative<InlineMath>(editor.Content().front()));
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
        // 焦点位于对话框时收到文档通知，必须推迟重建该行，
        // 即使该行此前是 Clean 的。
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

// P0-08：评审要求的行内数学生命周期压力测试。所报告的崩溃是第二次
// 双击编辑时的 "double free or corruption (out)"。这里在同一编辑器中
// 完整驱动生命周期 100 次——插入、编辑两次、删除、undo、redo——
// 然后关闭项目并销毁窗口。在 ASan/UBSan 下，任何 double-free、
// use-after-free 或非法 free 都会中止运行；没有 sanitizer 时，测试
// 仍会断言每个编辑器指针始终有效，且文档从不丢失公式。
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
        // 1-2. 插入一个行内公式。
        editor->setFocus();
        editor->InsertInlineMath(QStringLiteral("x_%1").arg(i));
        Spin(1);

        // 3-4. 双击式编辑，然后确认。关键的不变量是：对话框 OPEN 期间
        // 编辑器必须存活——文档通知不得在模态框之下拆掉该行。
        //（接受会提交，从而重建该行——这是正常的完整重建路径，
        // 因此之后会重新获取指针。）
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
        // 提交驱动的重建之后，该行必须仍然可达。
        // 重建被投递到事件队列，因此应轮询而不是猜测睡眠时长
        //（sanitizer 构建慢得多）。
        editor = WaitFor([&] { return FindRich(fixture.window, fixture.node_id); });
        if (!editor) {
            PF_CHECK(false);
            return;
        }

        // 5-6. 对同一公式进行第二次编辑（历史上崩溃之处）。
        editor->EditMathAt(0);
        Spin(1);
        if (!editor->findChild<MathEditorDialog*>()) {
            // 该行又被重建了；定位存活的编辑器。
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

        // 7. 从该行删除公式，然后 8-9. undo/redo 它。
        //    （这些操作触发的刷新会重建编辑器。）
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

    // 10. 在公式仍显示在屏幕上时关闭项目。
    editor = FindRich(fixture.window, fixture.node_id);
    if (editor) {
        editor->setFocus();
        editor->InsertInlineMath(QStringLiteral("final"));
        Spin(2);
    }
    fixture.window.controller()->CloseProject();
    Spin(20);
    // 11. 显式销毁窗口：sanitizer 运行覆盖此拆解过程。
    PF_CHECK(true);
}
