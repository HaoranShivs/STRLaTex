#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QTimer>

#include <fstream>

#include "app/AddReferenceDialog.h"
#include "app/MainWindow.h"
#include "app/OutlinePanel.h"

using namespace pf;
using namespace pf::gui;

namespace {

const char* kCompleteEntry = R"(@article{new2026,
    title = {A {NASA} study, with a comma},
    author = {Jane Smith and Armand Brown},
    journal = {Test Journal},
    year = 2026,
    abstract = {Preserve this extra information.}
})";

QLineEdit* Field(AddReferenceDialog& dialog, const char* name) {
    return dialog.findChild<QLineEdit*>(QStringLiteral("bibtexField_") + name);
}

void ParseNow(AddReferenceDialog& dialog, const QString& source) {
    dialog.findChild<QPlainTextEdit*>("bibtexSource")->setPlainText(source);
    dialog.findChild<QPushButton*>("parseBibTeXButton")->click();
}

void AddNow(AddReferenceDialog& dialog) {
    QTest::mouseClick(dialog.findChild<QPushButton*>("addReferenceButton"),
                      Qt::LeftButton);
}

std::string ReadBib(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void AnswerIncomplete(AddReferenceDialog& dialog, bool allow, bool& seen) {
    QTimer::singleShot(0, &dialog, [&dialog, allow, &seen]() {
        auto* message =
            dialog.findChild<QMessageBox*>("incompleteReferenceConfirmation");
        PF_CHECK(message != nullptr);
        if (!message)
            return;
        seen = true;
        PF_CHECK(message->text().contains("Authors"));
        PF_CHECK(message->text().contains("Year"));
        PF_CHECK(message->text().contains("Journal"));
        const QString choice = allow ? QStringLiteral("Add Anyway")
                                     : QStringLiteral("Continue Editing");
        for (auto* button : message->buttons()) {
            if (button->text() == choice) {
                QTest::mouseClick(button, Qt::LeftButton);
                return;
            }
        }
        PF_CHECK(false);
        message->reject();
    });
    AddNow(dialog);
}

} // namespace

PF_TEST(ReferenceEntryAutoParsesEditableFieldsAndAddsConsecutively) {
    test::ScopedTempDir directory("pf-reference-gui-add");
    ProjectController controller;
    PF_CHECK(controller.NewProject(QString::fromStdString(directory.string())));
    const QString original = "% Preserve original source\n@article{old, "
                             "title={Existing reference}}\n";
    PF_CHECK(controller.ImportBibliographyText(original).status ==
             BibliographyImportResult::Status::Ok);
    AddReferenceDialog dialog(&controller);
    dialog.show();
    dialog.findChild<QPlainTextEdit*>("bibtexSource")
        ->setPlainText(kCompleteEntry);
    PF_CHECK(QTest::qWaitFor([&]() {
        return Field(dialog, "title")->text() == "A {NASA} study, with a comma";
    }));
    PF_CHECK(Field(dialog, "author")->text() == "Jane Smith and Armand Brown");
    PF_CHECK(Field(dialog, "year")->text() == "2026");
    PF_CHECK(Field(dialog, "journal")->text() == "Test Journal");
    PF_CHECK(Field(dialog, "doi")->text().isEmpty());
    auto* extra = dialog.findChild<QTableWidget*>("bibtexExtraFields");
    PF_CHECK(extra->rowCount() == 1);
    PF_CHECK(extra->item(0, 0)->text() == "abstract");
    extra->item(0, 1)->setText("Manually edited extra information.");
    Field(dialog, "title")->setText("Manually corrected title");
    Field(dialog, "doi")->setText("10.1234/manual");
    QTest::qWait(300);
    PF_CHECK(Field(dialog, "title")->text() == "Manually corrected title");
    AddNow(dialog);
    controller.CancelBuild();
    PF_CHECK(controller.session().bibliography().Entries().size() == 2);
    const auto* added = controller.session().bibliography().Find("new2026");
    PF_CHECK(added != nullptr);
    if (added) {
        PF_CHECK(added->title == "Manually corrected title");
        PF_CHECK(added->fields.at("doi") == "10.1234/manual");
        PF_CHECK(added->fields.at("abstract") ==
                 "Manually edited extra information.");
    }
    PF_CHECK(ReadBib(directory / "references.bib")
                 .starts_with(original.toStdString()));
    PF_CHECK(dialog.isVisible());
    PF_CHECK(dialog.findChild<QPlainTextEdit*>("bibtexSource")
                 ->toPlainText()
                 .isEmpty());
    PF_CHECK(
        dialog.findChild<QLineEdit*>("bibtexCitationKey")->text().isEmpty());
    ParseNow(dialog,
             QString::fromUtf8(kCompleteEntry).replace("new2026", "next2026"));
    AddNow(dialog);
    controller.CancelBuild();
    PF_CHECK(controller.session().bibliography().Entries().size() == 3);
    // Manual entry needs no pasted source; Chinese citation keys stay intact.
    dialog.findChild<QLineEdit*>("bibtexCitationKey")->setText("王2026");
    Field(dialog, "title")->setText("手工填写的文献");
    Field(dialog, "author")->setText("王小明");
    Field(dialog, "journal")->setText("测试期刊");
    Field(dialog, "year")->setText("2026");
    AddNow(dialog);
    controller.CancelBuild();
    PF_CHECK(controller.session().bibliography().Entries().size() == 4);
    controller.Save();
    controller.FlushSaves();
    dialog.reject();
    controller.CloseProject();
    PF_CHECK(
        controller.OpenProject(QString::fromStdString(directory.string())));
    PF_CHECK(controller.session().bibliography().Find("old") != nullptr);
    PF_CHECK(controller.session().bibliography().Find("new2026") != nullptr);
    PF_CHECK(controller.session().bibliography().Find("next2026") != nullptr);
    PF_CHECK(controller.session().bibliography().Find("王2026") != nullptr);
}

