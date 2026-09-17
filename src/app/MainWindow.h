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
class PdfPreview;
class ProblemsPanel;
class WelcomePage;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    ProjectController* controller() { return controller_; }
    // Programmatic open (used by tests/tools): switches to the workspace.
    bool OpenProjectDir(const QString& dir);
    // Programmatic preview zoom/scroll (used by the UI verification tool).
    void ZoomPreviewForTest(double zoom, double scroll_x = 0.0,
                            double scroll_y = 0.0);

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
    void RefreshSidePanels();
    // Typed preview event: carries project/build/revision identity of the PDF.
    void OnPreviewUpdated(const pf::PreviewUpdate& update);
    void OnSaveFinished(bool success, const QString& detail);
    // Structured diagnostics of the accepted build (Build Diagnostics plan
    // §36-§37): MainWindow only wires panels to data; no log parsing here.
    void OnBuildCompleted(const pf::BuildResult& result);
    // One streamed build-log event of the current build.
    void OnBuildEvent(const pf::BuildEvent& event);
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
    // Problem -> Block navigation (Build Diagnostics plan §25/§28/§48):
    // focus the owning block, or fall back to the Build Log; a deleted block
    // is reported, never a crash.
    void OnProblemActivated(const pf::Diagnostic& diagnostic);

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
    PdfPreview* preview_;
    QWidget* preview_container_;
    ProblemsPanel* problems_;
    // Status bar
    QLabel* save_state_label_;
    QLabel* build_state_label_;
    QLabel* word_count_label_;
    QString current_pdf_path_;
    int last_build_revision_ = -1;
    // Revision of the last completed build, for the stale marker (plan §49).
    pf::ProjectRevision last_built_revision_;
    // Outcome of the last build: OnPreviewUpdated reads it so a Cancelled
    // attempt reports "cancelled", never "failed" (plan §34).
    pf::BuildResult::Outcome last_outcome_ = pf::BuildResult::Outcome::Failure;
    bool has_built_ = false;
    // True from BuildStarted until the terminal event: the Build button then
    // acts as Cancel (plan §39) and Problems shows "Building...".
    bool building_ = false;
    // Set once the window is being destroyed: editor signals and document
    // refreshes must then be ignored entirely.
    bool shutting_down_ = false;
    // Set when a refresh had to be skipped because the user was typing.
    bool pending_structural_refresh_ = false;
    // Recent projects (QSettings-backed)
    QStringList recent_projects_;
};

}  // namespace pf::gui
