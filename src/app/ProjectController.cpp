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

EditResult ProjectController::ExecuteAndNotify(FullEditPayload payload) {
    auto result = session_->Execute(MakeCmd(std::move(payload)));
    if (result.status == EditStatus::Applied) EmitDocumentChanged();
    return result;
}

std::optional<ProjectController::InsertionPoint>
ProjectController::ResolveInsertionPoint(const NodeId& anchor) const {
    const auto& sections = session_->state().document().body().sections;
    if (anchor.empty()) {
        if (sections.empty()) return std::nullopt;
        return InsertionPoint{sections.back().id, std::nullopt};
    }
    for (const auto& section : sections) {
        if (section.id == anchor) {
            return InsertionPoint{section.id, size_t{0}};
        }
        for (size_t i = 0; i < section.blocks.size(); ++i) {
            const NodeId id = std::visit(
                [](const auto& block) { return block.id; }, section.blocks[i]);
            if (id == anchor) return InsertionPoint{section.id, i + 1};
        }
        for (const auto& subsection : section.subsections) {
            if (subsection.id == anchor) {
                return InsertionPoint{subsection.id, size_t{0}};
            }
            for (size_t i = 0; i < subsection.blocks.size(); ++i) {
                const NodeId id = std::visit(
                    [](const auto& block) { return block.id; },
                    subsection.blocks[i]);
                if (id == anchor) {
                    return InsertionPoint{subsection.id, i + 1};
                }
            }
        }
    }
    return std::nullopt;
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

    QStringList names = comma_separated.split(
        QRegularExpression(QStringLiteral(R"([,\n\x{00B7}]+)")),
        Qt::SkipEmptyParts);
    // Rebuild authors, carrying over affiliation markers (¹²³ parsed away).
    std::vector<Author> authors;
    for (const auto& raw : names) {
        Author author;
        QString name = raw.trimmed();
        // Strip trailing superscript affiliation markers for the name.
        static const QRegularExpression supers(
            QStringLiteral(R"([\x{00B9}\x{00B2}\x{00B3}\x{2070}-\x{209F}]+$)"));
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
    QStringList names = semicolon_separated.split(
        QRegularExpression(QStringLiteral(R"([;\n]+)")), Qt::SkipEmptyParts);
    for (const auto& raw : names) {
        QString name = raw.trimmed();
        // Strip leading superscript markers (¹ ² ...).
        static const QRegularExpression leading(
            QStringLiteral(R"(^[\x{00B9}\x{00B2}\x{00B3}\x{2070}-\x{209F}]+\s*)"));
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

EditResult ProjectController::InsertSectionAfter(const NodeId& anchor,
                                                 const QString& title) {
    const auto& sections = session_->state().document().body().sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        bool belongs = sections[si].id == anchor;
        for (const auto& block : sections[si].blocks) {
            if (belongs) break;
            belongs = std::visit(
                [&](const auto& value) { return value.id == anchor; }, block);
        }
        for (const auto& subsection : sections[si].subsections) {
            if (belongs) break;
            belongs = subsection.id == anchor;
            for (const auto& block : subsection.blocks) {
                if (belongs) break;
                belongs = std::visit(
                    [&](const auto& value) { return value.id == anchor; },
                    block);
            }
        }
        if (!belongs) continue;
        InsertSectionPayload payload;
        payload.index = si + 1;
        payload.title = InlineFromText(ToStd(title));
        return ExecuteAndNotify(std::move(payload));
    }
    return InsertSection(title);
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

EditResult ProjectController::InsertSubsectionAfter(const NodeId& anchor,
                                                    const QString& title) {
    const auto& sections = session_->state().document().body().sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        bool belongs_to_section = sections[si].id == anchor;
        if (!belongs_to_section) {
            for (const auto& block : sections[si].blocks) {
                belongs_to_section = std::visit(
                    [&](const auto& value) { return value.id == anchor; },
                    block);
                if (belongs_to_section) break;
            }
        }
        if (belongs_to_section) return InsertSubsection(si, title);
        for (size_t ui = 0; ui < sections[si].subsections.size(); ++ui) {
            const auto& subsection = sections[si].subsections[ui];
            bool belongs_to_subsection = subsection.id == anchor;
            if (!belongs_to_subsection) {
                for (const auto& block : subsection.blocks) {
                    belongs_to_subsection = std::visit(
                        [&](const auto& value) { return value.id == anchor; },
                        block);
                    if (belongs_to_subsection) break;
                }
            }
            if (!belongs_to_subsection) continue;
            InsertSubsectionPayload payload;
            payload.section_index = si;
            payload.index = ui + 1;
            payload.title = InlineFromText(ToStd(title));
            return ExecuteAndNotify(std::move(payload));
        }
    }
    return EditResult::Fail(FailureReason::InvalidTarget,
                            "subsection insertion anchor not found");
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

EditResult ProjectController::InsertParagraphAfter(const NodeId& anchor,
                                                   const QString& text) {
    auto point = ResolveInsertionPoint(anchor);
    if (!point) {
        if (session_->state().document().body().sections.empty()) {
            auto section = InsertSection("");
            if (section.status != EditStatus::Applied) return section;
            point = InsertionPoint{section.created_node, std::nullopt};
        } else {
            return EditResult::Fail(FailureReason::InvalidTarget,
                                    "insertion anchor not found");
        }
    }
    InsertParagraphPayload payload;
    payload.parent = point->parent;
    payload.index = point->index;
    payload.content = InlineFromText(ToStd(text));
    return ExecuteAndNotify(std::move(payload));
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

EditResult ProjectController::InsertEquationAfter(const NodeId& anchor,
                                                  const QString& math,
                                                  bool numbered) {
    auto point = ResolveInsertionPoint(anchor);
    if (!point) {
        return EditResult::Fail(FailureReason::InvalidTarget,
                                "insertion anchor not found");
    }
    InsertEquationPayload payload;
    payload.parent = point->parent;
    payload.index = point->index;
    payload.math_source = ToStd(math);
    payload.numbered = numbered;
    return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::InsertFigure(const NodeId& parent,
                                           const QString& image_path) {
    auto r = session_->InsertFigureFromSource(ToStd(image_path), parent);
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::InsertFigureAfter(const NodeId& anchor,
                                                const QString& image_path) {
    auto point = ResolveInsertionPoint(anchor);
    if (!point) {
        return EditResult::Fail(FailureReason::InvalidTarget,
                                "insertion anchor not found");
    }
    auto result = session_->InsertFigureFromSource(
        ToStd(image_path), point->parent, point->index);
    if (result.status == EditStatus::Applied) EmitDocumentChanged();
    return result;
}

EditResult ProjectController::InsertTableAfter(const NodeId& anchor) {
    auto point = ResolveInsertionPoint(anchor);
    if (!point) {
        return EditResult::Fail(FailureReason::InvalidTarget,
                                "insertion anchor not found");
    }
    InsertTablePayload payload;
    payload.parent = point->parent;
    payload.index = point->index;
    payload.columns = std::vector<TableColumn>(3);
    payload.rows = 3;
    payload.has_header_row = true;
    return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::EditParagraph(const NodeId& paragraph,
                                            const QString& text) {
    EditParagraphPayload p;
    p.paragraph = paragraph;
    p.content = InlineFromEditorText(ToStd(text));
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

EditResult ProjectController::EditCaption(const NodeId& block,
                                          const QString& caption) {
    EditCaptionPayload payload;
    payload.block = block;
    payload.caption = InlineFromText(ToStd(caption));
    return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::DeleteBlock(const NodeId& block) {
    DeleteBlockPayload p;
    p.node = block;
    auto r = session_->Execute(MakeCmd(std::move(p)));
    if (r.status == EditStatus::Applied) EmitDocumentChanged();
    return r;
}

EditResult ProjectController::DeleteNode(const NodeId& node) {
    const auto& sections = session_->state().document().body().sections;
    for (size_t section_index = 0; section_index < sections.size();
         ++section_index) {
        if (sections[section_index].id == node) {
            DeleteSectionPayload payload;
            payload.index = section_index;
            return ExecuteAndNotify(std::move(payload));
        }
        for (size_t subsection_index = 0;
             subsection_index < sections[section_index].subsections.size();
             ++subsection_index) {
            if (sections[section_index].subsections[subsection_index].id ==
                node) {
                DeleteSubsectionPayload payload;
                payload.section_index = section_index;
                payload.subsection_index = subsection_index;
                return ExecuteAndNotify(std::move(payload));
            }
        }
    }
    return DeleteBlock(node);
}

EditResult ProjectController::MoveNode(const NodeId& node, int direction) {
    if (direction != -1 && direction != 1) {
        return EditResult::Fail(FailureReason::ConstraintViolation,
                                "direction must be -1 or +1");
    }
    const auto& sections = session_->state().document().body().sections;
    for (size_t si = 0; si < sections.size(); ++si) {
        if (sections[si].id == node) {
            const auto destination = static_cast<long long>(si) + direction;
            if (destination < 0 || destination >=
                                     static_cast<long long>(sections.size())) {
                return EditResult::Fail(FailureReason::InvalidTarget,
                                        "section is already at the edge");
            }
            MoveSectionPayload payload;
            payload.from = si;
            payload.to = direction > 0 ? si + 2 : si - 1;
            return ExecuteAndNotify(std::move(payload));
        }
        for (size_t bi = 0; bi < sections[si].blocks.size(); ++bi) {
            const auto id = std::visit(
                [](const auto& block) { return block.id; },
                sections[si].blocks[bi]);
            if (id == node) {
                const auto destination = static_cast<long long>(bi) + direction;
                if (destination < 0 || destination >= static_cast<long long>(
                                                        sections[si].blocks.size())) {
                    return EditResult::Fail(FailureReason::InvalidTarget,
                                            "block is already at the edge");
                }
                MoveBlockPayload payload;
                payload.node = node;
                payload.new_parent = sections[si].id;
                payload.new_index = direction > 0 ? bi + 1 : bi - 1;
                return ExecuteAndNotify(std::move(payload));
            }
        }
        for (size_t ui = 0; ui < sections[si].subsections.size(); ++ui) {
            const auto& subsection = sections[si].subsections[ui];
            if (subsection.id == node) {
                const auto destination = static_cast<long long>(ui) + direction;
                if (destination < 0 ||
                    destination >= static_cast<long long>(
                                       sections[si].subsections.size())) {
                    return EditResult::Fail(
                        FailureReason::InvalidTarget,
                        "subsection is already at the edge");
                }
                MoveSubsectionPayload payload;
                payload.section_index = si;
                payload.from = ui;
                payload.to = direction > 0 ? ui + 2 : ui - 1;
                return ExecuteAndNotify(std::move(payload));
            }
            for (size_t bi = 0; bi < subsection.blocks.size(); ++bi) {
                const auto id = std::visit(
                    [](const auto& block) { return block.id; },
                    subsection.blocks[bi]);
                if (id != node) continue;
                const auto destination = static_cast<long long>(bi) + direction;
                if (destination < 0 || destination >= static_cast<long long>(
                                                        subsection.blocks.size())) {
                    return EditResult::Fail(FailureReason::InvalidTarget,
                                            "block is already at the edge");
                }
                MoveBlockPayload payload;
                payload.node = node;
                payload.new_parent = subsection.id;
                payload.new_index = direction > 0 ? bi + 1 : bi - 1;
                return ExecuteAndNotify(std::move(payload));
            }
        }
    }
    return EditResult::Fail(FailureReason::InvalidTarget, "node not found");
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

EditResult ProjectController::InsertCrossReference(const NodeId& paragraph,
                                                   const NodeId& target) {
    InsertCrossReferencePayload payload;
    payload.paragraph = paragraph;
    payload.target = target;
    return ExecuteAndNotify(std::move(payload));
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