PF_TEST(ReferenceEntryIncompleteAddRequiresExplicitConfirmation) {
    test::ScopedTempDir directory("pf-reference-gui-incomplete");
    ProjectController controller;
    PF_CHECK(controller.NewProject(QString::fromStdString(directory.string())));
    AddReferenceDialog dialog(&controller);
    dialog.show();
    ParseNow(dialog, "@article{draft2026, title={Only a title}}");
    PF_CHECK(Field(dialog, "author")->text().isEmpty());
    PF_CHECK(Field(dialog, "journal")->text().isEmpty());
    PF_CHECK(Field(dialog, "year")->text().isEmpty());
    bool declined = false;
    AnswerIncomplete(dialog, false, declined);
    PF_CHECK(declined);
    PF_CHECK(controller.session().bibliography().Entries().empty());
    PF_CHECK(!std::filesystem::exists(directory / "references.bib"));
    PF_CHECK(Field(dialog, "title")->text() == "Only a title");
    bool confirmed = false;
    AnswerIncomplete(dialog, true, confirmed);
    controller.CancelBuild();
    PF_CHECK(confirmed);
    const auto* entry = controller.session().bibliography().Find("draft2026");
    PF_CHECK(entry != nullptr);
    if (entry) {
        PF_CHECK(entry->authors.empty() && entry->year.empty() &&
                 entry->venue.empty());
        PF_CHECK(entry->fields.count("author") == 0);
    }
    PF_CHECK(ReadBib(directory / "references.bib").find("Only a title") !=
             std::string::npos);
}

