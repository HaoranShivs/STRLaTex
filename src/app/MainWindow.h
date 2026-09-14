#pragma once
// PaperForge main window per the GUI design doc: Outline | Editor | Preview
// (18% / 52% / 30%), Build button in the header, status bar with save/build
// state and word count. Welcome page replaces the workspace when no project
// is open (design #1, #34, #51, #80).

#include <QMainWindow>
#include <QString>

#include "app/ProjectController.h"

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QSplitter;

namespace pf::gui {

class BlockEditor;
class OutlinePanel;
class ProblemsPanel;
class WelcomePage;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ProjectController* controller() { return controller_; }
    // Programmatic open (used by tests/tools): switches to the workspace.
    bool OpenProjectDir(const QString& dir);

private slots:
    void OnNewProject();
    void OnOpenProject();
    void OnSave();
    void OnUndo();
    void OnRedo();
    void OnBuild();
    void OnImportBibliography();
    void OnChangeTemplate(const QString& template_id);

    void RefreshDocumentView();
    void OnBuildFinished(bool success, const QString& pdf_path);
    void OnDiagnosticsUpdated(const QList<QString>& problems);
    void OnBuildStatusChanged(const QString& status);

private:
    void BuildUi();
    void BuildMenus();
    QWidget* BuildHeader();
    void WireEditor();
    void ShowWorkspace(bool show);
    void RefreshPreview();
    void UpdateRequiredHints();
    void RefreshReferenceItems();
    int CountWords() const;

    ProjectController* controller_;

    // Header
    QPushButton* build_button_;
    QComboBox* template_combo_ = nullptr;
    // Workspace
    QStackedWidget* central_stack_;
    WelcomePage* welcome_;
    QWidget* workspace_;
    QSplitter* vertical_splitter_ = nullptr;
    OutlinePanel* outline_;
    BlockEditor* editor_;
    QLabel* preview_label_;
    QWidget* preview_container_;
    ProblemsPanel* problems_;
    // Status bar
    QLabel* save_state_label_;
    QLabel* build_state_label_;
    QLabel* word_count_label_;
    QString current_pdf_path_;
    int last_build_revision_ = -1;
    // Recent projects (QSettings-backed)
    QStringList recent_projects_;
};

}  // namespace pf::gui
