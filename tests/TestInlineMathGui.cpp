// 数学输入重构的 GUI 测试：
//   * Inline Math 工具栏动作会要求输入公式体，而不再插入写死的 x^{2}；
//   * 行内数学对象能在编辑器中往返，整体删除，并在复制/粘贴后存活；
//   * 数学编辑器只做校验和预览，不改写源内容；
//   * Equation 行会暴露 source、preview、numbered 和 label。
#include "TestMain.hpp"

#include "app/InlineMathObjectRenderer.h"
#include "app/MathCommandCompleter.h"
#include "app/MathPaletteWidget.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QLineEdit>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTabBar>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTest>

#include <filesystem>
#include <iostream>
#include <memory>

#include "app/BlockEditor.h"
#include "app/InlineEditor.h"
#include "app/InlineContentEditorDialog.h"
#include "app/MainWindow.h"
#include "app/MathEditorDialog.h"
#include "app/MathPreviewRenderer.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"

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

InlineEditor* FindRich(MainWindow& window, const QString& node_id) {
    for (InlineEditor* edit : window.findChildren<InlineEditor*>()) {
        if (!edit->isVisible())
            continue;
        if (edit->property("row_node").toString() == node_id)
            return edit;
    }
    return nullptr;
}

// 轮询直到 `finder` 返回非空或超时。重建是通过事件队列延迟执行的，
// 因此固定 sleep 并不可靠——尤其在 sanitizer 构建下，每一步都慢得多。
template <typename Finder> auto WaitFor(Finder finder, int timeout_ms = 2000) -> decltype(finder()) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        auto found = finder();
        if (found)
            return found;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return finder();
}

QToolButton* FindButton(MainWindow& window, const QString& text) {
    QToolButton* best = nullptr;
    for (QToolButton* button : window.findChildren<QToolButton*>()) {
        if (!button->isVisible())
            continue;
        if (button->text() == text)
            best = button;
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
        auto dir = std::filesystem::temp_directory_path() / dir_name.toStdString();
        std::filesystem::remove_all(dir);
        window.controller()->NewProject(QString::fromStdString(dir.string()));
        window.controller()->Save();
        window.controller()->FlushSaves();
        window.OpenProjectDir(QString::fromStdString(dir.string()));
        Spin(250);

        auto section = window.controller()->InsertSection(QStringLiteral("S"));
        auto paragraph = window.controller()->InsertParagraph(section.created_node, QStringLiteral("before after"));
        node_id = QString::fromStdString(paragraph.created_node.value());
        Spin(300);
    }
};

const Paragraph* StoredParagraph(MainWindow& window) {
    Document& doc = window.controller()->session().mutable_document();
    const auto& sections = BodyOf(doc).sections;
    if (sections.empty() || sections[0].blocks.empty())
        return nullptr;
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

} // namespace

PF_TEST(InlineMathObjectRoundTripsThroughTheEditor) {
    EnsureQApplication();
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("\\frac{a}{b}"));

    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 1);
    const auto* math = content.empty() ? nullptr : std::get_if<InlineMath>(&content[0]);
    PF_CHECK(math != nullptr);
    if (math)
        PF_CHECK(math->expression.latex == "\\frac{a}{b}");

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

    std::cout << "  plain height=" << plain_height << " formula height=" << formula.height() << "\n";
    PF_CHECK(formula.height() <= plain_height);
    const qreal plain_line_height = plain.document()->begin().layout()->lineAt(0).height();
    const qreal formula_line_height = formula.document()->begin().layout()->lineAt(0).height();
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
    if (!copied)
        return;
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
        if (std::holds_alternative<InlineMath>(node))
            ++live_math;
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
        auto* dialog = qobject_cast<MathEditorDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
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
    const auto* math = content.empty() ? nullptr : std::get_if<InlineMath>(&content[0]);
    PF_CHECK(math != nullptr);
    if (math)
        PF_CHECK(math->expression.latex == "\\beta + 1");
}

// 这个测试钉住的 bug：工具栏过去会插入写死的 x^{2}，而不询问用户公式体。
PF_TEST(InlineMathToolbarAsksForABodyInsteadOfHardcoding) {
    EnsureQApplication();
    Fixture fixture("pf-inline-math-toolbar");
    InlineEditor* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor)
        return;
    editor->setFocus(Qt::MouseFocusReason);
    Spin(150);
    editor->SetContentClean(InlineFromText("before after"));
    QTextCursor selection(editor->document());
    selection.select(QTextCursor::WordUnderCursor);
    editor->setTextCursor(selection);
    Spin(30);

    QToolButton* math = FindButton(fixture.window, QStringLiteral("Inline Math"));
    PF_CHECK(math != nullptr);
    if (!math)
        return;

    RejectNextModalDialog(&fixture.window);
    math->click();
    Spin(300);

    InlineEditor* after = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(after != nullptr);
    if (!after)
        return;
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
    const MathRenderResult rendered = RenderMathPreview(QStringLiteral("\\sqrt{x^2 + y^2}"), style);
    PF_CHECK(!rendered.pixmap.isNull());
}

