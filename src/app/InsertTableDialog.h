#pragma once

#include <QDialog>

class QCheckBox;
class QSpinBox;

namespace pf::gui {

class InsertTableDialog : public QDialog {
    Q_OBJECT

  public:
    explicit InsertTableDialog(QWidget* parent = nullptr);
    int Rows() const;
    int Columns() const;
    bool HasHeaderRow() const;

  private:
    QSpinBox* rows_ = nullptr;
    QSpinBox* columns_ = nullptr;
    QCheckBox* header_ = nullptr;
};

} // namespace pf::gui
