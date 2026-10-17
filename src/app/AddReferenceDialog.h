#pragma once

#include <QDialog>
#include <QMap>
#include <QPointer>

#include "bibliography/BibliographyService.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTableWidget;
class QTimer;

namespace pf::gui {

class ProjectController;

class AddReferenceDialog : public QDialog {
    Q_OBJECT

  public:
    explicit AddReferenceDialog(ProjectController* controller,
                                QWidget* parent = nullptr);

  signals:
    void ReferenceAdded(const QString& key);

  private:
    void ParseSource();
    void LoadEntry(const BibEntry& entry);
    std::optional<BibEntry> DraftEntry(QString* error) const;
    void AddReference();
    void ResetForm();
    void SetStatus(const QString& text, const char* color);

    QPointer<ProjectController> controller_;
    QPlainTextEdit* source_ = nullptr;
    QComboBox* type_ = nullptr;
    QLineEdit* key_ = nullptr;
    QMap<QString, QLineEdit*> fields_;
    QTableWidget* extra_fields_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QScrollArea* details_scroll_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* add_ = nullptr;
    QTimer* debounce_ = nullptr;
    bool source_valid_ = true;
};

} // namespace pf::gui