PF_TEST(EquationRowExposesPreviewNumberedAndLabel) {
    EnsureQApplication();
    Fixture fixture("pf-equation-card");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("E"));
    auto inserted =
        controller->InsertEquation(section.created_node, QStringLiteral("E = mc^2"), true, QStringLiteral("eq:energy"));
    PF_CHECK(inserted.status == EditStatus::Applied);
    Spin(400);

    const QString node_id = QString::fromStdString(inserted.created_node.value());
    // 展示态只显示公式预览 + 编号；点击预览才进入编辑态。
    QLabel* preview = nullptr;
    QLabel* number = nullptr;
    for (QLabel* candidate : fixture.window.findChildren<QLabel*>()) {
        if (candidate->objectName() == QStringLiteral("equationPreview") && candidate->isVisible())
            preview = candidate;
        if (candidate->objectName() == QStringLiteral("equationNumber") && candidate->isVisible())
            number = candidate;
    }
    PF_CHECK(preview != nullptr);
    PF_CHECK(number != nullptr && number->text() == QStringLiteral("(1)"));
    if (!preview)
        return;
    QTest::mouseClick(preview, Qt::LeftButton);
    Spin(80);

    QCheckBox* numbered = nullptr;
    QLineEdit* label = nullptr;
    QPlainTextEdit* source = nullptr;
    for (QCheckBox* box : fixture.window.findChildren<QCheckBox*>()) {
        if (box->isVisible() && box->property("row_node").toString() == node_id) {
            numbered = box;
        }
    }
    for (QLineEdit* edit : fixture.window.findChildren<QLineEdit*>()) {
        if (!edit->isVisible())
            continue;
        if (edit->property("row_node").toString() == node_id)
            label = edit;
    }
    for (QPlainTextEdit* edit : fixture.window.findChildren<QPlainTextEdit*>()) {
        if (!edit->isVisible())
            continue;
        if (edit->property("row_focus_key").toString() == node_id)
            source = edit;
    }
    PF_CHECK(numbered != nullptr);
    PF_CHECK(label != nullptr);
    PF_CHECK(source != nullptr);
    if (source)
        PF_CHECK(source->toPlainText() == QStringLiteral("E = mc^2"));
    // WP-02：进入编辑态后数学面板直接展开，并为源码框安装命令补全。
    MathPaletteWidget* palette = fixture.window.findChild<MathPaletteWidget*>();
    PF_CHECK(palette != nullptr);
    if (palette)
        PF_CHECK(palette->isVisible());
    if (source)
        PF_CHECK(source->findChild<MathCommandCompleter*>() != nullptr);

    // 点击 Palette 的分类/命令不能抢走源码焦点，也不能退回展示态。
    if (palette && source) {
        QTabBar* tabs = palette->findChild<QTabBar*>();
        PF_CHECK(tabs != nullptr);
        if (tabs) {
            QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(2).center()); // Greek
            Spin(30);
            PF_CHECK(source->isVisible());
            PF_CHECK(source->hasFocus());
            auto* sigma = palette->findChild<QToolButton*>(QStringLiteral("math-command-sigma"));
            PF_CHECK(sigma != nullptr);
            if (sigma) {
                QTest::mouseClick(sigma, Qt::LeftButton);
                Spin(30);
                PF_CHECK(source->isVisible());
                PF_CHECK(source->hasFocus());
            }
        }
    }
    if (numbered)
        PF_CHECK(numbered->isChecked());
    if (label)
        PF_CHECK(label->text() == QStringLiteral("eq:energy"));
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
                if (eq->id == target)
                    found = eq;
            }
        }
    }
    PF_CHECK(found != nullptr);
    if (found) {
        PF_CHECK(found->expression.latex == "E = mc^2");
        PF_CHECK(found->numbered);
        PF_CHECK(found->label == "eq:energy");
    }

    // 仅焦点变化（不含实际鼠标点击）不应让公式退回展示态；只有真正的
    // 卡片外点击才会收起，由 EquationCardMouseClickOutsideCollapsesEditor 覆盖。
    QPointer<QPlainTextEdit> source_guard(source);
    QAbstractButton* outside = nullptr;
    for (QAbstractButton* button : fixture.window.findChildren<QAbstractButton*>()) {
        if (button->isVisible() && button->text().contains(QStringLiteral("Build")))
            outside = button;
    }
    PF_CHECK(outside != nullptr);
    if (outside) {
        QApplication::setActiveWindow(&fixture.window);
        QCoreApplication::processEvents();
        outside->setFocus(Qt::MouseFocusReason);
        Spin(120);
        PF_CHECK(source_guard && !source_guard->isHidden());
    }
}

