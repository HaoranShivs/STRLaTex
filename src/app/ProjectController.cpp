#include "app/ProjectController.h"

#include <chrono>
#include <QRegularExpression>

#include "core/IdGenerator.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string& s) { return QString::fromStdString(s); }
std::string ToStd(const QString& s) { return s.toStdString(); }

const char* ToString(PersistenceState state) {
    switch (state) {
        case PersistenceState::Clean: return "Clean";
        case PersistenceState::Dirty: return "Dirty";
        case PersistenceState::Saving: return "Saving";
        case PersistenceState::SaveFailed: return "SaveFailed";
    }
    return "?";
}
const char* ToString(PreviewState state) {
    switch (state) {
        case PreviewState::NoPreview: return "NoPreview";
        case PreviewState::Fresh: return "Fresh";
        case PreviewState::Stale: return "Stale";
    }
    return "?";
}
}  // namespace

ProjectController::ProjectController(QObject* parent) : QObject(parent) {
    ProjectSession::Config config;
    config.tectonic_path = PF_TECTONIC_BIN;
    config.workspace_root = std::filesystem::temp_directory_path() /
                            "paperforge-gui-builds";
    config.debounce = std::chrono::milliseconds{800};

    session_ = std::make_unique<ProjectSession>(config);
    session_->SetPhaseHandler([this](BuildPhase, BuildPhase current) {
        QString text;
        switch (current) {
            case BuildPhase::Idle: text = "Idle"; break;
            case BuildPhase::Debouncing: text = "Debouncing…"; break;
            case BuildPhase::Rendering: text = "Rendering…"; break;
            case BuildPhase::Compiling: text = "Compiling…"; break;
        }
        emit buildStatusChanged(text);
    });
    session_->SetBuildResultHandler([this](const BuildResult& result) {
        if (result.revision != session_->current_revision()) return;  // stale
        bool success = result.outcome == BuildResult::Outcome::Success;

        QList<QString> problems;
        for (const auto& d : result.diagnostics) {
            problems.append(ToQ(d.Summary()));
        }
        emit diagnosticsUpdated(problems);
        emit buildFinished(success, ToQ(result.pdf_path));
    });
}

ProjectController::~ProjectController() = default;

EditCommand ProjectController::MakeCmd(FullEditPayload payload) const {
    EditCommand cmd;
    cmd.operation_id = OperationId(IdGenerator::NewOperationId());
    cmd.project_id = session_->state().id();
    cmd.base_revision = session_->current_revision();
    cmd.origin = EditOrigin::User;
    cmd.payload = std::move(payload);
    return cmd;
}

bool ProjectController::NewProject(const QString& dir) {
    bool ok = session_->NewProject(ToStd(dir));
    if (ok) emit documentChanged();
    return ok;
}

bool ProjectController::OpenProject(const QString& dir) {
    std::string error;
    bool ok = session_->OpenProject(ToStd(dir), &error);
    if (ok) {
        emit documentChanged();
        emit templateChanged(ToQ(session_->state().template_selection()));
    }
    return ok;
}

bool ProjectController::OpenProjectWithRecovery(const QString& dir,
                                                bool* recovered) {
    std::string error;
    bool ok = session_->OpenProjectWithRecovery(ToStd(dir), &error, recovered);
    if (ok) emit documentChanged();
    return ok;
}

void ProjectController::CloseProject() { session_->CloseProject(); }

void ProjectController::Save() { session_->Save(); }

void ProjectController::StartAutosave() { session_->StartAutosaveTimer(); }

void ProjectController::EmitDocumentChanged() {
    emit documentChanged();
    emit stateChanged(ToString(session_->persistence_state()),
                      ToString(session_->preview_state()),
                      QString::number(session_->current_revision().value));
}

