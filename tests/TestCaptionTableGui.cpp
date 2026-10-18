#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLineEdit>
#include <QPointer>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include "app/BlockEditor.h"
#include "app/InlineContentEditorDialog.h"
#include "app/InlineEditor.h"
#include "app/InsertTableDialog.h"
#include "app/MainWindow.h"
#include "app/MathEditorDialog.h"
#include "app/PopupList.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"

using namespace pf;
using namespace pf::gui;

namespace {

void Spin(int ms = 60) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(2);
    }
}

InlineContent Rich(const NodeId& target) {
    return {TextRun{"Loss ", static_cast<std::uint8_t>(TextMark::Strong)},
            InlineMath{{"\\mathcal{L}"}},
            TextRun{" from ", static_cast<std::uint8_t>(TextMark::Emphasis)},
            Citation{{"smith2020"}, CitationMode::Narrative},
            TextRun{"; see ", 0},
            CrossReference{target}};
}

struct Fixture {
    test::ScopedTempDir directory{"pf-wp04-gui"};
    MainWindow window;
    NodeId section;
    NodeId table_id;

    Fixture() {
        window.resize(1400, 900);
        window.show();
        auto* controller = window.controller();
        PF_CHECK(
            controller->NewProject(QString::fromStdString(directory.string())));
        controller->Save();
        controller->FlushSaves();
        PF_CHECK(
            window.OpenProjectDir(QString::fromStdString(directory.string())));
        section =
            controller->InsertSection(QStringLiteral("Results")).created_node;
        table_id =
            controller->InsertTableAfter(section, 2, 2, true).created_node;
        QApplication::setActiveWindow(&window);
        Spin(100);
    }

    BlockEditor* Editor() {
        return window.findChild<BlockEditor*>();
    }
    Table* Stored() {
        Block* block = FindBlock(
            window.controller()->session().mutable_document(), table_id);
        return block ? std::get_if<Table>(block) : nullptr;
    }
    QWidget* Card(const NodeId& id) {
        for (QWidget* card :
             window.findChildren<QWidget*>(QStringLiteral("blockCard"))) {
            if (card->isVisible() && card->property("row_node").toString() ==
                                         QString::fromStdString(id.value()))
                return card;
        }
        return nullptr;
    }
    QTableWidget* Grid() {
        QWidget* card = Card(table_id);
        return card
                   ? card->findChild<QTableWidget*>(QStringLiteral("tableGrid"))
                   : nullptr;
    }
    InlineEditor* Caption(const NodeId& id) {
        QWidget* card = Card(id);
        return card ? card->findChild<InlineEditor*>(
                          QStringLiteral("captionEditor"))
                    : nullptr;
    }
    QPoint Position(QWidget* target, QPoint point) {
        auto* scroll = Editor()->findChild<QScrollArea*>();
        const QPoint content = target->mapTo(scroll->widget(), point);
        scroll->ensureVisible(content.x(), content.y(), 5, 5);
        Spin(20);
        return target->mapTo(&window, point);
    }
    void Click(QWidget* target) {
        const QPoint point = Position(target, target->rect().center());
        QTest::mouseClick(window.windowHandle(), Qt::LeftButton, {}, point);
        Spin();
    }
    InlineContentEditorDialog* OpenCell(int row = 0, int column = 0) {
        auto* grid = Grid();
        const QPoint point =
            Position(grid->viewport(),
                     grid->visualItemRect(grid->item(row, column)).center());
        QTest::mouseClick(window.windowHandle(), Qt::LeftButton, {}, point);
        QTest::mouseDClick(window.windowHandle(), Qt::LeftButton, {}, point);
        Spin();
        return qobject_cast<InlineContentEditorDialog*>(QApplication::activeModalWidget());
    }
};

} // namespace

