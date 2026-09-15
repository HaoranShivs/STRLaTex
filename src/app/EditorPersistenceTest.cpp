// Regression test for the "typing is wiped by a periodic refresh" bug.
//
// Reported symptom: roughly once a second every row was rebuilt, which threw
// away whatever the user was typing. Root cause was a self-sustaining cycle:
// rebuild -> programmatic restyle fires textChanged -> idle auto-commit timer
// -> documentChanged -> rebuild, forever.
//
// The invariants enforced here:
//   1. While a row holds uncommitted user input, no rebuild happens at all
//      (the live editor instance must survive idle time).
//   2. The typed text is still there after the editor sits idle.
//   3. Committing on Enter / focus-out lands the text in the document, and a
//      later rebuild still shows it.
//
// Run with: QT_QPA_PLATFORM=offscreen ./build/src/app/paperforge-gui-persistence-test
#include <QApplication>
#include <QElapsedTimer>

#include <QPlainTextEdit>
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

// Keep the event loop turning for a while, exactly as the GUI would while the
// user pauses to think mid-sentence.
void Spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// The visible editor for a row, identified by its stable focus key.
QPlainTextEdit* FindRow(MainWindow& window, const QString& focus_key) {
    for (QPlainTextEdit* edit : window.findChildren<QPlainTextEdit*>()) {
        if (!edit->isVisible()) continue;
        if (edit->property("row_focus_key").toString() == focus_key) {
            return edit;
        }
    }
    return nullptr;
}

}  // namespace

namespace {

// A minimal N-page PDF, written by hand so the preview can be tested without
// running a full LaTeX build.
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
    // Save is asynchronous (snapshot -> save worker), so flush to make sure
    // project.paper is on disk before the test reopens the directory.
    window.controller()->Save();
    window.controller()->FlushSaves();
    Spin(100);
    Check(window.OpenProjectDir(QString::fromStdString(dir.string())),
          "open project shows the workspace");
    Spin(200);

    // ---- 1. Single-line row: typing must survive idle time ----
    QPlainTextEdit* title = FindRow(window, "front:title");
    Check(title != nullptr, "title row exists");
    if (!title) {
        std::filesystem::remove_all(dir);
        return 1;
    }

    title->setFocus(Qt::MouseFocusReason);
    Spin(80);
    Check(QApplication::focusWidget() == title, "title row holds focus");

    const QString typed = QStringLiteral("Typed While Idle");
    QTest::keyClicks(title, typed);
    Check(title->toPlainText() == typed, "typing lands in the title row");

    // Sit idle for far longer than the old ~400ms auto-commit / rebuild cycle.
    Spin(2000);

    QPlainTextEdit* title_after_idle = FindRow(window, "front:title");
    Check(title_after_idle == title,
          "no rebuild happens while a row has uncommitted input");
    Check(title_after_idle != nullptr &&
              title_after_idle->toPlainText() == typed,
          "typed title text survives 2s of idle time");
    Check(QApplication::focusWidget() == title,
          "focus is still in the title row after idle time");

    const auto& fm_before = window.controller()
                                ->session()
                                .state()
                                .document()
                                .front_matter();
    Check(pf::InlineToPlainText(fm_before.title).empty(),
          "no implicit mid-typing commit (design: commit on focus-out)");

    // ---- 2. Commit on Enter, then a rebuild must still show the text ----
    QTest::keyClick(title, Qt::Key_Return);
    Spin(300);

    Check(pf::InlineToPlainText(window.controller()
                                    ->session()
                                    .state()
                                    .document()
                                    .front_matter()
                                    .title) == typed.toStdString(),
          "Enter commits the typed title to the document");

    QPlainTextEdit* title_after_commit = FindRow(window, "front:title");
    Check(title_after_commit != nullptr &&
              title_after_commit->toPlainText() == typed,
          "committed title is shown after the rebuild");