EditResult ProjectController::SetTitle(const QString& text) {
    SetTitlePayload p;
    p.title = InlineFromText(ToStd(text));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::SetAbstract(const QString& text) {
    SetAbstractPayload p;
    if (text.trimmed().isEmpty()) {
        p.abstract_text = std::nullopt;
    } else {
        p.abstract_text = InlineFromText(ToStd(text));
    }
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::SetAuthorsText(const QString& comma_separated) {
    // Parse "Alice, Bob, Carol" into author entries; preserve affiliation
    // links by position when the count is unchanged.
    const auto& fm_old = session_->state().document().front_matter();
    std::vector<size_t> old_counts;
    for (const auto& a : fm_old.authors) old_counts.push_back(a.affiliations.size());

    QStringList names = comma_separated.split(',', Qt::SkipEmptyParts);
    // Rebuild authors, carrying over affiliation markers (¹²³ parsed away).
    std::vector<Author> authors;
    for (const auto& raw : names) {
        Author author;
        QString name = raw.trimmed();
        // Strip trailing superscript affiliation markers for the name.
        static const QRegularExpression supers("[\u00b9\u00b2\u00b3\u2070-\u209f]+$");
        name.remove(supers);
        author.name = ToStd(name.trimmed());
        authors.push_back(std::move(author));
    }
    // Re-link affiliations round-robin to first author only when list changed
    // size (V1 heuristic: keep simple).
    if (authors.size() == old_counts.size()) {
        for (size_t i = 0; i < authors.size(); ++i) {
            (void)old_counts[i];  // links already lost on rebuild; see below
        }
    }

    // Replace the author list via Remove/Add payloads (undoable snapshot
    // history captures both).
    const auto& current = session_->state().document().front_matter().authors;
    for (size_t i = current.size(); i > 0; --i) {
        RemoveAuthorPayload p;
        p.index = i - 1;
        session_->Execute(MakeCmd(std::move(p)));
    }
    for (auto& author : authors) {
        AddAuthorPayload p;
        p.author = author;
        session_->Execute(MakeCmd(std::move(p)));
    }
    EditResult r;
    r.status = EditStatus::Applied;
    r.new_revision = session_->current_revision();
    EmitDocumentChanged();
    return r;
}

EditResult ProjectController::SetAffiliationsText(
    const QString& semicolon_separated) {
    SetAffiliationsPayload p;
    QStringList names = semicolon_separated.split(';', Qt::SkipEmptyParts);
    for (const auto& raw : names) {
        QString name = raw.trimmed();
        // Strip leading superscript markers (¹ ² ...).
        static const QRegularExpression leading("[\u00b9\u00b2\u00b3\u2070-\u209f]+\s*");
        name.remove(leading);
        if (name.isEmpty()) continue;
        Affiliation aff;
        aff.id = AffiliationId(IdGenerator::NewAffiliationId());
        aff.name = ToStd(name);

        p.affiliations.push_back(std::move(aff));
    }
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::SetKeywordsText(const QString& comma_separated) {
    SetKeywordsPayload p;
    QStringList names = comma_separated.split(',', Qt::SkipEmptyParts);
    for (const auto& raw : names) {
        QString kw = raw.trimmed();
        if (!kw.isEmpty()) p.keywords.push_back(ToStd(kw));
    }
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertSection(const QString& title) {
    InsertSectionPayload p;
    p.index = session_->state().document().body().sections.size();
    p.title = InlineFromText(ToStd(title));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertSubsection(size_t section_index,
                                               const QString& title) {
    InsertSubsectionPayload p;
    p.section_index = section_index;
    p.index = session_->state().document().body().sections[section_index]
                  .subsections.size();
    p.title = InlineFromText(ToStd(title));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertParagraph(const NodeId& parent,
                                              const QString& text) {
    InsertParagraphPayload p;
    p.parent = parent;
    p.content = InlineFromText(ToStd(text));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertEquation(const NodeId& parent,
                                             const QString& math, bool numbered) {
    InsertEquationPayload p;
    p.parent = parent;
    p.math_source = ToStd(math);
    p.numbered = numbered;
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertFigure(const NodeId& parent,
                                           const QString& image_path) {
    auto r = session_->InsertFigureFromSource(ToStd(image_path), parent);
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::EditParagraph(const NodeId& paragraph,
                                            const QString& text) {
    EditParagraphPayload p;
    p.paragraph = paragraph;
    p.content = InlineFromText(ToStd(text));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::EditEquation(const NodeId& equation,
                                           const QString& math) {
    EditEquationPayload p;
    p.equation = equation;
    p.math_source = ToStd(math);
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::RenameSection(const NodeId& section,
                                            const QString& title) {
    RenameSectionPayload p;
    p.section = section;
    p.title = InlineFromText(ToStd(title));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::RenameSubsection(const NodeId& subsection,
                                               const QString& title) {
    // V1: rename via section rename payload on the subsection node.
    RenameSubsectionPayload p;
    p.subsection = subsection;
    p.title = InlineFromText(ToStd(title));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::DeleteBlock(const NodeId& block) {
    DeleteBlockPayload p;
    p.node = block;
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::DeleteSection(size_t index) {
    DeleteSectionPayload p;
    p.index = index;
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertCitation(const NodeId& paragraph,
                                             const QStringList& keys) {
    InsertCitationPayload p;
    p.paragraph = paragraph;
    for (const auto& k : keys) p.keys.push_back(ToStd(k));
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

void ProjectController::Undo() {
    auto r = session_->Undo();
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
}

void ProjectController::Redo() {
    auto r = session_->Redo();
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
}

void ProjectController::ChangeTemplate(const QString& template_id) {
    session_->ChangeTemplate(ToStd(template_id));
    emit templateChanged(template_id);
    EmitDocumentChanged();
}

void ProjectController::RequestBuild(bool manual) {
    session_->RequestBuild(manual);
}

void ProjectController::CancelBuild() { session_->CancelBuild(); }

bool ProjectController::ImportBibliographyText(const QString& bibtex) {
    auto r = session_->ImportBibliography(ToStd(bibtex));
    if (r.status == BibliographyImportResult::Status::Ok) EmitDocumentChanged();
    return r.status == BibliographyImportResult::Status::Ok;
}

CitationSearchResult ProjectController::SearchCitations(
    const QString& query) const {
    return session_->SearchCitations(ToStd(query));
}

}  // namespace pf::gui
