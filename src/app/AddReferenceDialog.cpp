#include "app/AddReferenceDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "app/ProjectController.h"
#include "app/Theme.h"

namespace pf::gui {
namespace {

struct Field {
    const char* name;
    const char* label;
};

constexpr Field kFields[] = {
    {"title", "Title"},
    {"author", "Authors"},
    {"year", "Year"},
    {"journal", "Journal"},
    {"booktitle", "Book / conference title"},
    {"volume", "Volume"},
    {"number", "Issue / number"},
    {"pages", "Pages"},
    {"publisher", "Publisher"},
    {"editor", "Editors"},
    {"school", "School"},
    {"institution", "Institution"},
    {"doi", "DOI"},
    {"url", "URL"},
    {"month", "Month"},
    {"note", "Note"},
};

QStringList MissingFields(const BibEntry& entry) {
    QStringList missing;
    auto empty = [&](const char* name) {
        const auto it = entry.fields.find(name);
        return it == entry.fields.end() || it->second.empty();
    };
    auto require = [&](const char* name, const char* label) {
        if (empty(name))
            missing << QString::fromLatin1(label);
    };
    require("title", "Title");
    const auto& type = entry.entry_type;
    if (type == "book" || type == "inbook") {
        if (empty("author") && empty("editor"))
            missing << QStringLiteral("Authors or editors");
    } else if (type != "proceedings" && type != "manual" && type != "misc" &&
               type != "booklet") {
        require("author", "Authors");
    }
    if (type != "misc" && type != "manual" && type != "unpublished" &&
        type != "booklet")
        require("year", "Year");
    if (type == "article")
        require("journal", "Journal");
    if (type == "inproceedings" || type == "conference" ||
        type == "incollection")
        require("booktitle", "Book / conference title");
    if (type == "book" || type == "inbook" || type == "incollection")
        require("publisher", "Publisher");
    if (type == "phdthesis" || type == "mastersthesis")
        require("school", "School");
    if (type == "techreport")
        require("institution", "Institution");
    if (type == "unpublished")
        require("note", "Note");
    return missing;
}

} // namespace

AddReferenceDialog::AddReferenceDialog(ProjectController* controller,
                                       QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("addReferenceDialog"));
    setWindowTitle(QStringLiteral("Add Reference"));
    setModal(true);
    resize(780, 720);
    setMinimumSize(560, 520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* help = new QLabel(
        QStringLiteral(
            "Paste one BibTeX entry from Google Scholar or another source, "
            "or enter the details below. Missing information stays blank."),
        this);
    help->setWordWrap(true);
    layout->addWidget(help);
    source_ = new QPlainTextEdit(this);
    source_->setObjectName(QStringLiteral("bibtexSource"));
    source_->setFont(theme::MonoFont());
    source_->setPlaceholderText(QStringLiteral(
        "@article{smith2026,\n  title = {Article title},\n"
        "  author = {Jane Smith and John Doe},\n  year = {2026}\n}"));
    source_->setFixedHeight(132);
    layout->addWidget(source_);
    auto* parse_row = new QHBoxLayout;
    auto* parse = new QPushButton(QStringLiteral("Parse BibTeX"), this);
    parse->setObjectName(QStringLiteral("parseBibTeXButton"));
    parse_row->addWidget(parse);
    auto* parse_hint = new QLabel(
        QStringLiteral("Pasted text is also parsed automatically."), this);
    parse_hint->setStyleSheet(QString("color: %1;").arg(theme::kSecondaryText));
    parse_row->addWidget(parse_hint, 1);
    layout->addLayout(parse_row);

    auto* identity = new QFormLayout;
    type_ = new QComboBox(this);
    type_->setObjectName(QStringLiteral("bibtexEntryType"));
    type_->setEditable(true);
    type_->addItems({"article", "inproceedings", "book", "incollection",
                     "inbook", "proceedings", "phdthesis", "mastersthesis",
                     "techreport", "misc", "unpublished", "manual", "booklet"});
    identity->addRow(QStringLiteral("Entry type"), type_);
    key_ = new QLineEdit(this);
    key_->setObjectName(QStringLiteral("bibtexCitationKey"));
    key_->setPlaceholderText(
        QStringLiteral("e.g. smith2026 (unique within this project)"));
    identity->addRow(QStringLiteral("Citation key (required)"), key_);
    layout->addLayout(identity);

    auto* tabs = new QTabWidget(this);
    tabs_ = tabs;
    auto* scroll = new QScrollArea(tabs);
    details_scroll_ = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* details = new QWidget(scroll);
    auto* form = new QFormLayout(details);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const auto& field : kFields) {
        auto* edit = new QLineEdit(details);
        const QString name = QString::fromLatin1(field.name);
        edit->setObjectName(QStringLiteral("bibtexField_") + name);
        if (name == "author" || name == "editor")
            edit->setPlaceholderText(QStringLiteral(
                "Jane Smith and John Doe (separate names with 'and')"));
        fields_.insert(name, edit);
        form->addRow(QString::fromLatin1(field.label), edit);
    }
    scroll->setWidget(details);
    tabs->addTab(scroll, QStringLiteral("Details"));

