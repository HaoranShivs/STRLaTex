#include "app/InsertTableDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "document/Document.h"

namespace pf::gui {

InsertTableDialog::InsertTableDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("insertTableDialog"));
    setWindowTitle(QStringLiteral("Insert Table"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    rows_ = new QSpinBox(this);
    rows_->setObjectName(QStringLiteral("tableRows"));
    rows_->setRange(1, static_cast<int>(Table::kMaxRows));
    rows_->setValue(3);
    columns_ = new QSpinBox(this);
    columns_->setObjectName(QStringLiteral("tableColumns"));
    columns_->setRange(1, static_cast<int>(Table::kMaxColumns));
    columns_->setValue(3);
    header_ = new QCheckBox(QStringLiteral("First row is a header"), this);
    header_->setObjectName(QStringLiteral("tableHasHeader"));
    header_->setChecked(true);
    form->addRow(QStringLiteral("Rows (including header)"), rows_);
    form->addRow(QStringLiteral("Columns"), columns_);
    form->addRow(header_);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Insert"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

int InsertTableDialog::Rows() const {
    return rows_->value();
}
int InsertTableDialog::Columns() const {
    return columns_->value();
}
bool InsertTableDialog::HasHeaderRow() const {
    return header_->isChecked();
}

} // namespace pf::gui