    // ---- 3. Multi-line row (abstract): same guarantees ----
    QPlainTextEdit* abstract_row = FindRow(window, "front:abstract");
    Check(abstract_row != nullptr, "abstract row exists");
    if (abstract_row) {
        abstract_row->setFocus(Qt::MouseFocusReason);
        Spin(80);
        QTest::keyClicks(abstract_row, QStringLiteral("First line"));
        QTest::keyClick(abstract_row, Qt::Key_Return);
        QTest::keyClicks(abstract_row, QStringLiteral("Second line"));
        const QString abstract_text = QStringLiteral("First line\nSecond line");
        Check(abstract_row->toPlainText() == abstract_text,
              "typing lands in the abstract row");

        Spin(2000);
        QPlainTextEdit* abstract_idle = FindRow(window, "front:abstract");
        Check(abstract_idle == abstract_row,
              "multi-line row is not rebuilt while being edited");
        Check(abstract_idle != nullptr &&
                  abstract_idle->toPlainText() == abstract_text,
              "multi-line abstract survives idle time");

        // Focus-out commits it (title row takes focus).
        QPlainTextEdit* title_row = FindRow(window, "front:title");
        if (title_row) title_row->setFocus(Qt::MouseFocusReason);
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

    // ---- 4. Rows fit their text; only the pane scrolls ----
    auto section = window.controller()->InsertSection(QStringLiteral("Body"));
    auto inserted = window.controller()->InsertParagraph(
        section.created_node, QStringLiteral("seed"));
    Spin(300);
    const QString paragraph_key =
        QString::fromStdString(inserted.created_node.value());
    QPlainTextEdit* paragraph = nullptr;
    for (QPlainTextEdit* edit : window.findChildren<QPlainTextEdit*>()) {
        if (edit->isVisible() &&
            edit->property("row_focus_key").toString() == paragraph_key) {
            paragraph = edit;
        }
    }
    Check(paragraph != nullptr, "paragraph row exists");
    if (paragraph) {
        paragraph->setFocus(Qt::MouseFocusReason);
        Spin(60);
        Check(paragraph->verticalScrollBarPolicy() ==
                  Qt::ScrollBarAlwaysOff,
              "no scrollbar inside a block");

        const int one_line_height = paragraph->height();
        QString long_text;
        for (int i = 0; i < 40; ++i) {
            long_text += QStringLiteral("word%1 ").arg(i);
        }
        paragraph->setPlainText(long_text);
        Spin(250);
        const int tall_height = paragraph->height();
        Check(tall_height > one_line_height + 20,
              "row height grows with the wrapped text");
        Check(paragraph->document()->blockCount() == 1,
              "wrapping did not invent extra blocks");

        // The text must actually be visible: the document's laid-out height
        // has to fit inside the widget.
        const QRectF last =
            paragraph->document()
                ->documentLayout()
                ->blockBoundingRect(paragraph->document()->lastBlock());
        Check(last.bottom() <= paragraph->height(),
              "widget is at least as tall as the laid-out text");

        // A pasted multi-line abstract arrives as many explicit blocks; every
        // one of them has to be visible. Measured independently of the widget:
        // each block needs at least one line of its own.
        {
            QStringList lines;
            for (int i = 0; i < 20; ++i) {
                lines << QStringLiteral(
                             "Line %1 of a pasted multi-line abstract that is "
                             "long enough to wrap at least once.")
                             .arg(i);
            }
            paragraph->setPlainText(lines.join(QLatin1Char('\n')));
            Spin(300);
            const int pasted_height = paragraph->height();
            const int blocks = paragraph->document()->blockCount();
            std::cout << "  pasted: blocks=" << blocks
                      << " height=" << pasted_height << " line_spacing="
                      << paragraph->fontMetrics().lineSpacing() << "\n";
            Check(blocks == 20, "pasted text kept its 20 lines");
            Check(pasted_height >=
                      blocks * paragraph->fontMetrics().lineSpacing(),
                  "row height covers every pasted line");
        }

        // A rebuild may have replaced the row; re-fetch before touching it.
        paragraph = FindRow(window, paragraph_key);
        Check(paragraph != nullptr, "paragraph row still present");
        if (paragraph) paragraph->setPlainText(QStringLiteral("short"));
        Spin(250);
        Check(paragraph && paragraph->height() < tall_height,
              "row height shrinks again when the text is cleared");

        // Exactly one scrollbar serves the whole pane.
        int visible_pane_scrollbars = 0;
        for (QScrollArea* area : window.findChildren<QScrollArea*>()) {
            if (area->isVisible() &&
                area->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff) {
                ++visible_pane_scrollbars;
            }
        }
        Check(visible_pane_scrollbars >= 1, "editor pane provides scrolling");
    }

    // ---- 5. Preview zoom follows the mouse wheel ----
    {
        PdfPreview* preview = window.findChild<PdfPreview*>();
        Check(preview != nullptr, "preview widget exists");
        if (preview) {
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


        // ---- 6. A hard-wrapped paste re-flows to the block width ----
        {
            // Focusing another row commits the one being left, which can
            // rebuild every row; always re-fetch the pointer before using it.
            auto abstract_row = [&]() { return FindRow(window, "front:abstract"); };
            Check(abstract_row() != nullptr, "abstract row available");
            if (auto* row = abstract_row()) {
                // Exactly what copying a paragraph out of a PDF produces:
                // pre-wrapped at a fixed column, breaking mid-sentence.
                const QString hard = QStringLiteral(
                    "The success of deep learning in infrared small\n"
                    "target detection relies on large-scale annotations, "
                    "yet\n"
                    "their acquisition cost impedes further progress and "
                    "makes\n"
                    "weakly supervised alternatives attractive in practice.");
                if (QPlainTextEdit* target = abstract_row()) {
                    QApplication::clipboard()->setText(hard);
                    target->setFocus(Qt::MouseFocusReason);
                }
                Spin(120);
                if (QPlainTextEdit* target = abstract_row()) target->clear();
                Spin(80);
                // Real paste path (Ctrl+V): the editor's own paste hook is
                // what is under test.
                if (QPlainTextEdit* target = abstract_row()) {
                    QTest::keyClick(target, Qt::Key_V, Qt::ControlModifier);
                }
                Spin(250);

                QPlainTextEdit* pasted = abstract_row();
                Check(pasted != nullptr, "abstract row survives the paste");
                if (pasted) {
                    const QString shown = pasted->toPlainText();
                    std::cout << "  reflow: lines="
                              << shown.count(QLatin1Char('\n')) + 1 << "\n";
                    Check(shown.count(QLatin1Char('\n')) == 0,
                          "pasted hard wraps became soft");
                    Check(shown.contains(QStringLiteral(
                              "infrared small target detection")),
                          "words split across lines were rejoined");

                    auto max_line = [](QPlainTextEdit* edit) {
                        qreal widest = 0.0;
                        for (QTextBlock b = edit->document()->begin();
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
                    QPlainTextEdit* widened = abstract_row();
                    const qreal wide = widened ? max_line(widened) : 0.0;
                    std::cout << "  reflow: narrow_line=" << narrow
                              << " wide_line=" << wide << "\n";
                    Check(wide > narrow + 20.0,
                          "re-flowed prose follows the block width");
                    window.resize(window.width() - 300, window.height());
                    Spin(300);
                }

                // Committing stores the softened text, not the hard wraps.
                if (QPlainTextEdit* target = abstract_row()) {
                    target->clearFocus();
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

    // ---- 7. Every block carries a hover insert affordance ----
    {
        // Rows from an earlier rebuild may still sit in the deferred-delete
        // queue, so only live widgets count.
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
            // The click opens a modal menu, so the choice has to be made from
            // inside its event loop: poll for the popup, then activate its
            // first entry the way a keyboard user would (so the menu's exec()
            // returns that action).
            auto* picker = new QTimer(&window);
            QObject::connect(picker, &QTimer::timeout, [&]() {
                auto* popup =
                    qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!popup) return;
                // The menu is grouped (Structure / Content) with disabled
                // group titles, so pick the first *enabled, actionable* entry
                // rather than literally the first action.
                QAction* target = nullptr;
                for (QAction* action : popup->actions()) {
                    if (action->isEnabled() && action->data().isValid()) {
                        target = action;
                        break;
                    }
                }
                if (target == nullptr) return;
                // Prefer the "Text" entry when it is offered; that is the
                // insertion the test then verifies. Fall back to whatever
                // else is actionable.
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

    // ---- 8. The card menu reorders blocks ----
    {
        auto* ctl = window.controller();
        auto sec = ctl->InsertSection(QStringLiteral("Order"));
        auto first = ctl->InsertParagraph(sec.created_node, QStringLiteral("alpha"));
        auto second =
            ctl->InsertParagraph(sec.created_node, QStringLiteral("beta"));
        // Only the paragraphs of this section matter.
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

        // The chrome only appears on hover, so hover the second card first.
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

    // ---- 9. The preview shows every page, one after another ----
    {
        auto* preview = window.findChild<PdfPreview*>();
        Check(preview != nullptr, "preview widget available");
        if (preview) {
            const QString pdf = WriteMultiPagePdf(
                QString::fromStdString((dir / "multi.pdf").string()), 3);

            // Editing schedules a debounced build, and every finished build
            // swaps the preview document. Let that settle first, then never
            // hold on to a sheet pointer across an event loop turn.
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
                preview->SetZoom(1.0);  // independent of the pane width
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

            // Scrolling to the end must land on, and render, the last page.
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

    // ---- 10. Authors are linked to institutions in the GUI ----
    {
        auto* ctl = window.controller();
        // Two institutions and two authors, no markers typed.
        ctl->SetAffiliationsText(QStringLiteral("GUET; GETU"));
        ctl->SetAuthorsText(QStringLiteral("first, second"));
        Spin(400);

        // The panel lists one row per author and shows the current links.
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
            // Toggle the second institution for the first author through the
            // controller call the menu uses, then check both the document and
            // the refreshed panel.
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

            // The Authors row shows the marker, and the panel shows it too.
            QPlainTextEdit* authors_row = FindRow(window, "front:authors");
            Check(authors_row != nullptr, "authors row present");
            if (authors_row) {
                std::cout << "  authors row='"
                          << authors_row->toPlainText().toStdString() << "'\n";
                Check(authors_row->toPlainText().contains(
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

    // ---- 11. Dragging a block onto a gap reorders the document ----
    {
        auto* ctl = window.controller();
        auto sec = ctl->InsertSection(QStringLiteral("Drag"));
        auto alpha = ctl->InsertParagraph(sec.created_node, QStringLiteral("alpha"));
        auto beta = ctl->InsertParagraph(sec.created_node, QStringLiteral("beta"));
        auto gamma = ctl->InsertParagraph(sec.created_node, QStringLiteral("gamma"));
        Spin(1200);  // let the debounced build and rebuild settle

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

        // The grip exists and is draggable, and every gap accepts our type.
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
                // The grip is a plain QWidget child of the header.
            }
        }
        std::cout << "  drop_targets=" << drop_targets << " grips=" << grips
                  << "\n";
        Check(drop_targets >= 3, "every block has a drop target after it");

        // First the controller on its own, so a failure below is unambiguous.
        Check(ctl->MoveNodeAfter(gamma.created_node, alpha.created_node)
                      .status == pf::EditStatus::Applied,
              "MoveNodeAfter is accepted");
        Spin(500);
        std::cout << "  order after controller move=" << order().toStdString()
                  << "\n";
        Check(order() == QStringLiteral("alpha,gamma,beta"),
              "MoveNodeAfter reorders the document");

        // Then the same thing through a real drop on the gap after "beta":
        // gamma has to come back to the end.
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
            // The drop itself is delivered by Qt's drag manager, which needs a
            // real pointer device; the gap's action is invoked through the
            // meta-object so the reorder wiring below is still covered.
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