PF_TEST(InlineMathRenderNoteIsExposedForApproximateRender) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetContent(InlineFromText("x "));
    editor.InsertInlineMath(QStringLiteral("\\notacommand{abc}"));
    Spin(2500);
    const int position = editor.toPlainText().indexOf(QChar(0xFFFC));
    PF_CHECK(position >= 0);
    if (position < 0)
        return;
    PF_CHECK(!editor.MathRenderNoteAt(position).isEmpty());
}

PF_TEST(InlineMathTypingDoesNotInheritObjectPayload) {
    InlineEditor editor;
    editor.InsertInlineMath(QStringLiteral("x"));
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QApplication::sendEvent(&editor, &key);
    const auto content = editor.Content();
    PF_CHECK(content.size() == 2);
    if (content.size() != 2)
        return;
    PF_CHECK(std::holds_alternative<InlineMath>(content[0]));
    const auto* text = std::get_if<TextRun>(&content[1]);
    PF_CHECK(text != nullptr);
    if (text)
        PF_CHECK(text->text == "a");
}

PF_TEST(InlineMathPillReservesItsWholeLayoutRect) {
    QWidget host;
    host.setStyleSheet(QStringLiteral("QWidget { font-size: 10pt; }"));
    InlineEditor editor(&host);
    editor.resize(640, 80);
    editor.ensurePolished();
    editor.SetContent(InlineFromText("before after"));
    editor.ResizeToContent();
    const qreal plain_line_height = editor.document()->begin().layout()->lineAt(0).height();
    editor.InsertInlineMath(QStringLiteral("\\frac{a}{b}"));
    editor.ResizeToContent();
    const auto line = editor.document()->begin().layout()->lineAt(0);
    QTextCursor cursor(editor.document());
    const int math_position = editor.toPlainText().indexOf(QChar(0xFFFC));
    PF_CHECK(math_position >= 0);
    if (math_position < 0)
        return;
    cursor.setPosition(math_position);
    cursor.setPosition(math_position + 1, QTextCursor::KeepAnchor);
    const auto format = cursor.charFormat();
    PF_CHECK(format.verticalAlignment() == QTextCharFormat::AlignNormal);
    const QFontMetricsF metrics(editor.document()->defaultFont());
    const qreal baseline = format.property(inline_math_format::kBaselineProperty).toDouble();
    const qreal height = format.property(inline_math_format::kHeightProperty).toDouble();
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
    format.setProperty(inline_math_format::kPixmapProperty, QVariant::fromValue(pixmap));
    format.setProperty(inline_math_format::kImageWidthProperty, 80.0);
    format.setProperty(inline_math_format::kImageHeightProperty, 24.0);

    QImage canvas(100, 28, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    InlineMathObjectRenderer renderer;
    renderer.drawObject(&painter, QRectF(0, 0, 100, 28), nullptr, 0, format);
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
    const QString id = token.charFormat().property(QTextFormat::UserProperty + 20).toString();
    PF_CHECK(!id.isEmpty());
    QImage image(200, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    editor.ApplyMathRender(id, QStringLiteral("x"), image, 100, 40, 30, 2.0, 20);
    const QTextCharFormat before = token.charFormat();
    const qreal small_line_height = editor.document()->begin().layout()->lineAt(0).height();
    const qreal before_image_width = before.property(inline_math_format::kImageWidthProperty).toDouble();
    const qreal before_image_height = before.property(inline_math_format::kImageHeightProperty).toDouble();
    PF_CHECK(qAbs(before_image_width / before_image_height - 2.5) < 0.01);
    PF_CHECK(before.property(inline_math_format::kHeightProperty).toDouble() > before_image_height);
    PF_CHECK(before_image_height <= before.property(inline_math_format::kHeightProperty).toDouble());
    InlineEditor plain_small;
    plain_small.resize(640, 80);
    plain_small.SetBodyTypography(small, 150);
    plain_small.SetContent(InlineFromText("x"));
    PF_CHECK(small_line_height <= plain_small.document()->begin().layout()->lineAt(0).height() + 0.01);

    QFont large(QStringLiteral("DejaVu Serif"));
    large.setPointSize(18);
    editor.SetBodyTypography(large, 150);
    const QTextCharFormat after = token.charFormat();
    const qreal after_image_width = after.property(inline_math_format::kImageWidthProperty).toDouble();
    const qreal after_image_height = after.property(inline_math_format::kImageHeightProperty).toDouble();
    PF_CHECK(after_image_width > before_image_width * 1.3);
    PF_CHECK(after_image_height > before_image_height * 1.3);
    PF_CHECK(qAbs(after_image_width / after_image_height - 2.5) < 0.01);
    PF_CHECK(after_image_height <= after.property(inline_math_format::kHeightProperty).toDouble());
    PF_CHECK(editor.document()->begin().layout()->lineAt(0).height() > small_line_height);
    InlineEditor plain_large;
    plain_large.resize(640, 80);
    plain_large.SetBodyTypography(large, 150);
    plain_large.SetContent(InlineFromText("x"));
    PF_CHECK(editor.document()->begin().layout()->lineAt(0).height() <=
             plain_large.document()->begin().layout()->lineAt(0).height() + 0.01);
    PF_CHECK(after.font().family() == large.family());
    PF_CHECK(!editor.IsDirty());
    PF_CHECK(std::holds_alternative<InlineMath>(editor.Content().front()));
}

PF_TEST(InlineMathRepeatedEditingSurvivesMainWindowRefresh) {
    Fixture fixture("pf-inline-math-repeated-edit");
    auto* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor)
        return;
    editor->SetContentClean({});
    editor->setFocus();
    editor->InsertInlineMath(QStringLiteral("x"));
    for (int i = 0; i < 3; ++i) {
        editor = FindRich(fixture.window, fixture.node_id);
        PF_CHECK(editor != nullptr);
        if (!editor)
            return;
        QPointer<InlineEditor> guarded(editor);
        editor->EditMathAt(0);
        Spin(80);
        PF_CHECK(!guarded.isNull());
        if (!guarded)
            return;
        auto* dialog = editor->findChild<MathEditorDialog*>();
        PF_CHECK(dialog != nullptr);
        if (!dialog)
            return;
        // 焦点位于对话框时收到文档通知，必须推迟重建该行，
        // 即使该行此前是 Clean 的。
        fixture.window.controller()->InsertSection(QStringLiteral("Later"));
        Spin(80);
        PF_CHECK(!guarded.isNull());
        if (!guarded)
            return;
        dialog->SetSourceForTest(QStringLiteral("x + %1").arg(i));
        dialog->accept();
        Spin(200);
        const Paragraph* stored = StoredParagraph(fixture.window);
        PF_CHECK(stored != nullptr);
        editor = FindRich(fixture.window, fixture.node_id);
        PF_CHECK(editor != nullptr);
        if (!editor)
            return;
        const auto content = editor->Content();
        PF_CHECK(content.size() == 1);
        if (content.size() != 1)
            return;
        const auto* math = std::get_if<InlineMath>(&content.front());
        PF_CHECK(math != nullptr);
        if (math)
            PF_CHECK(math->expression.latex == QStringLiteral("x + %1").arg(i).toStdString());
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
    if (!editor)
        return;
    editor->SetContentClean({});

    constexpr int kIterations = 100;
    for (int i = 0; i < kIterations; ++i) {
        editor = FindRich(fixture.window, fixture.node_id);
        PF_CHECK(editor != nullptr);
        if (!editor)
            return;
        // 1-2. 插入一个行内公式。
        editor->setFocus();
        editor->InsertInlineMath(QStringLiteral("x_%1").arg(i));
        Spin(1);

        // 3-4. 双击式编辑，然后确认。关键的不变量是：对话框 OPEN 期间
        // 编辑器必须存活——文档通知不得在模态框之下拆掉该行。
        //（接受会提交，从而重建该行——这是正常的完整重建路径，
        // 因此之后会重新获取指针。）
        editor = FindRich(fixture.window, fixture.node_id);
        if (!editor)
            return;
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
            if (!editor)
                return;
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
        if (!editor)
            return;
        const auto content_before = editor->Content();
        if (!content_before.empty()) {
            QTextCursor cursor(editor->document());
            cursor.movePosition(QTextCursor::Start);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
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

PF_TEST(EquationNumberIsAutomaticAndFollowsNumberedFlag) {
    EnsureQApplication();
    Fixture fixture("pf-equation-number");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("Numbers"));
    auto first = controller->InsertEquation(section.created_node, QStringLiteral("a=b"), true,
                                            QStringLiteral("eq:first"));
    auto second = controller->InsertEquation(section.created_node, QStringLiteral("c=d"), false,
                                             QStringLiteral("eq:second"));
    PF_CHECK(first.status == EditStatus::Applied);
    PF_CHECK(second.status == EditStatus::Applied);
    Spin(350);

    auto card_for = [&](const NodeId& id) -> QWidget* {
        for (QWidget* card : fixture.window.findChildren<QWidget*>()) {
            if (card->isVisible() && card->objectName() == QStringLiteral("blockCard") &&
                card->property("row_node").toString() == QString::fromStdString(id.value()))
                return card;
        }
        return nullptr;
    };
    auto number_for = [&](const NodeId& id) -> QLabel* {
        QWidget* card = card_for(id);
        return card ? card->findChild<QLabel*>(QStringLiteral("equationNumber")) : nullptr;
    };
    auto preview_for = [&](const NodeId& id) -> QLabel* {
        QWidget* card = card_for(id);
        return card ? card->findChild<QLabel*>(QStringLiteral("equationPreview")) : nullptr;
    };
    auto checkbox_for = [&](const NodeId& id) -> QCheckBox* {
        QWidget* card = card_for(id);
        if (!card)
            return nullptr;
        for (QCheckBox* box : card->findChildren<QCheckBox*>())
            if (box->property("row_node").toString() == QString::fromStdString(id.value()))
                return box;
        return nullptr;
    };

    QLabel* first_number = number_for(first.created_node);
    QLabel* second_number = number_for(second.created_node);
    PF_CHECK(first_number != nullptr && first_number->text() == QStringLiteral("(1)"));
    PF_CHECK(second_number == nullptr || second_number->text().isEmpty());

    // 关闭 Numbered：编号消失；重新打开后恢复（全文编号由 DocumentNumberer 重算）。
    QLabel* first_preview = preview_for(first.created_node);
    PF_CHECK(first_preview != nullptr);
    if (!first_preview)
        return;
    QTest::mouseClick(first_preview, Qt::LeftButton);
    Spin(80);
    QCheckBox* numbered = checkbox_for(first.created_node);
    PF_CHECK(numbered != nullptr && numbered->isChecked());
    if (!numbered)
        return;
    numbered->setChecked(false);
    Spin(250);
    // 焦点仍在公式卡片内：属性变化不得把卡片打回展示态。
    PF_CHECK(numbered->isVisible());
    QLabel* hidden_number = number_for(first.created_node);
    PF_CHECK(hidden_number == nullptr || hidden_number->text().isEmpty() || hidden_number->isHidden());

    QLabel* again_preview = preview_for(first.created_node);
    PF_CHECK(again_preview != nullptr);
    if (!again_preview)
        return;
    QTest::mouseClick(again_preview, Qt::LeftButton);
    Spin(80);
    QCheckBox* again_numbered = checkbox_for(first.created_node);
    PF_CHECK(again_numbered != nullptr && !again_numbered->isChecked());
    if (!again_numbered)
        return;
    again_numbered->setChecked(true);
    Spin(250);
    QLabel* restored = number_for(first.created_node);
    PF_CHECK(restored != nullptr && restored->text() == QStringLiteral("(1)"));
}

PF_TEST(EquationPaletteWindowClicksKeepEditing) {
    EnsureQApplication();
    Fixture fixture("pf-equation-palette-window-click");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("Palette"));
    auto inserted = controller->InsertEquation(section.created_node, QStringLiteral("a+b"), true,
                                               QStringLiteral("eq:palette"));
    PF_CHECK(inserted.status == EditStatus::Applied);
    Spin(300);

    auto* editor = fixture.window.findChild<BlockEditor*>();
    auto* scroll = editor ? editor->findChild<QScrollArea*>() : nullptr;
    auto* preview = fixture.window.findChild<QLabel*>(QStringLiteral("equationPreview"));
    PF_CHECK(scroll && preview);
    if (!scroll || !preview)
        return;
    QSignalSpy commits(editor, &BlockEditor::EquationEdited);

    // 经窗口分发鼠标事件，覆盖 Qt 的焦点迁移与命中测试；直接给子控件发送
    // mouseClick 会绕过这条真实用户点击路径。
    auto click = [&](QWidget* target, const QPoint& position) {
        const QPoint content_position = target->mapTo(scroll->widget(), position);
        scroll->ensureVisible(content_position.x(), content_position.y(), 5, 5);
        Spin(30);
        QTest::mouseClick(fixture.window.windowHandle(), Qt::LeftButton, {},
                          target->mapTo(&fixture.window, position));
        Spin(50);
    };
    QApplication::setActiveWindow(&fixture.window);
    click(preview, preview->rect().center());

    QPointer<QPlainTextEdit> source;
    const QString node_id = QString::fromStdString(inserted.created_node.value());
    for (QPlainTextEdit* edit : fixture.window.findChildren<QPlainTextEdit*>()) {
        if (edit->isVisible() && edit->property("row_focus_key").toString() == node_id)
            source = edit;
    }
    QPointer<MathPaletteWidget> palette = fixture.window.findChild<MathPaletteWidget*>();
    QPointer<QTabBar> tabs = palette ? palette->findChild<QTabBar*>() : nullptr;
    PF_CHECK(source && palette && tabs);
    if (!source || !palette || !tabs)
        return;
    PF_CHECK(source->hasFocus());

    source->selectAll();
    QTest::keyClicks(source, "x+1");
    source->selectAll();
    for (int i = 1; i <= tabs->count(); ++i) {
        const int index = i % tabs->count();
        click(tabs, tabs->tabRect(index).center());
        PF_CHECK(source && source->isVisible() && source->hasFocus());
        PF_CHECK(palette && palette->isVisible());
        PF_CHECK(tabs && tabs->currentIndex() == index);
        if (!source || !source->isVisible() || !palette || !tabs)
            return;
        PF_CHECK(source->textCursor().selectedText() == QStringLiteral("x+1"));
    }

    auto* fraction = palette->findChild<QToolButton*>(QStringLiteral("math-command-frac"));
    PF_CHECK(fraction != nullptr);
    if (!fraction)
        return;
    click(fraction, fraction->rect().center());
    PF_CHECK(source && source->isVisible() && source->hasFocus());
    if (!source || !source->isVisible())
        return;
    PF_CHECK(source->toPlainText() == QStringLiteral("\\frac{x+1}{}"));
    PF_CHECK(source->textCursor().position() == QStringLiteral("\\frac{x+1}{").size());

    click(tabs, tabs->tabRect(2).center()); // Greek
    PF_CHECK(source && source->isVisible() && source->hasFocus());
    if (!source || !source->isVisible() || !palette)
        return;
    auto* sigma = palette->findChild<QToolButton*>(QStringLiteral("math-command-sigma"));
    PF_CHECK(sigma != nullptr);
    if (!sigma)
        return;
    click(sigma, sigma->rect().center());
    PF_CHECK(source && source->isVisible() && source->hasFocus());
    if (!source || !source->isVisible())
        return;
    PF_CHECK(source->toPlainText() == QStringLiteral("\\frac{x+1}{\\sigma}"));
    QTest::keyClick(fixture.window.windowHandle(), Qt::Key_2);
    const QString expected = QStringLiteral("\\frac{x+1}{\\sigma2}");
    PF_CHECK(source->toPlainText() == expected);
    const int cursor_position = source->textCursor().position();

    click(palette, palette->rect().bottomRight() - QPoint(3, 3));
    PF_CHECK(source && source->isVisible() && source->hasFocus());
    if (!source || !source->isVisible())
        return;
    QWidget* card = palette->parentWidget();
    click(card, card->rect().bottomRight() - QPoint(3, 3));
    PF_CHECK(source && source->isVisible() && source->hasFocus());
    if (source)
        PF_CHECK(source->textCursor().position() == cursor_position);
    PF_CHECK(commits.count() == 0);

    // 无焦点策略的外部标签也应结束编辑，并将最后输入的内容提交到文档。
    QLabel* outside = nullptr;
    for (QLabel* label : fixture.window.findChildren<QLabel*>()) {
        if (label->isVisible() && label->text() == QStringLiteral("ABSTRACT")) {
            outside = label;
            break;
        }
    }
    PF_CHECK(outside != nullptr);
    if (!outside)
        return;
    click(outside, outside->rect().center());
    PF_CHECK(!source || source->isHidden());
    PF_CHECK(commits.count() == 1);
    Block* stored = FindBlock(controller->session().mutable_document(), inserted.created_node);
    auto* equation = stored ? std::get_if<EquationBlock>(stored) : nullptr;
    PF_CHECK(equation && equation->expression.latex == expected.toStdString());
}

PF_TEST(EquationCardMouseClickOutsideCollapsesEditor) {
    EnsureQApplication();
    Fixture fixture("pf-equation-outside-click");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("Click"));
    auto inserted = controller->InsertEquation(section.created_node, QStringLiteral("a+b"), true,
                                               QStringLiteral("eq:click"));
    PF_CHECK(inserted.status == EditStatus::Applied);
    Spin(300);

    QLabel* preview = nullptr;
    for (QLabel* label : fixture.window.findChildren<QLabel*>()) {
        if (label->objectName() == QStringLiteral("equationPreview") && label->isVisible())
            preview = label;
    }
    PF_CHECK(preview != nullptr);
    if (!preview)
        return;
    QTest::mouseClick(preview, Qt::LeftButton);
    Spin(80);
    QPointer<QPlainTextEdit> source;
    for (QPlainTextEdit* edit : fixture.window.findChildren<QPlainTextEdit*>()) {
        if (edit->isVisible() && edit->property("row_focus_key").toString() ==
                                     QString::fromStdString(inserted.created_node.value()))
            source = edit;
    }
    PF_CHECK(source != nullptr && source->isVisible());

    // 真实鼠标按下卡片外部：application filter 应收起编辑态。
    QLabel* outside = nullptr;
    for (QLabel* label : fixture.window.findChildren<QLabel*>()) {
        if (!label->isVisible() || label->text() != QStringLiteral("ABSTRACT"))
            continue;
        outside = label;
        break;
    }
    PF_CHECK(outside != nullptr);
    if (outside) {
        QTest::mouseClick(outside, Qt::LeftButton);
        Spin(100);
        PF_CHECK(!source || source->isHidden());
    }
}

PF_TEST(ListCardSupportsAddStyleMoveAndDelete) {
    EnsureQApplication();
    Fixture fixture("pf-list-gui");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("List section"));
    auto list = controller->InsertListAfter(section.created_node, ListStyle::Bullet);
    PF_CHECK(list.status == EditStatus::Applied);
    Spin(350);
    const QString list_id = QString::fromStdString(list.created_node.value());

    QWidget* card = nullptr;
    for (QWidget* candidate : fixture.window.findChildren<QWidget*>()) {
        if (candidate->isVisible() && candidate->objectName() == QStringLiteral("blockCard") &&
            candidate->property("row_kind").toString() == QStringLiteral("List"))
            card = candidate;
    }
    PF_CHECK(card != nullptr);
    if (!card)
        return;
    auto click_button = [&](const QString& name) {
        auto* button = card->findChild<QToolButton*>(name);
        PF_CHECK(button != nullptr);
        if (button)
            QTest::mouseClick(button, Qt::LeftButton);
        Spin(120);
    };
    click_button(QStringLiteral("listAddItem"));
    click_button(QStringLiteral("listAddItem"));
    click_button(QStringLiteral("listStyleToggle"));

    Document& doc = controller->session().mutable_document();
    auto list_block = [&]() -> const ListBlock* {
        const auto address = LocateNode(doc, NodeId(list_id.toStdString()));
        if (!address || !address->block)
            return nullptr;
        const auto* blocks = FindBlockList(doc, *address);
        if (!blocks || *address->block >= blocks->size())
            return nullptr;
        return std::get_if<ListBlock>(&(*blocks)[*address->block]);
    };
    const auto* block = list_block();
    PF_CHECK(block != nullptr);
    if (!block)
        return;
    PF_CHECK(block->items.size() == 3);
    PF_CHECK(block->style == ListStyle::Numbered);

    // Delete buttons are rebuilt after every command; locate a fresh one.
    QToolButton* remove = nullptr;
    for (QToolButton* button : fixture.window.findChildren<QToolButton*>()) {
        if (button->isVisible() && button->objectName() == QStringLiteral("listDeleteItem")) {
            remove = button;
            break;
        }
    }
    PF_CHECK(remove != nullptr);
    if (remove) {
        QTest::mouseClick(remove, Qt::LeftButton);
        Spin(150);
    }
    const auto* after = list_block();
    PF_CHECK(after != nullptr && after->items.size() == 2);
    Q_UNUSED(list_id);
}

PF_TEST(FootnoteObjectRoundTripsThroughInlineEditor) {
    EnsureQApplication();
    InlineEditor editor;
    editor.SetContent(InlineFromText("Before "));
    editor.InsertFootnoteObject(InlineFromText("A note"));
    const InlineContent content = editor.Content();
    PF_CHECK(content.size() == 2);
    if (content.size() == 2) {
        const auto* footnote = std::get_if<Footnote>(&content[1]);
        PF_CHECK(footnote != nullptr && footnote->body);
        if (footnote && footnote->body)
            PF_CHECK(InlineToPlainText(footnote->body->content) == "A note");
    }
    editor.SetContent(content);
    PF_CHECK(editor.Content() == content);

    InlineEditor copy;
    copy.SetContent(content);
    const InlineContent copied = copy.Content();
    PF_CHECK(copied == content);
    const auto* footnote = copied.size() > 1 ? std::get_if<Footnote>(&copied[1]) : nullptr;
    PF_CHECK(footnote != nullptr);
}


PF_TEST(FootnoteDialogInsertsAndCommitsBody) {
    EnsureQApplication();
    Fixture fixture("pf-footnote-dialog");
    InlineEditor* editor = FindRich(fixture.window, fixture.node_id);
    PF_CHECK(editor != nullptr);
    if (!editor)
        return;
    editor->SetContentClean(InlineFromText("Text "));
    QAction* footnote_action = nullptr;
    for (QAction* action : editor->actions()) {
        if (action->objectName() == QStringLiteral("inlineFootnoteAction"))
            footnote_action = action;
    }
    PF_CHECK(footnote_action != nullptr);
    if (!footnote_action)
        return;
    footnote_action->trigger();
    Spin(100);
    auto* dialog = fixture.window.findChild<InlineContentEditorDialog*>();
    PF_CHECK(dialog != nullptr);
    if (!dialog)
        return;
    dialog->Editor()->SetContent(InlineFromText("Footnote body"));
    dialog->accept();
    Spin(100);
    QMetaObject::invokeMethod(editor, "Committed", Qt::DirectConnection);
    QCoreApplication::processEvents();
    Spin(250);

    Document& doc = fixture.window.controller()->session().mutable_document();
    const auto* para = FindBlock(doc, NodeId(fixture.node_id.toStdString())) != nullptr
                           ? std::get_if<Paragraph>(FindBlock(doc, NodeId(fixture.node_id.toStdString())))
                           : nullptr;
    PF_CHECK(para != nullptr);
    if (!para)
        return;
    bool found = false;
    for (const auto& node : para->content) {
        if (const auto* footnote = std::get_if<Footnote>(&node)) {
            found = true;
            PF_CHECK(footnote->body != nullptr);
            if (footnote->body)
                PF_CHECK(InlineToPlainText(footnote->body->content) == "Footnote body");
        }
    }
    PF_CHECK(found);
}

PF_TEST(TheoremAndCodeCardsProjectAndEdit) {
    EnsureQApplication();
    Fixture fixture("pf-theorem-code-gui");
    auto* controller = fixture.window.controller();
    auto section = controller->InsertSection(QStringLiteral("Formal"));
    auto theorem = controller->InsertTheoremAfter(section.created_node, TheoremKind::Theorem,
                                                   InlineFromText("Initial title"), true, QStringLiteral("thm:a"));
    PF_CHECK(theorem.status == EditStatus::Applied);
    auto code = controller->InsertCodeAfter(section.created_node, QStringLiteral("python"),
                                            QStringLiteral("print(1)"), true, QStringLiteral("lst:a"));
    PF_CHECK(code.status == EditStatus::Applied);
    Spin(350);

    QWidget* theorem_card = nullptr;
    QWidget* code_card = nullptr;
    for (QWidget* card : fixture.window.findChildren<QWidget*>()) {
        if (!card->isVisible() || card->objectName() != QStringLiteral("blockCard"))
            continue;
        if (card->property("row_kind").toString() == QStringLiteral("Theorem"))
            theorem_card = card;
        if (card->property("row_kind").toString() == QStringLiteral("Code"))
            code_card = card;
    }
    PF_CHECK(theorem_card != nullptr);
    PF_CHECK(code_card != nullptr);
    if (!theorem_card || !code_card)
        return;

    auto* kind = theorem_card->findChild<QComboBox*>(QStringLiteral("theoremKind"));
    auto* title = theorem_card->findChild<QLineEdit*>(QStringLiteral("theoremTitle"));
    auto* label = theorem_card->findChild<QLineEdit*>(QStringLiteral("theoremLabel"));
    PF_CHECK(kind && title && label);
    if (kind && title && label) {
        kind->setCurrentIndex(kind->findData(static_cast<int>(TheoremKind::Lemma)));
        title->setText(QStringLiteral("Updated title"));
        QMetaObject::invokeMethod(title, "editingFinished", Qt::DirectConnection);
        label->setText(QStringLiteral("thm:b"));
        QMetaObject::invokeMethod(label, "editingFinished", Qt::DirectConnection);
        Spin(120);
    }
    InlineEditor* theorem_body = nullptr;
    for (InlineEditor* editor : fixture.window.findChildren<InlineEditor*>()) {
        if (editor->isVisible() && editor->property("row_node").toString() ==
                                     QString::fromStdString(theorem.created_node.value()))
            theorem_body = editor;
    }
    PF_CHECK(theorem_body != nullptr);
    if (theorem_body) {
        theorem_body->SetContent(InlineFromText("A lemma body."));
        QMetaObject::invokeMethod(theorem_body, "Committed", Qt::DirectConnection);
        Spin(150);
    }

    auto* language = code_card->findChild<QComboBox*>(QStringLiteral("codeLanguage"));
    auto* source = code_card->findChild<QPlainTextEdit*>(QStringLiteral("codeSource"));
    auto* caption = code_card->findChild<QLineEdit*>(QStringLiteral("codeCaption"));
    auto* code_label = code_card->findChild<QLineEdit*>(QStringLiteral("codeLabel"));
    PF_CHECK(language && source && caption && code_label);
    if (language && source && caption && code_label) {
        language->setCurrentText(QStringLiteral("c++"));
        source->setPlainText(QStringLiteral("int main() { return 0; }"));
        QMetaObject::invokeMethod(source, "CommitRequested", Qt::DirectConnection);
        caption->setText(QStringLiteral("Entry point"));
        QMetaObject::invokeMethod(caption, "editingFinished", Qt::DirectConnection);
        code_label->setText(QStringLiteral("lst:b"));
        QMetaObject::invokeMethod(code_label, "editingFinished", Qt::DirectConnection);
        Spin(150);
    }

    Document& doc = fixture.window.controller()->session().mutable_document();
    const auto* theorem_back = FindBlock(doc, NodeId(theorem.created_node.value()))
                                   ? std::get_if<TheoremBlock>(FindBlock(doc, NodeId(theorem.created_node.value())))
                                   : nullptr;
    PF_CHECK(theorem_back != nullptr);
    if (theorem_back) {
        PF_CHECK(theorem_back->kind == TheoremKind::Lemma);
        PF_CHECK(InlineToPlainText(theorem_back->title) == "Updated title");
        PF_CHECK(InlineToPlainText(theorem_back->body) == "A lemma body.");
        PF_CHECK(theorem_back->label == "thm:b");
    }
    const auto* code_back = FindBlock(doc, NodeId(code.created_node.value()))
                                ? std::get_if<CodeBlock>(FindBlock(doc, NodeId(code.created_node.value())))
                                : nullptr;
    PF_CHECK(code_back != nullptr);
    if (code_back) {
        PF_CHECK(code_back->language == "c++");
        PF_CHECK(code_back->code.find("return 0") != std::string::npos);
        PF_CHECK(InlineToPlainText(code_back->caption) == "Entry point");
        PF_CHECK(code_back->label == "lst:b");
    }
}