PF_TEST(WP04CaptionCommitsKeepRichContentAndSurviveDeferredRefresh) {
    Fixture fixture;
    const InlineContent original = Rich(fixture.section);
    PF_CHECK(fixture.window.controller()
                 ->EditCaptionRich(fixture.table_id, original)
                 .status == EditStatus::Applied);
    Spin();
    QPointer<InlineEditor> caption = fixture.Caption(fixture.table_id);
    PF_CHECK(caption && caption->Content() == original);
    if (!caption)
        return;
    QSignalSpy captions(fixture.Editor(), &BlockEditor::CaptionContentEdited);
    QSignalSpy paragraphs(fixture.Editor(),
                          &BlockEditor::ParagraphContentEdited);
    caption->setFocus();
    QTextCursor cursor = caption->textCursor();
    cursor.movePosition(QTextCursor::End);
    caption->setTextCursor(cursor);
    QTest::keyClicks(caption, " retained");
    const InlineContent expected = caption->Content();
    fixture.window.controller()->InsertSection(QStringLiteral("Later"));
    Spin();
    PF_CHECK(caption && caption->Content() == expected);
    fixture.Editor()->CommitFocused();
    Spin();
    PF_CHECK(fixture.Stored()->caption == expected);
    PF_CHECK(captions.count() == 1);
    PF_CHECK(paragraphs.count() == 0);

    fixture.window.controller()->Save();
    fixture.window.controller()->FlushSaves();
    PF_CHECK(fixture.window.controller()->session().persistence_state() ==
             PersistenceState::Clean);
    if (fixture.window.controller()->session().persistence_state() !=
        PersistenceState::Clean)
        return;
    PF_CHECK(fixture.window.OpenProjectDir(
        QString::fromStdString(fixture.directory.string())));
    Spin();
    PF_CHECK(fixture.Caption(fixture.table_id)->Content() == expected);
    fixture.Caption(fixture.table_id)->EditMathAt(5);
    Spin();
    auto* math =
        fixture.Caption(fixture.table_id)->findChild<MathEditorDialog*>();
    PF_CHECK(math != nullptr);
    if (math) {
        math->SetSourceForTest(QStringLiteral("\\sqrt{x}"));
        math->accept();
        Spin();
        const auto* expression =
            std::get_if<InlineMath>(&fixture.Stored()->caption[1]);
        PF_CHECK(expression && expression->expression.latex == "\\sqrt{x}");
    }
}

PF_TEST(WP04CellDoubleClickCancelAcceptAndUndoPreserveRichData) {
    Fixture fixture;
    const InlineContent original = Rich(fixture.section);
    fixture.window.controller()->SetTableCell(fixture.table_id, 0, 0, original);
    Spin();
    QSignalSpy cells(fixture.Editor(), &BlockEditor::TableCellEdited);
    const auto revision = fixture.window.controller()->current_revision();
    QPointer<InlineContentEditorDialog> dialog = fixture.OpenCell();
    PF_CHECK(dialog && dialog->isVisible());
    if (!dialog)
        return;
    PF_CHECK(QApplication::activeModalWidget() == dialog);
    PF_CHECK(dialog->content() == original);
    dialog->Editor()->SetContent(InlineFromText("discarded"));
    dialog->reject();
    Spin();
    PF_CHECK(fixture.Stored()->cells[0][0].content == original);
    PF_CHECK(fixture.window.controller()->current_revision() == revision);
    PF_CHECK(cells.count() == 0);

    dialog = fixture.OpenCell();
    PF_CHECK(dialog != nullptr);
    if (!dialog)
        return;
    InlineContent expected = original;
    expected.push_back(
        TextRun{" edited", static_cast<std::uint8_t>(TextMark::Emphasis)});
    dialog->Editor()->SetContent(expected);
    dialog->accept();
    Spin();
    PF_CHECK(cells.count() == 1);
    PF_CHECK(fixture.Stored()->cells[0][0].content == expected);
    fixture.window.controller()->Undo();
    Spin();
    PF_CHECK(fixture.Stored()->cells[0][0].content == original);
    fixture.window.controller()->Redo();
    Spin();
    PF_CHECK(fixture.Stored()->cells[0][0].content == expected);
}