    auto* extra = new QWidget(tabs);
    auto* extra_layout = new QVBoxLayout(extra);
    auto* extra_hint = new QLabel(
        QStringLiteral(
            "Additional fields from the pasted entry are preserved here. "
            "You can also add fields such as ISBN, address or abstract."),
        extra);
    extra_hint->setWordWrap(true);
    extra_layout->addWidget(extra_hint);
    extra_fields_ = new QTableWidget(0, 2, extra);
    extra_fields_->setObjectName(QStringLiteral("bibtexExtraFields"));
    extra_fields_->setHorizontalHeaderLabels(
        {QStringLiteral("Field"), QStringLiteral("Value")});
    extra_fields_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    extra_fields_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    extra_fields_->verticalHeader()->hide();
    extra_layout->addWidget(extra_fields_, 1);
    auto* extra_buttons = new QHBoxLayout;
    auto* add_field = new QPushButton(QStringLiteral("Add field"), extra);
    auto* remove_field = new QPushButton(QStringLiteral("Remove field"), extra);
    extra_buttons->addWidget(add_field);
    extra_buttons->addWidget(remove_field);
    extra_buttons->addStretch();
    extra_layout->addLayout(extra_buttons);
    tabs->addTab(extra, QStringLiteral("Other fields"));
    layout->addWidget(tabs, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("referenceEntryStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    SetStatus(QStringLiteral("Review the fields, then click Add. Optional "
                             "fields may remain empty."),
              theme::kSecondaryText);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    add_ =
        buttons->addButton(QStringLiteral("Add"), QDialogButtonBox::ActionRole);
    add_->setObjectName(QStringLiteral("addReferenceButton"));
    add_->setDefault(true);
    add_->setEnabled(controller_ && controller_->has_project());
    add_->setStyleSheet(
        QString(
            "QPushButton { background: %1; color: white; padding: 6px 22px; "
            "border: none; border-radius: 4px; }")
            .arg(theme::kAccent));
    layout->addWidget(buttons);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(250);
    connect(source_, &QPlainTextEdit::textChanged, this, [this]() {
        source_valid_ = false;
        add_->setEnabled(false);
        debounce_->start();
    });
    connect(debounce_, &QTimer::timeout, this,
            &AddReferenceDialog::ParseSource);
    connect(parse, &QPushButton::clicked, this,
            &AddReferenceDialog::ParseSource);
    connect(add_, &QPushButton::clicked, this,
            &AddReferenceDialog::AddReference);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(add_field, &QPushButton::clicked, this, [this]() {
        const int row = extra_fields_->rowCount();
        extra_fields_->insertRow(row);
        extra_fields_->setItem(row, 0, new QTableWidgetItem);
        extra_fields_->setItem(row, 1, new QTableWidgetItem);
        extra_fields_->setCurrentCell(row, 0);
        extra_fields_->editItem(extra_fields_->item(row, 0));
    });
    connect(remove_field, &QPushButton::clicked, this, [this]() {
        extra_fields_->removeRow(extra_fields_->currentRow());
    });
    source_->setFocus();
}

void AddReferenceDialog::SetStatus(const QString& text, const char* color) {
    status_->setText(text);
    status_->setStyleSheet(QString("color: %1;").arg(color));
}

void AddReferenceDialog::LoadEntry(const BibEntry& entry) {
    tabs_->setCurrentIndex(0);
    details_scroll_->verticalScrollBar()->setValue(0);
    type_->setCurrentText(entry.entry_type.empty()
                              ? QStringLiteral("article")
                              : QString::fromStdString(entry.entry_type));
    key_->setText(QString::fromStdString(entry.key));
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        const auto field = entry.fields.find(it.key().toStdString());
        it.value()->setText(field == entry.fields.end()
                                ? QString()
                                : QString::fromStdString(field->second));
    }
    extra_fields_->setRowCount(0);
    for (const auto& [name, value] : entry.fields) {
        if (fields_.contains(QString::fromStdString(name)))
            continue;
        const int row = extra_fields_->rowCount();
        extra_fields_->insertRow(row);
        extra_fields_->setItem(
            row, 0, new QTableWidgetItem(QString::fromStdString(name)));
        extra_fields_->setItem(
            row, 1, new QTableWidgetItem(QString::fromStdString(value)));
    }
}

