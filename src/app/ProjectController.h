#pragma once
// ProjectController: Qt-side adapter between the UI and the domain layer.
// Owns the ProjectSession and re-emits domain events as Qt signals.
// (Qt isolation rule: domain layer knows nothing about this file.)

#include <QObject>
#include <QString>
#include <memory>

#include "project/ProjectSession.h"

namespace pf::gui {

class ProjectController : public QObject {
    Q_OBJECT

public:
    explicit ProjectController(QObject* parent = nullptr);
    ~ProjectController() override;

    ProjectSession& session() { return *session_; }
    const ProjectSession& session() const { return *session_; }
    bool has_project() const { return session_ != nullptr; }
    ProjectRevision current_revision() const {
        return session_ ? session_->current_revision() : ProjectRevision{};
    }
    PersistenceState persistence_state() const {
        return session_ ? session_->persistence_state()
                        : PersistenceState::Clean;
    }
    PreviewState preview_state() const {
        return session_ ? session_->preview_state() : PreviewState::NoPreview;
    }

    // Lifecycle actions (async work stays inside the session).
    bool NewProject(const QString& dir);
    bool OpenProject(const QString& dir);
    bool OpenProjectWithRecovery(const QString& dir, bool* recovered);
    void CloseProject();
    void Save();
    void StartAutosave();

    // Editing actions - build EditCommands from UI inputs.
    EditResult SetTitle(const QString& text);
    EditResult SetAbstract(const QString& text);
    // "Alice, Bob" -> authors; empty string clears.
    EditResult SetAuthorsText(const QString& comma_separated);
    // "University A; University B" -> affiliations; re-links existing authors
    // by their inline ¹²³ markers order (authors keep prior links where valid).
    EditResult SetAffiliationsText(const QString& semicolon_separated);
    EditResult SetKeywordsText(const QString& comma_separated);
    EditResult InsertSection(const QString& title);
    EditResult InsertSubsection(size_t section_index, const QString& title);
    EditResult InsertParagraph(const NodeId& parent, const QString& text);
    EditResult InsertEquation(const NodeId& parent, const QString& math,
                              bool numbered);
    EditResult InsertFigure(const NodeId& parent, const QString& image_path);
    EditResult EditParagraph(const NodeId& paragraph, const QString& text);
    EditResult EditEquation(const NodeId& equation, const QString& math);
    EditResult RenameSection(const NodeId& section, const QString& title);
    EditResult RenameSubsection(const NodeId& subsection, const QString& title);
    EditResult DeleteBlock(const NodeId& block);
    EditResult DeleteSection(size_t index);
    EditResult InsertCitation(const NodeId& paragraph, const QStringList& keys);
    void Undo();
    void Redo();
    void ChangeTemplate(const QString& template_id);
    void RequestBuild(bool manual = true);
    void CancelBuild();

    // Bibliography
    bool ImportBibliographyText(const QString& bibtex);
    CitationSearchResult SearchCitations(const QString& query) const;

signals:
    void documentChanged();
    void buildStatusChanged(QString phase_text);
    void buildFinished(bool success, QString pdf_path);
    void diagnosticsUpdated(QList<QString> problems);
    void stateChanged(QString persistence, QString preview, QString revision);
    void templateChanged(QString template_id);

private:
    void EmitDocumentChanged();
    EditCommand MakeCmd(FullEditPayload payload) const;

    std::unique_ptr<ProjectSession> session_;
    bool build_in_flight_ = false;
};

}  // namespace pf::gui