PF_TEST(
    ReferenceEntryRejectsInvalidMultipleAndDuplicateEntriesWithoutChangingFiles) {
    test::ScopedTempDir directory("pf-reference-gui-validation");
    ProjectController controller;
    PF_CHECK(controller.NewProject(QString::fromStdString(directory.string())));
    PF_CHECK(controller.ImportBibliographyText(kCompleteEntry).status ==
             BibliographyImportResult::Status::Ok);
    const auto original = ReadBib(directory / "references.bib");
    AddReferenceDialog dialog(&controller);
    dialog.show();
    auto* add = dialog.findChild<QPushButton*>("addReferenceButton");
    ParseNow(dialog, kCompleteEntry);
    AddNow(dialog);
    PF_CHECK(dialog.findChild<QLabel*>("referenceEntryStatus")
                 ->text()
                 .contains("already exists"));
    PF_CHECK(Field(dialog, "title")->text() == "A {NASA} study, with a comma");
    ParseNow(dialog, QString::fromUtf8(kCompleteEntry) +
                         "\n@article{second, title={Another}}");
    PF_CHECK(!add->isEnabled());
    ParseNow(dialog, "@article{broken, title={Unclosed}");
    PF_CHECK(!add->isEnabled());
    PF_CHECK(ReadBib(directory / "references.bib") == original);
    ParseNow(
        dialog,
        QString::fromUtf8(kCompleteEntry).replace("new2026", "invalid2026"));
    Field(dialog, "title")->setText("Extra } brace");
    AddNow(dialog);
    PF_CHECK(dialog.findChild<QLabel*>("referenceEntryStatus")
                 ->text()
                 .contains("Unbalanced"));
    PF_CHECK(ReadBib(directory / "references.bib") == original);
    dialog.reject();
    PF_CHECK(controller.session().bibliography().Entries().size() == 1);
}

PF_TEST(ReferenceEntryWriteFailurePreservesTheEditableDraft) {
    test::ScopedTempDir directory("pf-reference-gui-write-failure");
    ProjectController controller;
    PF_CHECK(controller.NewProject(QString::fromStdString(directory.string())));
    std::filesystem::create_directory(directory / "references.bib.tmp");
    std::ofstream(directory / "references.bib.tmp/keep")
        << "Prevent replacement";
    AddReferenceDialog dialog(&controller);
    dialog.show();
    ParseNow(dialog, kCompleteEntry);
    AddNow(dialog);
    PF_CHECK(controller.session().bibliography().Entries().empty());
    PF_CHECK(dialog.findChild<QLabel*>("referenceEntryStatus")
                 ->text()
                 .contains("Cannot write"));
    PF_CHECK(dialog.findChild<QLineEdit*>("bibtexCitationKey")->text() ==
             "new2026");
    PF_CHECK(Field(dialog, "title")->text() == "A {NASA} study, with a comma");
    PF_CHECK(!std::filesystem::exists(directory / "references.bib"));
}

PF_TEST(ReferenceEntryFileMenuOpensTheDialogAndRefreshesTheReferenceLibrary) {
    test::ScopedTempDir directory("pf-reference-gui-file-menu");
    MainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>("addReferenceAction");
    PF_CHECK(action != nullptr);
    if (!action)
        return;
    PF_CHECK(!action->isEnabled());
    PF_CHECK(window.menuBar()->actions().front()->menu()->actions().contains(
        action));
    PF_CHECK(window.controller()->NewProject(
        QString::fromStdString(directory.string())));
    PF_CHECK(action->isEnabled());
    window.controller()->Save();
    window.controller()->FlushSaves();
    PF_CHECK(window.OpenProjectDir(QString::fromStdString(directory.string())));
    bool opened = false;
    QTimer::singleShot(0, &window, [&]() {
        auto* dialog =
            window.findChild<AddReferenceDialog*>("addReferenceDialog");
        PF_CHECK(dialog != nullptr);
        if (!dialog)
            return;
        opened = true;
        ParseNow(*dialog, kCompleteEntry);
        AddNow(*dialog);
        window.controller()->CancelBuild();
        dialog->reject();
    });
    action->trigger();
    PF_CHECK(opened);
    PF_CHECK(window.controller()->session().bibliography().Find("new2026") !=
             nullptr);
    auto* panel = window.findChild<OutlinePanel*>();
    auto* list = panel->findChild<QListWidget*>();
    PF_CHECK(list->count() == 1);
    if (list->count() == 1)
        PF_CHECK(list->item(0)->data(Qt::UserRole).toString() == "new2026");
    window.controller()->CloseProject();
    PF_CHECK(!action->isEnabled());
}
