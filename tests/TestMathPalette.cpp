// WP-02：数学面板的插入/包裹逻辑与缺包禁用。
#include "TestMain.hpp"

#include <QApplication>
#include <QPlainTextEdit>
#include <QToolButton>

#include "app/MathCommandCompleter.h"
#include "app/MathEditorDialog.h"
#include "app/MathPaletteWidget.h"

using namespace pf;
using namespace pf::gui;

PF_TEST(MathInsertionReplacesFirstPlaceholderWithSelection) {
    PF_CHECK(MakeMathInsertion("\\frac{$}{$}", QString()) == QStringLiteral("\\frac{}{}"));
    PF_CHECK(MakeMathInsertion("\\frac{$}{$}", QStringLiteral("x+1")) == QStringLiteral("\\frac{x+1}{}"));
    PF_CHECK(MakeMathInsertion("\\sqrt{$}", QStringLiteral("y")) == QStringLiteral("\\sqrt{y}"));
    PF_CHECK(MakeMathInsertion("\\alpha", QStringLiteral("ignored")) == QStringLiteral("\\alpha"));
}

PF_TEST(MathPaletteWrapsSelectionAndPositionsCursor) {
    PF_CHECK(qApp != nullptr);
    if (!qApp)
        return;
    QPlainTextEdit target;
    target.setPlainText(QStringLiteral("x+1"));
    target.selectAll();
    MathPaletteWidget palette;
    palette.SetTarget(&target);
    palette.SetCategory(MathCategory::Structure);
    auto* fraction = palette.findChild<QToolButton*>(QStringLiteral("math-command-frac"));
    PF_CHECK(fraction != nullptr);
    if (!fraction)
        return;
    fraction->click();
    PF_CHECK(target.toPlainText() == QStringLiteral("\\frac{x+1}{}"));
    // 光标位于第二个空参数内。
    PF_CHECK(target.textCursor().position() == QStringLiteral("\\frac{x+1}{").size());
}

PF_TEST(MathPaletteInsertsFontWithCursorInsideBracesAndDisablesMissingPackages) {
    PF_CHECK(qApp != nullptr);
    if (!qApp)
        return;
    QPlainTextEdit target;
    MathPaletteWidget palette;
    palette.SetTarget(&target);
    palette.SetAvailablePackages({"amssymb"});
    palette.SetCategory(MathCategory::Font);

    auto* mathbb = palette.findChild<QToolButton*>(QStringLiteral("math-command-mathbb"));
    auto* mathscr = palette.findChild<QToolButton*>(QStringLiteral("math-command-mathscr"));
    PF_CHECK(mathbb != nullptr);
    if (!mathbb || !mathscr)
        return;
    PF_CHECK(mathbb->isEnabled());
    PF_CHECK(!mathscr->isEnabled()); // 需要 mathrsfs
    mathbb->click();
    PF_CHECK(target.toPlainText() == QStringLiteral("\\mathbb{}"));
    PF_CHECK(target.textCursor().position() == QStringLiteral("\\mathbb{").size());

    // 提供 mathrsfs 后按钮恢复可用。
    palette.SetAvailablePackages({"amssymb", "mathrsfs"});
    auto* reenabled = palette.findChild<QToolButton*>(QStringLiteral("math-command-mathscr"));
    PF_CHECK(reenabled != nullptr && reenabled->isEnabled());
}

PF_TEST(MathEditorDialogExposesPaletteValidationAndPackageWarning) {
    PF_CHECK(qApp != nullptr);
    if (!qApp)
        return;
    MathEditorDialog dialog(QStringLiteral("\\mathscr{F}"));
    dialog.SetAvailablePackages({"amsmath", "amssymb"});
    PF_CHECK(dialog.Palette() != nullptr);
    dialog.SetSourceForTest(QStringLiteral("\\mathscr{F}"));
    PF_CHECK(dialog.Validation().required_packages == std::vector<std::string>{"mathrsfs"});
    PF_CHECK(dialog.StateText().contains(QStringLiteral("mathrsfs")));

    dialog.SetPaletteVisible(false);
    PF_CHECK(dialog.Palette()->isHidden());
    dialog.SetPaletteVisible(true);
    PF_CHECK(!dialog.Palette()->isHidden());

    dialog.SetAvailablePackages({"amsmath", "amssymb", "mathrsfs"});
    dialog.SetSourceForTest(QStringLiteral("\\mathbb{R}"));
    PF_CHECK(dialog.Validation().valid());
    PF_CHECK(dialog.StateText() == QStringLiteral("Valid"));
}

PF_TEST(MathCommandCompleterShowsAndAcceptsCommand) {
    PF_CHECK(qApp != nullptr);
    if (!qApp)
        return;
    QPlainTextEdit target;
    target.resize(400, 120);
    target.show();
    QApplication::setActiveWindow(&target);
    target.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    MathCommandCompleter completer;
    completer.Attach(&target);
    target.insertPlainText(QStringLiteral("\\mathb"));
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    PF_CHECK(completer.IsCompletionVisibleForTest());
    // 真实键盘路径：Tab 必须被补全接收，而不是把焦点移走。
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(&target, &tab);
    QCoreApplication::processEvents();
    PF_CHECK(target.toPlainText() == QStringLiteral("\\mathbb{}"));
    PF_CHECK(target.textCursor().position() == QStringLiteral("\\mathbb{").size());
    target.close();
}


PF_TEST(MathCommandCompleterPopupHasUsableGeometry) {
    PF_CHECK(qApp != nullptr);
    if (!qApp)
        return;
    QPlainTextEdit target;
    target.resize(400, 120);
    target.show();
    QApplication::setActiveWindow(&target);
    target.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    MathCommandCompleter completer;
    completer.Attach(&target);
    target.insertPlainText(QStringLiteral("\\sigma"));
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QWidget* popup = completer.PopupForTest();
    PF_CHECK(popup != nullptr && popup->isVisible());
    if (!popup)
        return;
    PF_CHECK(popup->width() >= 120);
    PF_CHECK(popup->height() >= 20);
    popup->close();
    target.close();
}