void AddReferenceDialog::ParseSource() {
    debounce_->stop();
    const QString text = source_->toPlainText().trimmed();
    source_valid_ = false;
    if (!text.isEmpty()) {
        const auto parsed = ParseBibTeX(text.toStdString());
        if (!parsed.error.empty()) {
            SetStatus(QString::fromStdString(parsed.error), theme::kError);
            return;
        }
        if (parsed.entries.size() != 1) {
            SetStatus(QStringLiteral("Paste exactly one BibTeX reference. Use "
                                     "File > Import Bibliography "
                                     "to import a complete .bib file."),
                      theme::kError);
            return;
        }
        LoadEntry(parsed.entries.front());
    }
    source_valid_ = true;
    add_->setEnabled(controller_ && controller_->has_project());
    SetStatus(QStringLiteral("Review the parsed details or fill in missing "
                             "information, then click Add."),
              theme::kSecondaryText);
}

std::optional<BibEntry> AddReferenceDialog::DraftEntry(QString* error) const {
    BibEntry entry;
    entry.entry_type = type_->currentText().trimmed().toLower().toStdString();
    entry.key = key_->text().trimmed().toStdString();
    for (auto it = fields_.cbegin(); it != fields_.cend(); ++it) {
        if (!it.value()->text().trimmed().isEmpty())
            entry.fields[it.key().toStdString()] =
                it.value()->text().trimmed().toStdString();
    }
    for (int row = 0; row < extra_fields_->rowCount(); ++row) {
        const auto* name_item = extra_fields_->item(row, 0);
        const auto* value_item = extra_fields_->item(row, 1);
        const QString name =
            name_item ? name_item->text().trimmed().toLower() : QString();
        const QString value =
            value_item ? value_item->text().trimmed() : QString();
        if (name.isEmpty() && value.isEmpty())
            continue;
        if (name.isEmpty() || fields_.contains(name) ||
            entry.fields.count(name.toStdString())) {
            *error = QStringLiteral("Each additional field needs a unique "
                                    "name. Edit standard fields in Details.");
            return std::nullopt;
        }
        entry.fields[name.toStdString()] = value.toStdString();
    }
    return entry;
}

void AddReferenceDialog::AddReference() {
    if (!source_valid_ || !controller_ || !controller_->has_project())
        return;
    QString error;
    const auto entry = DraftEntry(&error);
    std::string validation_error;
    if (!entry || !SerializeBibEntry(*entry, &validation_error)) {
        SetStatus(entry ? QString::fromStdString(validation_error) : error,
                  theme::kError);
        return;
    }
    if (controller_->session().bibliography().Find(entry->key)) {
        SetStatus(
            QStringLiteral(
                "Citation key '%1' already exists. Choose a different key.")
                .arg(QString::fromStdString(entry->key)),
            theme::kError);
        key_->setFocus();
        return;
    }
    const auto missing = MissingFields(*entry);
    if (!missing.isEmpty()) {
        QMessageBox confirm(
            QMessageBox::Question, QStringLiteral("Add incomplete reference?"),
            QStringLiteral("The following required details are empty:\n\n%1\n\n"
                           "Add this reference with these fields left blank?")
                .arg(missing.join(QStringLiteral(", "))),
            QMessageBox::NoButton, this);
        confirm.setObjectName(
            QStringLiteral("incompleteReferenceConfirmation"));
        confirm.setTextFormat(Qt::PlainText);
        auto* add_anyway = confirm.addButton(QStringLiteral("Add Anyway"),
                                             QMessageBox::AcceptRole);
        auto* edit = confirm.addButton(QStringLiteral("Continue Editing"),
                                       QMessageBox::RejectRole);
        confirm.setDefaultButton(edit);
        confirm.setEscapeButton(edit);
        confirm.exec();
        if (confirm.clickedButton() != add_anyway)
            return;
    }
    const auto result = controller_->AddBibliographyEntry(*entry);
    if (result.status != BibliographyImportResult::Status::Ok) {
        SetStatus(QString::fromStdString(result.detail), theme::kError);
        return;
    }
    const QString key = QString::fromStdString(entry->key);
    ResetForm();
    const auto& path =
        controller_->session().state().settings().bibliography_path;
    SetStatus(QStringLiteral(
                  "Added '%1' to %2. Paste another entry or close this window.")
                  .arg(key, QString::fromStdString(path)),
              theme::kOk);
    emit ReferenceAdded(key);
}

void AddReferenceDialog::ResetForm() {
    debounce_->stop();
    const QSignalBlocker block(source_);
    source_->clear();
    LoadEntry({});
    source_valid_ = true;
    add_->setEnabled(controller_ && controller_->has_project());
    source_->setFocus();
}

} // namespace pf::gui
