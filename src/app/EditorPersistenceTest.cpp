// 针对「周期性刷新会清空正在输入的内容」这一缺陷的回归测试。
//
// 现象：大约每秒所有行都会被重建一次，用户正在输入的内容随之丢失。根因是一个自我
// 维持的循环：rebuild -> 程序化重设样式触发 textChanged -> 空闲自动提交定时器
// -> documentChanged -> rebuild，如此往复。
//
// 这里要守护的不变量：
//   1. 只要某一行存在未提交的用户输入，就完全不进行 rebuild
//      （存活的编辑器实例必须能熬过空闲时间）。
//   2. 编辑器空闲后，已输入的文本仍然存在。
//   3. 按 Enter / 失焦提交后，文本进入 document，之后的 rebuild 仍能显示它。
//
// 运行方式：QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-gui-persistence-test
#include <QApplication>
#include <QElapsedTimer>

#include <QPlainTextEdit>
#include <QTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QClipboard>
#include <QMenu>
#include <QFrame>
#include <QLabel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTextBlock>
#include <QToolButton>
#include <functional>
#include <fstream>
#include <QTimer>
#include <QTextDocument>
#include <QWheelEvent>
#include <QThread>
#include <filesystem>
#include <iostream>

#include "app/BlockEditor.h"
#include "build/BuildCoordinator.h"
#include "app/MainWindow.h"
#include "app/PdfPreview.h"
#include "app/ProjectController.h"
#include "document/InlineText.h"

using namespace pf::gui;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::cout << (ok ? "[ OK  ] " : "[FAIL] ") << what << "\n";
    if (!ok) ++failures;
}

// 等待直到没有 build 在运行、也没有排队中的应用事件。若某个 build 在测试中途完成，
// 会把新的 PDF 交给预览，而加载 PDF 会重新适配缩放；有关缩放的断言不能与此竞争。
void SettleBuilds(MainWindow& window, int timeout_ms = 30000) {
    auto& session = window.controller()->session();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
        if (pf::BuildPhase::Idle == session.build_phase() &&
            !session.HasPendingApplicationEvents()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (pf::BuildPhase::Idle == session.build_phase()) return;
        }
    }
}

// 让事件循环持续运转一段时间，正如用户在句中停顿思考时 GUI 所做的那样。
void Spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// 某一行的可见编辑器，由其稳定的 focus key 标识。
//
// rebuild 会隐藏旧行并创建新行；两者都会存活到延迟删除执行，因此只有可见的 widget
// 才算数，且以最后创建的那个为准。
//
// 行要么是 QPlainTextEdit（front matter、标题、公式），要么是 InlineEditor（Text
// 行）。两者除 QAbstractScrollArea 外没有共同的 widget 基类，因此查找返回的是测试
// 实际需要的通用编辑界面：文本读写、光标、document、焦点。
struct RowEditor {
    QWidget* widget = nullptr;
    QPlainTextEdit* plain = nullptr;  // 该行是 QPlainTextEdit 时设置
    QTextEdit* rich = nullptr;        // 该行是 InlineEditor 时设置

    QString toPlainText() const {
        return plain ? plain->toPlainText() : rich->toPlainText();
    }
    void SetText(const QString& text) {
        if (plain) plain->setPlainText(text);
        else rich->setPlainText(text);
    }
    QTextDocument* document() const {
        return plain ? plain->document() : rich->document();
    }
    int height() const { return widget ? widget->height() : 0; }
    void setFocus(Qt::FocusReason reason) { widget->setFocus(reason); }
    bool isVisible() const { return widget && widget->isVisible(); }
    QWidget* get() const { return widget; }
    void keyClick(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QTest::keyClick(widget, key, mods);
    }
    // Text 行专有。仅当该行是富文本编辑器时有效。
    QTextEdit* AsRich() const { return rich; }
    QPlainTextEdit* AsPlain() const { return plain; }
    int LineSpacing() const {
        return plain ? plain->fontMetrics().lineSpacing()
                     : rich->fontMetrics().lineSpacing();
    }
    int VerticalScrollBarPolicy() const {
        return plain ? static_cast<int>(plain->verticalScrollBarPolicy())
                     : static_cast<int>(rich->verticalScrollBarPolicy());
    }
    void SetPlainText(const QString& text) {
        if (plain) plain->setPlainText(text);
        else rich->setPlainText(text);
    }
    template <typename... Args>
    void keyClicks(Args... args) {
        QTest::keyClicks(plain ? static_cast<QWidget*>(plain)
                               : static_cast<QWidget*>(rich),
                         args...);
    }
    bool operator==(const RowEditor& other) const { return widget == other.widget; }
    bool operator!=(const RowEditor& other) const { return widget != other.widget; }
    bool operator==(std::nullptr_t) const { return widget == nullptr; }
    bool operator!=(std::nullptr_t) const { return widget != nullptr; }
    explicit operator bool() const { return widget != nullptr; }
};