PF_TEST(WP04CellDraftSurvivesDocumentChangesAndNestedCitationPicker) {
    Fixture fixture;
    fixture.window.controller()->ImportBibliographyText(
        QStringLiteral("@article{smith2020, author={Smith, Jane}, title={An "
                       "example}, year={2020}}"));
    Spin();
    QPointer<QTableWidget> grid = fixture.Grid();
    QPointer<InlineEditor> caption = fixture.Caption(fixture.table_id);
    QPointer<InlineContentEditorDialog> dialog = fixture.OpenCell();
    PF_CHECK(dialog != nullptr);
    if (!dialog)
        return;
    auto* draft = dialog->Editor();
    draft->insertPlainText(QStringLiteral("draft "));
    fixture.window.controller()->InsertSection(QStringLiteral("While editing"));
    Spin();
    PF_CHECK(grid && caption && dialog && dialog->isVisible());
    PF_CHECK(draft->toPlainText() == QStringLiteral("draft "));
    fixture.Editor()->ShowCitationPicker(draft);
    Spin();
    auto* popup = dialog->findChild<PopupList*>();
    PF_CHECK(popup && popup->isVisible());
    if (!popup) {
        dialog->reject();
        return;
    }
    auto* list = popup->ListForTest();
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        if (item->data(0, PopupList::PayloadRole).toString() ==
            QStringLiteral("smith2020"))
            item->setCheckState(0, Qt::Checked);
    }
    popup->ToggleForTest()->setChecked(true);
    QTest::keyClick(popup->SearchForTest(), Qt::Key_Return);
    Spin();
    PF_CHECK(draft->hasFocus());
    const InlineContent expected = draft->Content();
    PF_CHECK(expected.size() == 2);
    if (expected.size() == 2) {
        const auto* citation = std::get_if<Citation>(&expected[1]);
        PF_CHECK(citation && citation->mode == CitationMode::Narrative);
    }
    PF_CHECK(fixture.Stored()->cells[0][0].content.empty());
    dialog->accept();
    Spin();
    PF_CHECK(fixture.Stored()->cells[0][0].content == expected);
    PF_CHECK(fixture.window.controller()
                 ->session()
                 .state()
                 .document()
                 .body()
                 .sections.size() == 2);
    PF_CHECK(fixture.window.controller()->CitationResolver()->FormatPill(
                 {"smith2020"}) == "[1]");
}

PF_TEST(WP04ClosingCellAlsoDestroysItsNestedMathEditor) {
    Fixture fixture;
    QPointer<InlineContentEditorDialog> dialog = fixture.OpenCell();
    PF_CHECK(dialog != nullptr);
    if (!dialog)
        return;
    dialog->Editor()->InsertInlineMath(QStringLiteral("x"));
    dialog->Editor()->EditMathAt(0);
    Spin();
    QPointer<MathEditorDialog> math = dialog->findChild<MathEditorDialog*>();
    PF_CHECK(math && math->isVisible());
    dialog->reject();
    Spin();
    PF_CHECK(!dialog && !math);
    PF_CHECK(fixture.Stored()->cells[0][0].content.empty());
    PF_CHECK(!fixture.Editor()->HasUncommittedFocus());
}

PF_TEST(WP04TableToolbarEditsCommitCaptionAndKeepSelection) {
    Fixture fixture;
    auto* caption = fixture.Caption(fixture.table_id);
    caption->setFocus();
    caption->InsertInlineMath(QStringLiteral("x^2"));
    const InlineContent expected = caption->Content();
    auto* add_row =
        fixture.Card(fixture.table_id)
            ->findChild<QToolButton*>(QStringLiteral("tableAddRow"));
    fixture.Click(add_row);
    PF_CHECK(fixture.Stored()->RowCount() == 3);
    PF_CHECK(fixture.Stored()->caption == expected);
    auto* grid = fixture.Grid();
    grid->setCurrentCell(1, 1);
    grid->setFocus();
    fixture.Click(
        fixture.Card(fixture.table_id)
            ->findChild<QToolButton*>(QStringLiteral("tableAddColumn")));
    PF_CHECK(fixture.Stored()->ColumnCount() == 3);
    PF_CHECK(fixture.Grid()->currentRow() == 1 &&
             fixture.Grid()->currentColumn() == 1);
    auto* alignment =
        fixture.Card(fixture.table_id)
            ->findChild<QComboBox*>(QStringLiteral("tableColumnAlignment"));
    alignment->setFocus();
    alignment->setCurrentIndex(2);
    QMetaObject::invokeMethod(alignment, "activated", Qt::DirectConnection,
                              Q_ARG(int, 2));
    Spin();
    PF_CHECK(fixture.Stored()->columns[1].alignment == ColumnAlignment::Right);
    PF_CHECK(fixture.Grid()->item(0, 1)->textAlignment() & Qt::AlignRight);
    PF_CHECK(fixture.Grid()->currentColumn() == 1);
    fixture.Click(
        fixture.Card(fixture.table_id)
            ->findChild<QCheckBox*>(QStringLiteral("tableHeaderRow")));
    PF_CHECK(!fixture.Stored()->has_header_row);
    PF_CHECK(!fixture.Grid()->item(0, 0)->font().bold());
    fixture.Click(
        fixture.Card(fixture.table_id)
            ->findChild<QToolButton*>(QStringLiteral("tableDeleteRow")));
    fixture.Click(
        fixture.Card(fixture.table_id)
            ->findChild<QToolButton*>(QStringLiteral("tableDeleteColumn")));
    PF_CHECK(fixture.Stored()->RowCount() == 2 &&
             fixture.Stored()->ColumnCount() == 2);
    PF_CHECK(fixture.Stored()->IsRectangular());
}

