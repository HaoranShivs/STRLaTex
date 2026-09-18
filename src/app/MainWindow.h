#pragma once
// PaperForge main window per the GUI design doc: Outline | Editor | Preview
// (18% / 52% / 30%), Build button in the header, status bar with save/build
// state and word count. Welcome page replaces the workspace when no project
// is open (design #1, #34, #51, #80).

#include <QList>
#include <QMainWindow>
#include <QString>
#include <QStringList>

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

    // P0-01: the single unsaved-changes guard. Every destructive navigation
    // (close, new, open, recent-project, project switch) goes through it and
    // no call site implements its own dirty check.
    enum class DestructiveNavigationDecision : std::uint8_t {
        Proceed,
        Cancel,
    };
    // Fixed order: commit the focused row, pump completed async events, read
    // the authoritative persistence state, then ask the user
    // (Save / Discard / Cancel or Wait / Discard / Cancel or
    // Retry Save / Discard / Cancel). Only proceeds when the target revision
    // is actually Clean or the user explicitly chose to discard.
    DestructiveNavigationDecision MaybeSaveBeforeDestructiveNavigation();
    // Blocks until an in-flight save for the current revision has landed,
    // pumping events so its completion is applied. False on timeout.
    bool WaitForUserSaveCompletion(int timeout_ms = 10000);

protected:
    // P0-01: closing the window is a destructive navigation too; it must go
    // through the same guard instead of relying on a destructor-time
    // best-effort CommitFocused().
    void closeEvent(QCloseEvent* event) override;

private slots:
    void OnNewProject();
    void OnOpenProject();
    // P0-01: closing the current project (back to the Welcome page) is a
    // destructive navigation and shares the same unsaved-changes guard.
    void OnCloseProject();
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
    // P0-06: reports the raw write result; the state label is NOT set here
    // (it renders from the authoritative session state instead).
    void OnSaveFinished(bool success, const QString& detail);
    // P0-06: single renderer for the save/preview state label. Connected to
    // ProjectController::stateChanged and invoked from every state-affecting
    // path; reads ProjectSession::persistence_state() /
    // preview_state() / current_revision() only. No GUI dirty flag exists.
    void RenderProjectState();
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
    // Focus editing mode (UI plan §11): collapse Outline + Preview so the
    // editor owns the whole window for long writing sessions; the second
    // click restores the previous splitter sizes.
    void OnToggleFocusMode(bool on);
    // Problem -> Block navigation (Build Diagnostics plan §25/§28/§48):
    // focus the owning block, or fall back to the Build Log; a deleted block
    // is reported, never a crash.
    void OnProblemActivated(const pf::Diagnostic& diagnostic);

    ProjectController* controller_;

    // Header
    QPushButton* build_button_;
    QComboBox* template_combo_ = nullptr;
    QPushButton* focus_button_ = nullptr;
    // Workspace
    QStackedWidget* central_stack_;
    WelcomePage* welcome_;
    QWidget* workspace_;
    QSplitter* main_splitter_ = nullptr;
    QList<int> pre_focus_sizes_;
    bool focus_mode_ = false;
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