RowEditor FindRow(MainWindow& window, const QString& focus_key) {
    RowEditor best;
    for (QWidget* widget : window.findChildren<QWidget*>()) {
        if (!widget->isVisible()) continue;
        if (widget->property("row_focus_key").toString() != focus_key) continue;
        if (auto* plain = qobject_cast<QPlainTextEdit*>(widget)) {
            best = RowEditor{widget, plain, nullptr};
        } else if (auto* rich = qobject_cast<QTextEdit*>(widget)) {
            best = RowEditor{widget, nullptr, rich};
        }
    }
    return best;
}

}  // namespace

namespace {

// 一个最小的 N 页 PDF，手工写出，以便无需运行完整的 LaTeX build 就能测试预览。
QString WriteMultiPagePdf(const QString& path, int page_count) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (int i = 0; i < page_count; ++i) {
        if (i) kids += " ";
        kids += std::to_string(3 + 2 * i) + " 0 R";
    }
    objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                      std::to_string(page_count) + " >>");
    for (int i = 0; i < page_count; ++i) {
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 "
                          "842] /Contents " +
                          std::to_string(4 + 2 * i) + " 0 R >>");
        objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    }
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
    out += "0000000000 65535 f \n";
    for (size_t offset : offsets) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%010zu 00000 n \n", offset);
        out += buffer;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
           "\n%%EOF\n";
    std::ofstream file(path.toStdString(), std::ios::binary);
    file << out;
    return path;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    auto dir = std::filesystem::temp_directory_path() / "pf-gui-persistence";
    std::filesystem::remove_all(dir);

    MainWindow window;
    window.resize(1400, 900);
    window.show();
    window.activateWindow();
    QApplication::setActiveWindow(&window);
    Spin(50);

    Check(window.controller()->NewProject(QString::fromStdString(dir.string())),
          "create project");
    // 保存是异步的（snapshot -> save worker），因此要 flush 以确保测试重新打开该
    // 目录之前 project.paper 已落盘。
    window.controller()->Save();
    window.controller()->FlushSaves();
    Spin(100);
    Check(window.OpenProjectDir(QString::fromStdString(dir.string())),
          "open project shows the workspace");
    Spin(200);

    // ---- 1. 单行输入行：输入必须能熬过空闲时间 ----
    RowEditor title = FindRow(window, "front:title");
    Check(title != nullptr, "title row exists");
    if (!title) {
        std::filesystem::remove_all(dir);
        return 1;
    }

    title.setFocus(Qt::MouseFocusReason);
    Spin(80);
    Check(QApplication::focusWidget() == title.get(), "title row holds focus");

    const QString typed = QStringLiteral("Typed While Idle");
    title.keyClicks(typed);
    Check(title.toPlainText() == typed, "typing lands in the title row");

    // 空闲等待的时间远超旧的约 400ms 自动提交 / rebuild 周期。
    Spin(2000);

    RowEditor title_after_idle = FindRow(window, "front:title");
    Check(title_after_idle == title,
          "no rebuild happens while a row has uncommitted input");
    Check(title_after_idle != nullptr &&
              title_after_idle.toPlainText() == typed,
          "typed title text survives 2s of idle time");
    Check(QApplication::focusWidget() == title.get(),
          "focus is still in the title row after idle time");

    const auto& fm_before = window.controller()
                                ->session()
                                .state()
                                .document()
                                .front_matter();
    Check(pf::InlineToPlainText(fm_before.title).empty(),
          "no implicit mid-typing commit (design: commit on focus-out)");

    // ---- 2. 按 Enter 提交，之后的 rebuild 仍须显示该文本 ----
    title.keyClick(Qt::Key_Return);
    Spin(300);

    Check(pf::InlineToPlainText(window.controller()
                                    ->session()
                                    .state()
                                    .document()
                                    .front_matter()
                                    .title) == typed.toStdString(),
          "Enter commits the typed title to the document");

    RowEditor title_after_commit = FindRow(window, "front:title");
    Check(title_after_commit != nullptr &&
              title_after_commit.toPlainText() == typed,
          "committed title is shown after the rebuild");

    // ---- 3. 多行输入行（abstract）：同样的保证 ----
    RowEditor abstract_row = FindRow(window, "front:abstract");
    Check(abstract_row != nullptr, "abstract row exists");
    if (abstract_row) {
        abstract_row.setFocus(Qt::MouseFocusReason);
        Spin(80);
        abstract_row.keyClicks(QStringLiteral("First line"));
        abstract_row.keyClick(Qt::Key_Return);
        abstract_row.keyClicks(QStringLiteral("Second line"));
        const QString abstract_text = QStringLiteral("First line\nSecond line");
        Check(abstract_row.toPlainText() == abstract_text,
              "typing lands in the abstract row");

        Spin(2000);
        RowEditor abstract_idle = FindRow(window, "front:abstract");
        Check(abstract_idle == abstract_row,
              "multi-line row is not rebuilt while being edited");
        Check(abstract_idle != nullptr &&
                  abstract_idle.toPlainText() == abstract_text,
              "multi-line abstract survives idle time");

        // 失焦时提交它（title 行取得焦点）。
        RowEditor title_row = FindRow(window, "front:title");
        if (title_row) title_row.setFocus(Qt::MouseFocusReason);
        Spin(400);
        const auto& fm = window.controller()
                             ->session()
                             .state()
                             .document()
                             .front_matter();
        Check(fm.abstract_text.has_value() &&
                  pf::InlineToPlainText(*fm.abstract_text) ==
                      abstract_text.toStdString(),
              "focus-out commits the abstract to the document");
    }

    // ---- 4. 行高度适应其文本；只有窗格滚动 ----
    auto section = window.controller()->InsertSection(QStringLiteral("Body"));
    auto inserted = window.controller()->InsertParagraph(
        section.created_node, QStringLiteral("seed"));
    Spin(300);
    const QString paragraph_key =
        QString::fromStdString(inserted.created_node.value());
    RowEditor paragraph;
    for (QWidget* widget : window.findChildren<QWidget*>()) {
        if (!widget->isVisible()) continue;
        if (widget->property("row_focus_key").toString() == paragraph_key) {
            paragraph = FindRow(window, paragraph_key);
        }
    }
    Check(paragraph != nullptr, "paragraph row exists");
    if (paragraph) {
        paragraph.setFocus(Qt::MouseFocusReason);
        Spin(60);
        Check(paragraph.VerticalScrollBarPolicy() ==
                  Qt::ScrollBarAlwaysOff,
              "no scrollbar inside a block");

        const int one_line_height = paragraph.height();
        QString long_text;
        for (int i = 0; i < 40; ++i) {
            long_text += QStringLiteral("word%1 ").arg(i);
        }
        paragraph.SetPlainText(long_text);
        Spin(250);
        const int tall_height = paragraph.height();
        Check(tall_height > one_line_height + 20,
              "row height grows with the wrapped text");
        Check(paragraph.document()->blockCount() == 1,
              "wrapping did not invent extra blocks");

        // 文本必须真正可见：document 排布后的高度必须能容纳在 widget 内。
        const QRectF last =
            paragraph.document()
                ->documentLayout()
                ->blockBoundingRect(paragraph.document()->lastBlock());
        Check(last.bottom() <= paragraph.height(),
              "widget is at least as tall as the laid-out text");

        // 粘贴进来的多行 abstract 会以多个显式 block 的形式到达；其中每一个都必须
        // 可见。此处独立于 widget 进行测量：每个 block 至少需要占一行。
        {
            QStringList lines;
            for (int i = 0; i < 20; ++i) {
                lines << QStringLiteral(
                             "Line %1 of a pasted multi-line abstract that is "
                             "long enough to wrap at least once.")
                             .arg(i);
            }
            paragraph.SetPlainText(lines.join(QLatin1Char('\n')));
            Spin(300);
            const int pasted_height = paragraph.height();
            const int blocks = paragraph.document()->blockCount();
            std::cout << "  pasted: blocks=" << blocks
                      << " height=" << pasted_height << " line_spacing="
                      << paragraph.LineSpacing() << "\n";
            Check(blocks == 20, "pasted text kept its 20 lines");
            Check(pasted_height >=
                      blocks * paragraph.LineSpacing(),
                  "row height covers every pasted line");
        }

        // rebuild 可能已替换了该行；操作它之前要重新获取。
        paragraph = FindRow(window, paragraph_key);
        Check(paragraph != nullptr, "paragraph row still present");
        if (paragraph) paragraph.SetPlainText(QStringLiteral("short"));
        Spin(250);
        Check(paragraph && paragraph.height() < tall_height,
              "row height shrinks again when the text is cleared");

        // 整个窗格恰好由一个滚动条提供服务。
        int visible_pane_scrollbars = 0;
        for (QScrollArea* area : window.findChildren<QScrollArea*>()) {
            if (area->isVisible() &&
                area->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff) {
                ++visible_pane_scrollbars;
            }
        }
        Check(visible_pane_scrollbars >= 1, "editor pane provides scrolling");
    }

    // ---- 5. 预览缩放跟随鼠标滚轮 ----
    {
        PdfPreview* preview = window.findChild<PdfPreview*>();
        Check(preview != nullptr, "preview widget exists");
        if (preview) {
            // 先让任何进行中的 build 结束：加载 PDF 会把缩放重置为适应宽度，否则
            // 它会插入到滚轮事件与断言之间。
            SettleBuilds(window);
            preview->SetZoom(1.0);
            const double before = preview->zoom();
            QWidget* viewport = preview->findChild<QScrollArea*>()
                                    ->viewport();
            const QPointF centre(viewport->width() / 2.0,
                                 viewport->height() / 2.0);
            QWheelEvent up(centre, viewport->mapToGlobal(centre.toPoint()),
                           QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                           Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &up);
            Spin(80);
            Check(preview->zoom() > before, "wheel up zooms the preview in");

            const double zoomed_in = preview->zoom();
            QWheelEvent down(centre, viewport->mapToGlobal(centre.toPoint()),
                             QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
                             Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &down);
            Spin(80);
            Check(preview->zoom() < zoomed_in,
                  "wheel down zooms the preview out");

            preview->SetZoom(100.0);
            Check(preview->zoom() <= PdfPreview::kMaxZoom,
                  "zoom is clamped to the maximum");
            preview->SetZoom(0.0001);
            Check(preview->zoom() >= PdfPreview::kMinZoom,
                  "zoom is clamped to the minimum");
        }
    }


        // ---- 6. 硬换行的粘贴内容会按块宽度重新排版 ----
        {
            // 聚焦另一行会提交被离开的那一行，而这可能重建所有行；使用指针前
            // 始终要重新获取。
            auto abstract_row_fn = [&]() { return FindRow(window, "front:abstract"); };
            Check(abstract_row_fn() != nullptr, "abstract row available");
            if (RowEditor row = abstract_row_fn()) {
                // 正是从 PDF 中复制段落所得到的结果：在固定列宽处预先换行，句子
                // 中途被断开。
                const QString hard = QStringLiteral(
                    "The success of deep learning in infrared small\n"
                    "target detection relies on large-scale annotations, "
                    "yet\n"
                    "their acquisition cost impedes further progress and "
                    "makes\n"
                    "weakly supervised alternatives attractive in practice.");
                if (RowEditor target = abstract_row_fn()) {
                    QApplication::clipboard()->setText(hard);
                    target.setFocus(Qt::MouseFocusReason);
                }
                Spin(120);
                if (RowEditor target = abstract_row_fn()) target.SetText(QString());
                Spin(80);
                // 真实的粘贴路径（Ctrl+V）：被测的是编辑器自身的粘贴钩子。
                if (RowEditor target = abstract_row_fn()) {
                    target.keyClick(Qt::Key_V, Qt::ControlModifier);
                }
                Spin(250);

                RowEditor pasted = abstract_row_fn();
                Check(pasted != nullptr, "abstract row survives the paste");
                if (pasted) {
                    const QString shown = pasted.toPlainText();
                    std::cout << "  reflow: lines="
                              << shown.count(QLatin1Char('\n')) + 1 << "\n";
                    Check(shown.count(QLatin1Char('\n')) == 0,
                          "pasted hard wraps became soft");
                    Check(shown.contains(QStringLiteral(
                              "infrared small target detection")),
                          "words split across lines were rejoined");

                    auto max_line = [](RowEditor edit) {
                        qreal widest = 0.0;
                        for (QTextBlock b = edit.document()->begin();
                             b.isValid(); b = b.next()) {
                            if (b.layout()) {
                                widest = qMax(
                                    widest,
                                    b.layout()->boundingRect().width());
                            }
                        }
                        return widest;
                    };
                    const qreal narrow = max_line(pasted);
                    window.resize(window.width() + 300, window.height());
                    Spin(400);
                    RowEditor widened = abstract_row_fn();
                    const qreal wide = widened ? max_line(widened) : 0.0;
                    std::cout << "  reflow: narrow_line=" << narrow
                              << " wide_line=" << wide << "\n";
                    Check(wide > narrow + 20.0,
                          "re-flowed prose follows the block width");
                    window.resize(window.width() - 300, window.height());
                    Spin(300);
                }

                // 提交时存储的是被软化的文本，而非硬换行。
                if (RowEditor target = abstract_row_fn()) {
                    target.widget->clearFocus();
                }
                Spin(250);
                const auto& front = window.controller()
                                        ->session()
                                        .state()
                                        .document()
                                        .front_matter();
                const std::string stored =
                    front.abstract_text
                        ? pf::InlineToPlainText(*front.abstract_text)
                        : std::string();
                Check(stored.find("infrared small target detection") !=
                          std::string::npos,
                      "committed text keeps the re-flowed sentence");
                Check(stored.find('\n') == std::string::npos,
                      "committed text has no stray hard wraps");
            }
        }

    // ---- 7. 每个 block 都带有悬停插入入口 ----
    {
        // 来自先前 rebuild 的行可能仍在延迟删除队列中，因此只统计存活的 widget。
        auto gaps = [&]() {
            std::vector<QWidget*> found;
            for (QWidget* w : window.findChildren<QWidget*>()) {
                if (w->property("gap_anchor").isValid() && w->isVisible()) {
                    found.push_back(w);
                }
            }
            return found;
        };
        auto paragraphs_in_body = [&]() {
            int count = 0;
            const auto& doc = window.controller()->session().state().document();
            for (const auto& section : doc.body().sections) {
                for (const auto& block : section.blocks) {
                    if (std::get_if<pf::Paragraph>(&block)) ++count;
                }
            }
            return count;
        };

        const auto before = gaps();
        std::cout << "  gaps=" << before.size() << "\n";
        Check(before.size() == 2, "one gap per body block");
        bool reserves_space = true;
        for (QWidget* gap : before) {
            if (gap->height() < 10) reserves_space = false;
            Check(!gap->property("gap_anchor").toString().isEmpty(),
                  "each gap knows which block it follows");
        }
        Check(reserves_space, "gap space is reserved even when not hovered");

        QWidget* target_gap = nullptr;
        for (QWidget* gap : before) {
            if (gap->property("gap_anchor").toString() == paragraph_key) {
                target_gap = gap;
            }
        }
        Check(target_gap != nullptr, "the paragraph has its own gap");
        auto* editor = window.findChild<BlockEditor*>();
        Check(editor != nullptr, "block editor reachable");
        if (target_gap && editor) {
            const int paragraphs_before = paragraphs_in_body();
            QString got_kind;
            QString got_anchor;
            QObject::connect(editor, &BlockEditor::InsertBlockRequested,
                             [&](QString kind, QString anchor) {
                                 got_kind = kind;
                                 got_anchor = anchor;
                             });
            // 点击会打开模态菜单，因此必须在其事件循环内部做出选择：轮询弹出
            // 菜单，然后像键盘用户那样激活它的第一个条目（使菜单的 exec() 返回
            // 该 action）。
            auto* picker = new QTimer(&window);
            QObject::connect(picker, &QTimer::timeout, [&]() {
                auto* popup =
                    qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!popup) return;
                // 菜单是分组的（Structure / Content），组标题被禁用，因此要选
                // 第一个*已启用且可操作*的条目，而不是字面上的第一个 action。
                QAction* target = nullptr;
                for (QAction* action : popup->actions()) {
                    if (action->isEnabled() && action->data().isValid()) {
                        target = action;
                        break;
                    }
                }
                if (target == nullptr) return;
                // 若提供「Text」条目则优先选它；那正是测试随后验证的插入操作。
                // 否则回退到任何其他可操作的条目。
                QAction* preferred = nullptr;
                for (QAction* action : popup->actions()) {
                    if (action->isEnabled() &&
                        action->data().toString() == QStringLiteral("text")) {
                        preferred = action;
                        break;
                    }
                }
                popup->setActiveAction(preferred ? preferred : target);
                QTest::keyClick(popup, Qt::Key_Return);
            });
            picker->start(40);
            QTest::mouseClick(target_gap, Qt::LeftButton);
            picker->stop();
            picker->deleteLater();
            Spin(400);

            Check(got_kind == QStringLiteral("text"),
                  "the gap menu offers block kinds");
            Check(got_anchor == paragraph_key,
                  "the gap inserts at its own position");
            Check(paragraphs_in_body() == paragraphs_before + 1,
                  "the gap button really inserted a block");
            Check(gaps().size() == before.size() + 1,
                  "the new block got its own gap");
        }
    }

    // ---- 8. 卡片菜单可重排 block ----
    {
        auto* ctl = window.controller();
        auto sec = ctl->InsertSection(QStringLiteral("Order"));
        auto first = ctl->InsertParagraph(sec.created_node, QStringLiteral("alpha"));
        auto second =
            ctl->InsertParagraph(sec.created_node, QStringLiteral("beta"));
        // 只有本节内的段落才算数。
        auto order = [&]() {
            QStringList texts;
            const auto& doc = window.controller()->session().state().document();
            for (const auto& s : doc.body().sections) {
                if (s.id != sec.created_node) continue;
                for (const auto& b : s.blocks) {
                    if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                        texts << QString::fromStdString(
                            pf::InlineToPlainText(p->content));
                    }
                }
            }
            return texts.join(",");
        };
        Spin(400);
        Check(order() == QStringLiteral("alpha,beta"),
              "paragraphs start in order");

        // 界面装饰只在悬停时出现，因此先悬停第二张卡片。
        QWidget* card = nullptr;
        for (QWidget* w : window.findChildren<QWidget*>()) {
            auto* frame = qobject_cast<QFrame*>(w);
            if (frame && frame->objectName() == QStringLiteral("blockCard") &&
                frame->property("row_node").toString() ==
                    QString::fromStdString(second.created_node.value())) {
                card = frame;
            }
        }
        Check(card != nullptr, "the paragraph card exists");
        if (card) {
            QEvent enter(QEvent::Enter);
            QApplication::sendEvent(card, &enter);
            Spin(150);
        }
        QToolButton* more = nullptr;
        for (QToolButton* button : window.findChildren<QToolButton*>()) {
            if (button->text() == QStringLiteral("⋯") && button->isVisible()) {
                more = button;
            }
        }
        Check(more != nullptr, "a card exposes its menu button");
        if (more) {
            std::function<QAction*(QMenu*, const QString&)> find_action =
                [&](QMenu* menu, const QString& text) -> QAction* {
                for (QAction* action : menu->actions()) {
                    if (action->text() == text) return action;
                    if (action->menu()) {
                        if (QAction* nested =
                                find_action(action->menu(), text)) {
                            return nested;
                        }
                    }
                }
                return nullptr;
            };
            auto* picker = new QTimer(&window);
            QObject::connect(picker, &QTimer::timeout, [&]() {
                auto* popup =
                    qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!popup) return;
                if (QAction* action = find_action(popup, QStringLiteral(
                                                              "Move Up"))) {
                    popup->setActiveAction(action);
                    QTest::keyClick(popup, Qt::Key_Return);
                }
            });
            picker->start(40);
            QTest::mouseClick(more, Qt::LeftButton);
            picker->stop();
            picker->deleteLater();
            Spin(400);
            std::cout << "  order after Move Up="
                      << order().toStdString() << "\n";
            Check(order() == QStringLiteral("beta,alpha"),
                  "Move Up reorders the document");
        }
    }

    // ---- 9. 预览依次显示每一页 ----
    {
        auto* preview = window.findChild<PdfPreview*>();
        Check(preview != nullptr, "preview widget available");
        if (preview) {
            const QString pdf = WriteMultiPagePdf(
                QString::fromStdString((dir / "multi.pdf").string()), 3);

            // 编辑会调度一次去抖的 build，而每次完成的 build 都会替换预览
            // document。先让它稳定，之后绝不要跨越事件循环轮次持有 sheet 指针。
            Spin(4000);
            auto sheets_now = [&]() {
                std::vector<QLabel*> found;
                auto* area = preview->findChild<QScrollArea*>();
                QWidget* column = area ? area->widget() : nullptr;
                if (!column || !column->layout()) return found;
                for (QLabel* label : column->findChildren<QLabel*>(
                         QStringLiteral("pdfPageSheet"))) {
                    if (column->layout()->indexOf(label) >= 0) {
                        found.push_back(label);
                    }
                }
                return found;
            };
            auto load = [&]() {
                preview->SetDocument(pdf);
                preview->SetZoom(1.0);  // 独立于窗格宽度
            };

            load();
            Spin(700);
            std::cout << "  pages=" << preview->pageCount() << "\n";
            Check(preview->pageCount() == 3, "all pages of the PDF are known");

            auto sheets = sheets_now();
            std::cout << "  sheets=" << sheets.size() << " page_h="
                      << (sheets.empty() ? -1 : sheets.front()->height())
                      << "\n";
            Check(sheets.size() == 3, "one sheet per page");
            if (sheets.size() == 3) {
                const int seam = sheets[1]->y() -
                                 (sheets[0]->y() + sheets[0]->height());
                const int seam2 = sheets[2]->y() -
                                  (sheets[1]->y() + sheets[1]->height());
                std::cout << "  seams=" << seam << "," << seam2 << "px\n";
                Check(seam >= 8 && seam <= 24 && seam2 >= 8 && seam2 <= 24,
                      "pages are stacked with a visible seam");
                Check(preview->visiblePage() == 1, "the view starts on page 1");
            }

            // 滚动到末尾必须落到最后一页并渲染它。
            load();
            Spin(700);
            preview->ScrollTo(0.0, 1.0);
            Spin(800);
            auto scrolled = sheets_now();
            const bool last_rendered =
                scrolled.size() == 3 &&
                !scrolled[2]->pixmap(Qt::ReturnByValue).isNull();
            std::cout << "  visible at bottom=" << preview->visiblePage()
                      << " sheets=" << scrolled.size()
                      << " last has raster=" << (last_rendered ? 1 : 0)
                      << "\n";
            Check(preview->visiblePage() == 3,
                  "scrolling reaches the last page");
            Check(last_rendered, "the last page is rendered on demand");
        }
    }

    // ---- 10. 在 GUI 中把作者关联到机构 ----
    {
        auto* ctl = window.controller();
        // 两个机构和两位作者，未输入任何标记。
        ctl->SetAffiliationsText(QStringLiteral("GUET; GETU"));
        ctl->SetAuthorsText(QStringLiteral("first, second"));
        Spin(400);

        // 面板为每位作者列出一行，并显示当前的关联关系。
        std::vector<QToolButton*> pickers;
        for (QToolButton* button : window.findChildren<QToolButton*>()) {
            if (button->objectName() ==
                    QStringLiteral("authorAffiliationPicker") &&
                button->isVisible()) {
                pickers.push_back(button);
            }
        }
        std::cout << "  pickers=" << pickers.size() << "\n";
        Check(pickers.size() == 2, "one institution picker per author");
        Check(!pickers.empty() && pickers[0]->text() == QStringLiteral("link…"),
              "an unlinked author says so");

        const auto affiliations =
            ctl->session().state().document().front_matter().affiliations;
        Check(affiliations.size() == 2, "two institutions available");
        if (pickers.size() == 2 && affiliations.size() == 2) {
            // 通过菜单所用的 controller 调用切换第一位作者的第二个机构，然后
            // 同时检查 document 和刷新后的面板。
            Check(ctl->SetAuthorAffiliation(0, affiliations[1].id, true)
                      .status == pf::EditStatus::Applied,
                  "link author 1 to institution 2");
            Spin(400);
            const auto& authors =
                ctl->session().state().document().front_matter().authors;
            Check(authors[0].affiliations.size() == 1 &&
                      authors[0].affiliations[0] == affiliations[1].id,
                  "the binding is stored on the author");
            Check(authors[1].affiliations.empty(),
                  "other authors are untouched");

            // Authors 行会显示该标记，面板也会显示它。
            RowEditor authors_row = FindRow(window, "front:authors");
            Check(authors_row != nullptr, "authors row present");
            if (authors_row) {
                std::cout << "  authors row='"
                          << authors_row.toPlainText().toStdString() << "'\n";
                Check(authors_row.toPlainText().contains(
                          QString::fromUtf8("\u00b2")),
                      "the row shows the institution number");
            }

            Check(ctl->SetAuthorAffiliation(0, affiliations[1].id, false)
                      .status == pf::EditStatus::Applied,
                  "unlink again");
            Spin(300);
            const auto& after =
                ctl->session().state().document().front_matter();
            Check(after.authors[0].affiliations.empty(),
                  "unlinking removes the binding");
        }
    }

    // ---- 11. 把 block 拖放到 gap 上可重排 document ----
    {
        auto* ctl = window.controller();
        auto sec = ctl->InsertSection(QStringLiteral("Drag"));
        auto alpha = ctl->InsertParagraph(sec.created_node, QStringLiteral("alpha"));
        auto beta = ctl->InsertParagraph(sec.created_node, QStringLiteral("beta"));
        auto gamma = ctl->InsertParagraph(sec.created_node, QStringLiteral("gamma"));
        Spin(1200);  // 让去抖的 build 与 rebuild 稳定下来

        auto order = [&]() {
            QStringList texts;
            const auto& doc = window.controller()->session().state().document();
            for (const auto& s : doc.body().sections) {
                if (s.id != sec.created_node) continue;
                for (const auto& b : s.blocks) {
                    if (const auto* p = std::get_if<pf::Paragraph>(&b)) {
                        texts << QString::fromStdString(
                            pf::InlineToPlainText(p->content));
                    }
                }
            }
            return texts.join(",");
        };
        Check(order() == QStringLiteral("alpha,beta,gamma"),
              "blocks start in order");

        // 拖拽手柄存在且可拖动，每个 gap 都接受我们的类型。
        int grips = 0;
        int drop_targets = 0;
        for (QWidget* w : window.findChildren<QWidget*>()) {
            if (w->property("gap_anchor").isValid() && w->isVisible()) {
                ++drop_targets;
                Check(w->acceptDrops(), "a gap accepts drops");
            }
        }
        for (QObject* child : window.findChildren<QObject*>()) {
            if (child->inherits("QWidget") &&
                child->property("row_node").toString() ==
                    QString::fromStdString(gamma.created_node.value())) {
                // 拖拽手柄是 header 的普通 QWidget 子对象。
            }
        }
        std::cout << "  drop_targets=" << drop_targets << " grips=" << grips
                  << "\n";
        Check(drop_targets >= 3, "every block has a drop target after it");

        // 先单独测试 controller，这样下面若失败则原因明确。
        Check(ctl->MoveNodeAfter(gamma.created_node, alpha.created_node)
                      .status == pf::EditStatus::Applied,
              "MoveNodeAfter is accepted");
        Spin(500);
        std::cout << "  order after controller move=" << order().toStdString()
                  << "\n";
        Check(order() == QStringLiteral("alpha,gamma,beta"),
              "MoveNodeAfter reorders the document");

        // 然后通过对「beta」之后的 gap 执行真实拖放来做同样的事：gamma 必须回到
        // 末尾。
        QWidget* target_gap = nullptr;
        for (QWidget* w : window.findChildren<QWidget*>()) {
            if (w->property("gap_anchor").toString() ==
                    QString::fromStdString(beta.created_node.value()) &&
                w->isVisible()) {
                target_gap = w;
            }
        }
        Check(target_gap != nullptr, "the gap after beta exists");
        if (target_gap) {
            // 拖放本身由 Qt 的拖拽管理器投递，它需要真实的指针设备；这里通过
            // meta-object 调用 gap 的 action，从而仍能覆盖下面的重排接线。
            const QString gamma_id =
                QString::fromStdString(gamma.created_node.value());
            const bool invoked = QMetaObject::invokeMethod(
                target_gap, "applyDrop", Q_ARG(QString, gamma_id));
            Check(invoked, "the gap accepts a dropped block");
            Spin(500);
            std::cout << "  order after drop=" << order().toStdString()
                      << "\n";
            Check(order() == QStringLiteral("alpha,beta,gamma"),
                  "the dragged block lands where it was dropped");
        }
    }

    std::filesystem::remove_all(dir);
    std::cout << (failures == 0 ? "[ DONE ] all checks passed\n"
                                : "[ DONE ] failures detected\n");
    return failures == 0 ? 0 : 1;
}
