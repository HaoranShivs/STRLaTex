#pragma once
#include "build/TemplateVerifier.h"
#include <QDialog>
#include <QTemporaryDir>
#include <atomic>
#include <optional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QThread;

namespace pf::gui {
class ProjectController;
class TemplateImportDialog : public QDialog {
    Q_OBJECT
  public:
    TemplateImportDialog(ProjectController* controller, QString archive,
                         QWidget* parent = nullptr);
    ~TemplateImportDialog() override;

  private:
    void Start();
    void Complete();
    void Import();
    ProjectController* controller_;
    QString archive_;
    QLabel* status_;
    QPlainTextEdit* details_;
    QComboBox* profiles_;
    QPushButton* import_;
    QThread* worker_ = nullptr;
    QTemporaryDir workspace_;
    std::atomic<bool> cancelled_{false};
    std::optional<Result<VerifiedTemplatePackage>> result_;
};
} // namespace pf::gui
