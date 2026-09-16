#pragma once
// ProjectController: Qt-side adapter between the UI and the domain layer.
// Owns the ProjectSession and re-emits domain events as Qt signals.
// (Qt isolation rule: domain layer knows nothing about this file.)

#include <QObject>
#include <QString>
#include <memory>

#include "project/PreviewUpdate.h"
#include "project/ProjectSession.h"

class QTimer;

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
    // Block until queued saves are written (tests/tools only).
    void FlushSaves();
    void StartAutosave();
    void StopAutosave();

    // Editing actions - build EditCommands from UI inputs.
    EditResult SetTitle(const QString& text);
    EditResult SetAbstract(const QString& text);
    // "Alice, Bob" -> authors; empty string clears.
    EditResult SetAuthorsText(const QString& comma_separated);
    // "University A; University B" -> affiliations; re-links existing authors
    // by their inline ¹²³ markers order (authors keep prior links where valid).
    EditResult SetAffiliationsText(const QString& semicolon_separated);

    // Graphical author <-> institution binding: link or unlink one author
    // from one institution, keeping the document's institution order.
    EditResult SetAuthorAffiliation(size_t author_index,
                                    const AffiliationId& affiliation,
                                    bool linked);
    EditResult SetKeywordsText(const QString& comma_separated);
    EditResult InsertSection(const QString& title);
    EditResult InsertSectionAfter(const NodeId& anchor, const QString& title);
    EditResult InsertSubsection(size_t section_index, const QString& title);
    EditResult InsertSubsectionAfter(const NodeId& anchor,
                                     const QString& title);
    EditResult InsertSubsubsection(size_t section_index, size_t subsection_index,
                                   const QString& title);
    EditResult InsertSubsubsectionAfter(const NodeId& anchor,
                                        const QString& title);
    EditResult RenameSubsubsection(const NodeId& subsubsection,
                                   const QString& title);
    EditResult DeleteSubsubsection(const NodeId& subsubsection);
    EditResult InsertParagraph(const NodeId& parent, const QString& text);
    EditResult InsertParagraphAfter(const NodeId& anchor, const QString& text);
    EditResult InsertEquation(const NodeId& parent, const QString& math,
                              bool numbered, const QString& label = QString());
    EditResult InsertEquationAfter(const NodeId& anchor, const QString& math,
                                   bool numbered,
                                   const QString& label = QString());
    EditResult InsertFigure(const NodeId& parent, const QString& image_path);
    EditResult InsertFigureAfter(const NodeId& anchor, const QString& image_path);
    EditResult InsertTableAfter(const NodeId& anchor);
    EditResult EditParagraph(const NodeId& paragraph, const QString& text);
    // Structured variant used by InlineEditor: bold/italic runs, citations,
    // cross references and inline equations survive the round trip.
    EditResult EditParagraphRich(const NodeId& paragraph,
                                 const InlineContent& content);
    EditResult EditEquation(const NodeId& equation, const QString& math,
                            bool numbered, const QString& label);
    EditResult RenameSection(const NodeId& section, const QString& title);
    EditResult RenameSubsection(const NodeId& subsection, const QString& title);
    EditResult EditCaption(const NodeId& block, const QString& caption);
    EditResult DeleteBlock(const NodeId& block);
    EditResult DeleteNode(const NodeId& node);
    EditResult MoveNode(const NodeId& node, int direction);

    // Drag-and-drop reordering: put `node` directly after `anchor` (a block, a
    // subsection or a section). Keeps the document order everywhere else.
    EditResult MoveNodeAfter(const NodeId& node, const NodeId& anchor);
    EditResult DeleteSection(size_t index);
    EditResult InsertCitation(const NodeId& paragraph, const QStringList& keys);
    EditResult InsertCrossReference(const NodeId& paragraph,
                                    const NodeId& target);
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
    // Typed replacement for buildFinished(bool, pdf_path): carries the
    // project/build/revision identity of the PDF so the view can reject
    // anything that no longer belongs to the current document.
    void previewUpdated(const pf::PreviewUpdate& update);
    void diagnosticsUpdated(QList<QString> problems);
    void saveFinished(bool success, QString detail);
    void stateChanged(QString persistence, QString preview, QString revision);
    void templateChanged(QString template_id);

private:
    struct InsertionPoint {
        NodeId parent;
        std::optional<size_t> index;
    };

    void EmitDocumentChanged();
    // Application-thread drain of ProjectSession's event queue.
    void PumpEvents();
    EditCommand MakeCmd(FullEditPayload payload) const;
    std::optional<InsertionPoint> ResolveInsertionPoint(
        const NodeId& anchor) const;
    EditResult ExecuteAndNotify(FullEditPayload payload);

    std::unique_ptr<ProjectSession> session_;
    QTimer* pump_timer_ = nullptr;
    bool shutting_down_ = false;
    bool build_in_flight_ = false;
};

}  // namespace pf::gui

Q_DECLARE_METATYPE(pf::PreviewUpdate)