PF_TEST(WP04TableInsertionDialogUsesDimensionsAndCancelIsUnchanged) {
    Fixture fixture;
    QTimer::singleShot(0, &fixture.window, [&]() {
        auto* dialog =
            qobject_cast<InsertTableDialog*>(QApplication::activeModalWidget());
        PF_CHECK(dialog != nullptr);
        if (!dialog)
            return;
        PF_CHECK(dialog->findChild<QSpinBox*>(QStringLiteral("tableRows"))
                     ->maximum() == static_cast<int>(Table::kMaxRows));
        PF_CHECK(dialog->findChild<QSpinBox*>(QStringLiteral("tableColumns"))
                     ->maximum() == static_cast<int>(Table::kMaxColumns));
        dialog->findChild<QSpinBox*>(QStringLiteral("tableRows"))->setValue(4);
        dialog->findChild<QSpinBox*>(QStringLiteral("tableColumns"))
            ->setValue(3);
        dialog->findChild<QCheckBox*>(QStringLiteral("tableHasHeader"))
            ->setChecked(false);
        dialog->accept();
    });
    fixture.Editor()->InsertBlockRequested(
        QStringLiteral("table"),
        QString::fromStdString(fixture.table_id.value()));
    Spin();
    const auto& blocks = fixture.window.controller()
                             ->session()
                             .state()
                             .document()
                             .body()
                             .sections[0]
                             .blocks;
    PF_CHECK(blocks.size() == 2);
    const auto* added =
        blocks.size() == 2 ? std::get_if<Table>(&blocks[1]) : nullptr;
    PF_CHECK(added && added->RowCount() == 4 && added->ColumnCount() == 3 &&
             !added->has_header_row);
    const auto revision = fixture.window.controller()->current_revision();
    QTimer::singleShot(0, &fixture.window, []() {
        if (auto* dialog =
                qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    fixture.Editor()->InsertBlockRequested(
        QStringLiteral("table"),
        QString::fromStdString(fixture.table_id.value()));
    PF_CHECK(fixture.window.controller()->current_revision() == revision);
    PF_CHECK(fixture.window.controller()
                 ->InsertTableAfter(fixture.table_id, 0, 2)
                 .status == EditStatus::Rejected);
    PF_CHECK(fixture.window.controller()
                 ->InsertTableAfter(fixture.table_id, Table::kMaxRows + 1, 2)
                 .status == EditStatus::Rejected);
}

PF_TEST(WP04FigureCaptionAndWidthUseRichUndoableCommands) {
    Fixture fixture;
    const auto path = fixture.directory / "figure.png";
    QImage image(40, 20, QImage::Format_RGB32);
    image.fill(Qt::blue);
    PF_CHECK(image.save(QString::fromStdString(path.string())));
    const auto result = fixture.window.controller()->InsertFigureAfter(
        fixture.table_id, QString::fromStdString(path.string()));
    PF_CHECK(result.status == EditStatus::Applied);
    Spin();
    const NodeId id = result.created_node;
    const InlineContent content = Rich(fixture.table_id);
    fixture.window.controller()->EditCaptionRich(id, content);
    Spin();
    QPointer<InlineEditor> caption = fixture.Caption(id);
    PF_CHECK(caption && caption->Content() == content);
    auto* width =
        fixture.Card(id)->findChild<QComboBox*>(QStringLiteral("figureWidth"));
    width->setCurrentIndex(1);
    Spin();
    PF_CHECK(caption && caption == fixture.Caption(id));
    const auto& stored = std::get<Figure>(*FindBlock(
        fixture.window.controller()->session().mutable_document(), id));
    PF_CHECK(stored.width == FigureWidth::Percent50 &&
             stored.caption == content);
    fixture.window.controller()->Undo();
    Spin();
    const auto& undone = std::get<Figure>(*FindBlock(
        fixture.window.controller()->session().mutable_document(), id));
    PF_CHECK(undone.width == FigureWidth::Percent100 &&
             undone.caption == content);
}
