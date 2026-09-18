#include "app/ProjectController.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <QTimer>
#include <chrono>

#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string &s) { return QString::fromStdString(s); }
std::string ToStd(const QString &s) { return s.toStdString(); }

const char *ToString(PersistenceState state) {
  switch (state) {
  case PersistenceState::Clean:
    return "Clean";
  case PersistenceState::Dirty:
    return "Dirty";
  case PersistenceState::Saving:
    return "Saving";
  case PersistenceState::SaveFailed:
    return "SaveFailed";
  }
  return "?";
}
const char *ToString(PreviewState state) {
  switch (state) {
  case PreviewState::NoPreview:
    return "NoPreview";
  case PreviewState::Fresh:
    return "Fresh";
  case PreviewState::Stale:
    return "Stale";
  }
  return "?";
}
} // namespace

ProjectController::ProjectController(QObject *parent) : QObject(parent) {
  qRegisterMetaType<pf::PreviewUpdate>("pf::PreviewUpdate");
  qRegisterMetaType<pf::BuildResult>("pf::BuildResult");
  qRegisterMetaType<pf::BuildEvent>("pf::BuildEvent");

  ProjectSession::Config config;
  // The bundled portable TeX Live is the production environment (plan §3):
  // the app directory carries runtime/texlive, so no user TeX install is
  // needed and the user's PATH cannot influence a build.
  config.install_root = PF_INSTALL_ROOT;
  config.workspace_root =
      std::filesystem::temp_directory_path() / "paperforge-gui-builds";
  config.debounce = std::chrono::milliseconds{800};

  session_ = std::make_unique<ProjectSession>(config);

  // Background workers wake the application thread through this hook; the
  // queued invocation lands exactly where the mutable state lives.
  session_->SetWakeHandler([this]() {
    QMetaObject::invokeMethod(
        this, [this]() { PumpEvents(); }, Qt::QueuedConnection);
  });

  session_->SetPhaseHandler([this](BuildPhase, BuildPhase current) {
    QString text;
    switch (current) {
    case BuildPhase::Idle:
      text = "Idle";
      break;
    case BuildPhase::Debouncing:
      text = "Debouncing…";
      break;
    case BuildPhase::Rendering:
      text = "Rendering…";
      break;
    case BuildPhase::Compiling:
      text = "Compiling…";
      break;
    }
    emit buildStatusChanged(text);
  });
  // Runs on the application thread, and only for results that passed the
  // ProjectSession preview gate (project id + revision + build id). The
  // whole BuildResult travels: diagnostics keep their structure, so the
  // Problems panel never re-parses text (plan §37).
  session_->SetBuildResultHandler(
      [this](const BuildResult &result) { emit buildCompleted(result); });
  // Live build-log events of the current build (the session already dropped
  // any event whose build id went stale).
  session_->SetBuildEventHandler(
      [this](const BuildEvent &event) { emit buildEvent(event); });
  session_->SetPreviewUpdateHandler(
      [this](const PreviewUpdate &update) { emit previewUpdated(update); });
  session_->SetSaveResultHandler([this](const SaveResult &result, SaveKind) {
    // P0-06: the bool says whether the write landed (Saved) or was replaced
    // by a newer save (Superseded) - a superseded save is not an error the
    // user must worry about.
    const bool landed = result.status == SaveResult::Status::Ok;
    emit saveFinished(landed, ToQ(result.detail));
    // The state label always re-derives from the session's authoritative
    // state; a superseded revision never shows "Saved".
    EmitProjectStateChanged();
  });

  // Backstop: the wake handler already delivers events promptly, so this is
  // only a safety net against a missed wake-up. Kept slow enough not to spin
  // the event loop when the app is idle.
  pump_timer_ = new QTimer(this);
  pump_timer_->setInterval(100);
  connect(pump_timer_, &QTimer::timeout, this, [this]() { PumpEvents(); });
  pump_timer_->start();
}

ProjectController::~ProjectController() {
  shutting_down_ = true;
  if (pump_timer_)
    pump_timer_->stop();
  session_.reset();
}

void ProjectController::PumpEvents() {
  if (shutting_down_ || !session_)
    return;
  session_->ProcessApplicationEvents();
}

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
  if (result.status == EditStatus::Applied)
    EmitDocumentChanged();
  return result;
}

std::optional<ProjectController::InsertionPoint>
ProjectController::ResolveInsertionPoint(const NodeId &anchor) const {
  const auto &sections = session_->state().document().body().sections;
  if (anchor.empty()) {
    if (sections.empty())
      return std::nullopt;
    return InsertionPoint{sections.back().id, std::nullopt};
  }
  for (const auto &section : sections) {
    if (section.id == anchor) {
      return InsertionPoint{section.id, size_t{0}};
    }
    for (size_t i = 0; i < section.blocks.size(); ++i) {
      const NodeId id = std::visit([](const auto &block) { return block.id; },
                                   section.blocks[i]);
      if (id == anchor)
        return InsertionPoint{section.id, i + 1};
    }
    for (const auto &subsection : section.subsections) {
      if (subsection.id == anchor) {
        return InsertionPoint{subsection.id, size_t{0}};
      }
      for (size_t i = 0; i < subsection.blocks.size(); ++i) {
        const NodeId id = std::visit([](const auto &block) { return block.id; },
                                     subsection.blocks[i]);
        if (id == anchor) {
          return InsertionPoint{subsection.id, i + 1};
        }
      }
    }
  }
  return std::nullopt;
}

namespace {

// 1-based institution ordinal encoded by a superscript character: the
// Institution row renders superscripts, so marker N means slot N.
int SuperscriptOrdinal(QChar ch) {
  switch (ch.unicode()) {
  case 0x00B9:
    return 1;
  case 0x00B2:
    return 2;
  case 0x00B3:
    return 3;
  case 0x2070:
    return 10; // superscript zero: tenth institution
  default:
    break;
  }
  if (ch.unicode() >= 0x2074 && ch.unicode() <= 0x2079) {
    return ch.unicode() - 0x2074 + 4; // superscripts four..nine
  }
  return 0;
}

} // namespace

bool ProjectController::NewProject(const QString &dir, std::string *error) {
  std::string local_error;
  bool ok = session_->NewProject(ToStd(dir), &local_error);
  if (!ok && error)
    *error = local_error;
  if (ok)
    emit documentChanged();
  return ok;
}

bool ProjectController::OpenProject(const QString &dir) {
  std::string error;
  bool ok = session_->OpenProject(ToStd(dir), &error);
  if (ok) {
    emit documentChanged();
    emit templateChanged(ToQ(session_->state().template_selection()));
  }
  return ok;
}

bool ProjectController::OpenProjectWithRecovery(const QString &dir,
                                                bool *recovered) {
  std::string error;
  bool ok = session_->OpenProjectWithRecovery(ToStd(dir), &error, recovered);
  if (ok)
    emit documentChanged();
  return ok;
}

void ProjectController::CloseProject() { session_->CloseProject(); }

void ProjectController::Save() { session_->Save(); }

void ProjectController::FlushSaves() {
  if (session_)
    session_->FlushSaves();
}

void ProjectController::StartAutosave() { session_->StartAutosaveTimer(); }

void ProjectController::StopAutosave() { session_->StopAutosaveTimer(); }

void ProjectController::EmitDocumentChanged() {
  emit documentChanged();
  EmitProjectStateChanged();
}

void ProjectController::EmitProjectStateChanged() {
  // P0-06: the only source of the UI's save/preview/revision state.
  emit stateChanged(ToString(session_->persistence_state()),
                    ToString(session_->preview_state()),
                    QString::number(session_->current_revision().value));
}

EditResult ProjectController::SetTitle(const QString &text) {
  SetTitlePayload p;
  p.title = InlineFromText(ToStd(text));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::SetAbstract(const QString &text) {
  SetAbstractPayload p;
  if (text.trimmed().isEmpty()) {
    p.abstract_text = std::nullopt;
  } else {
    p.abstract_text = InlineFromText(ToStd(text));
  }
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::SetAuthorsText(const QString &comma_separated) {
  // Parse "Alice, Bob, Carol" into author entries. Superscript markers keep
  // their institution binding, so editing names never loses the link.
  QStringList names = comma_separated.split(
      QRegularExpression(QStringLiteral(R"([,\n\x{00B7}]+)")),
      Qt::SkipEmptyParts);
  std::vector<Author> authors;
  const auto &affiliations =
      session_->state().document().front_matter().affiliations;
  for (const auto &raw : names) {
    Author author;
    QString name = raw.trimmed();
    // Superscript markers after a name select institutions by their
    // 1-based position in the Institution row: "Alice\u00b9, Bob\u00b2".
    static const QRegularExpression markers(QStringLiteral(
        R"(([\x{00B9}\x{00B2}\x{00B3}\x{2070}-\x{209F}]+)\s*$)"));
    const auto match = markers.match(name);
    if (match.hasMatch()) {
      for (const QChar &marker : match.captured(1)) {
        const int ordinal = SuperscriptOrdinal(marker);
        if (ordinal >= 1 &&
            static_cast<size_t>(ordinal) <= affiliations.size()) {
          author.affiliations.push_back(
              affiliations[static_cast<size_t>(ordinal) - 1].id);
        }
      }
      name.remove(match.capturedStart(0), match.capturedLength(0));
    }
    author.name = ToStd(name.trimmed());
    // With exactly one institution and no markers there is nothing to
    // disambiguate, so everyone belongs to it.
    if (author.affiliations.empty() && affiliations.size() == 1) {
      author.affiliations.push_back(affiliations.front().id);
    }
    authors.push_back(std::move(author));
  }

  // Replace the author list via Remove/Add payloads (undoable snapshot
  // history captures both).
  const auto &current = session_->state().document().front_matter().authors;
  for (size_t i = current.size(); i > 0; --i) {
    RemoveAuthorPayload p;
    p.index = i - 1;
    session_->Execute(MakeCmd(std::move(p)));
  }
  for (auto &author : authors) {
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

EditResult ProjectController::SetAuthorAffiliation(
    size_t author_index, const AffiliationId &affiliation, bool linked) {
  const auto &authors = session_->state().document().front_matter().authors;
  if (author_index >= authors.size()) {
    return EditResult::Fail(FailureReason::InvalidTarget,
                            "author index out of range");
  }
  Author updated = authors[author_index];
  std::vector<AffiliationId> links;
  for (const auto &id : updated.affiliations) {
    if (!(id == affiliation))
      links.push_back(id);
  }
  if (linked) {
    // Keep the document's institution order so the superscripts read 1,2.
    const auto &all = session_->state().document().front_matter().affiliations;
    std::vector<AffiliationId> ordered;
    for (const auto &candidate : all) {
      if (candidate.id == affiliation)
        ordered.push_back(candidate.id);
      for (const auto &existing : links) {
        if (existing == candidate.id)
          ordered.push_back(existing);
      }
    }
    if (ordered.size() != links.size() + 1) {
      return EditResult::Fail(FailureReason::InvalidTarget,
                              "unknown institution");
    }
    links = std::move(ordered);
  }
  updated.affiliations = std::move(links);
  UpdateAuthorPayload payload;
  payload.index = author_index;
  payload.author = std::move(updated);
  auto result = ExecuteAndNotify(std::move(payload));
  if (result.status == EditStatus::Applied)
    EmitDocumentChanged();
  return result;
}

EditResult
ProjectController::SetAffiliationsText(const QString &semicolon_separated) {
  SetAffiliationsPayload p;
  QStringList names = semicolon_separated.split(
      QRegularExpression(QStringLiteral(R"([;\n]+)")), Qt::SkipEmptyParts);
  const auto &existing =
      session_->state().document().front_matter().affiliations;
  size_t slot = 0;
  for (const auto &raw : names) {
    QString name = raw.trimmed();
    // Drop the leading ordinal marker so "1 University" and the
    // superscript form both parse to the same institution.
    static const QRegularExpression leading(
        QStringLiteral(R"(^[\x{00B9}\x{00B2}\x{00B3}\x{2070}-\x{209F}]+\s*)"));
    name.remove(leading);
    if (name.isEmpty())
      continue;
    Affiliation aff;
    // Reuse the id already occupying this slot. Affiliation ids are what
    // authors reference, so minting a fresh one per keystroke would
    // silently detach every author from its institution.
    aff.id = slot < existing.size()
                 ? existing[slot].id
                 : AffiliationId(IdGenerator::NewAffiliationId());
    aff.name = ToStd(name);
    p.affiliations.push_back(std::move(aff));
    ++slot;
  }
  auto r = session_->Execute(MakeCmd(std::move(p)));
  // EditingSystem prunes author links that no longer resolve.
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::SetKeywordsText(const QString &comma_separated) {
  SetKeywordsPayload p;
  QStringList names = comma_separated.split(',', Qt::SkipEmptyParts);
  for (const auto &raw : names) {
    QString kw = raw.trimmed();
    if (!kw.isEmpty())
      p.keywords.push_back(ToStd(kw));
  }
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertSection(const QString &title) {
  InsertSectionPayload p;
  p.index = session_->state().document().body().sections.size();
  p.title = InlineFromText(ToStd(title));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertSectionAfter(const NodeId &anchor,
                                                 const QString &title) {
  const auto &sections = session_->state().document().body().sections;
  for (size_t si = 0; si < sections.size(); ++si) {
    bool belongs = sections[si].id == anchor;
    for (const auto &block : sections[si].blocks) {
      if (belongs)
        break;
      belongs = std::visit(
          [&](const auto &value) { return value.id == anchor; }, block);
    }
    for (const auto &subsection : sections[si].subsections) {
      if (belongs)
        break;
      belongs = subsection.id == anchor;
      for (const auto &block : subsection.blocks) {
        if (belongs)
          break;
        belongs = std::visit(
            [&](const auto &value) { return value.id == anchor; }, block);
      }
    }
    if (!belongs)
      continue;
    InsertSectionPayload payload;
    payload.index = si + 1;
    payload.title = InlineFromText(ToStd(title));
    return ExecuteAndNotify(std::move(payload));
  }
  return InsertSection(title);
}

EditResult ProjectController::InsertSubsection(size_t section_index,
                                               const QString &title) {
  InsertSubsectionPayload p;
  p.section_index = section_index;
  p.index = session_->state()
                .document()
                .body()
                .sections[section_index]
                .subsections.size();
  p.title = InlineFromText(ToStd(title));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertSubsectionAfter(const NodeId &anchor,
                                                    const QString &title) {
  // The core resolves the anchor and, for a block, hands the following
  // blocks to the new subsection so the heading lands where it was asked
  // for rather than at the end of the section.
  InsertSubsectionAfterPayload payload;
  payload.after = anchor;
  payload.title = InlineFromText(ToStd(title));
  return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::InsertSubsubsection(size_t section_index,
                                                  size_t subsection_index,
                                                  const QString &title) {
  InsertSubsubsectionPayload p;
  p.section_index = section_index;
  p.subsection_index = subsection_index;
  const auto &subsubsections = session_->state()
                                   .document()
                                   .body()
                                   .sections[section_index]
                                   .subsections[subsection_index]
                                   .subsubsections;
  p.index = subsubsections.size();
  p.title = InlineFromText(ToStd(title));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertSubsubsectionAfter(const NodeId &anchor,
                                                       const QString &title) {
  InsertSubsubsectionAfterPayload payload;
  payload.after = anchor;
  payload.title = InlineFromText(ToStd(title));
  return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::RenameSubsubsection(const NodeId &subsubsection,
                                                  const QString &title) {
  RenameSubsubsectionPayload p;
  p.subsubsection = subsubsection;
  p.title = InlineFromText(ToStd(title));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::DeleteSubsubsection(const NodeId &subsubsection) {
  auto address = LocateNode(session_->state().document(), subsubsection);
  if (!address || address->kind != NodeKind::Subsubsection) {
    return EditResult::Fail(FailureReason::InvalidTarget,
                            "subsubsection not found");
  }
  DeleteSubsubsectionPayload p;
  p.section_index = *address->section;
  p.subsection_index = *address->subsection;
  p.subsubsection_index = *address->subsubsection;
  return ExecuteAndNotify(std::move(p));
}

EditResult ProjectController::InsertParagraph(const NodeId &parent,
                                              const QString &text) {
  InsertParagraphPayload p;
  p.parent = parent;
  p.content = InlineFromText(ToStd(text));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertParagraphAfter(const NodeId &anchor,
                                                   const QString &text) {
  auto point = ResolveInsertionPoint(anchor);
  if (!point) {
    if (session_->state().document().body().sections.empty()) {
      auto section = InsertSection("");
      if (section.status != EditStatus::Applied)
        return section;
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

EditResult ProjectController::InsertEquation(const NodeId &parent,
                                             const QString &math, bool numbered,
                                             const QString &label) {
  InsertEquationPayload p;
  p.parent = parent;
  p.latex = ToStd(math);
  p.numbered = numbered;
  p.label = ToStd(label);
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertEquationAfter(const NodeId &anchor,
                                                  const QString &math,
                                                  bool numbered,
                                                  const QString &label) {
  auto point = ResolveInsertionPoint(anchor);
  if (!point) {
    return EditResult::Fail(FailureReason::InvalidTarget,
                            "insertion anchor not found");
  }
  InsertEquationPayload payload;
  payload.parent = point->parent;
  payload.index = point->index;
  payload.latex = ToStd(math);
  payload.numbered = numbered;
  payload.label = ToStd(label);
  return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::InsertFigure(const NodeId &parent,
                                           const QString &image_path) {
  auto r = session_->InsertFigureFromSource(ToStd(image_path), parent);
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::InsertFigureAfter(const NodeId &anchor,
                                                const QString &image_path) {
  auto point = ResolveInsertionPoint(anchor);
  if (!point) {
    return EditResult::Fail(FailureReason::InvalidTarget,
                            "insertion anchor not found");
  }
  auto result = session_->InsertFigureFromSource(ToStd(image_path),
                                                 point->parent, point->index);
  if (result.status == EditStatus::Applied)
    EmitDocumentChanged();
  return result;
}

EditResult ProjectController::InsertTableAfter(const NodeId &anchor) {
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

EditResult ProjectController::EditParagraphRich(const NodeId &paragraph,
                                                const InlineContent &content) {
  EditParagraphPayload p;
  p.paragraph = paragraph;
  p.content = content; // already structured; no string round trip
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::EditEquation(const NodeId &equation,
                                           const QString &math, bool numbered,
                                           const QString &label) {
  EditEquationPayload p;
  p.equation = equation;
  p.latex = ToStd(math);
  p.numbered = numbered;
  p.label = ToStd(label);
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::RenameSection(const NodeId &section,
                                            const QString &title) {
  RenameSectionPayload p;
  p.section = section;
  p.title = InlineFromText(ToStd(title));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::RenameSubsection(const NodeId &subsection,
                                               const QString &title) {
  // V1: rename via section rename payload on the subsection node.
  RenameSubsectionPayload p;
  p.subsection = subsection;
  p.title = InlineFromText(ToStd(title));
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::EditCaption(const NodeId &block,
                                          const QString &caption) {
  EditCaptionPayload payload;
  payload.block = block;
  payload.caption = InlineFromText(ToStd(caption));
  return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::EditFigureSpan(const NodeId &figure,
                                             bool double_column) {
  EditFigureSpanPayload payload;
  payload.figure = figure;
  payload.span =
      double_column ? FigureSpan::DoubleColumn : FigureSpan::SingleColumn;
  return ExecuteAndNotify(std::move(payload));
}

EditResult ProjectController::DeleteBlock(const NodeId &block) {
  DeleteBlockPayload p;
  p.node = block;
  auto r = session_->Execute(MakeCmd(std::move(p)));
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

EditResult ProjectController::DeleteNode(const NodeId &node) {
  // One traversal answers "what kind of node is this, and where" for every
  // branch below.
  auto address = LocateNode(session_->state().document(), node);
  if (!address)
    return DeleteBlock(node);
  if (address->kind == NodeKind::Section) {
    DeleteSectionPayload payload;
    payload.index = *address->section;
    return ExecuteAndNotify(std::move(payload));
  }
  if (address->kind == NodeKind::Subsection) {
    DeleteSubsectionPayload payload;
    payload.section_index = *address->section;
    payload.subsection_index = *address->subsection;
    return ExecuteAndNotify(std::move(payload));
  }
  if (address->kind == NodeKind::Subsubsection) {
    DeleteSubsubsectionPayload payload;
    payload.section_index = *address->section;
    payload.subsection_index = *address->subsection;
    payload.subsubsection_index = *address->subsubsection;
    return ExecuteAndNotify(std::move(payload));
  }
  return DeleteBlock(node);
}

EditResult ProjectController::MoveNodeAfter(const NodeId &node,
                                            const NodeId &anchor) {
  if (node == anchor) {
    return EditResult::Fail(FailureReason::InvalidTarget, "already in place");
  }
  const Document &doc = session_->state().document();
  const auto source = LocateNode(doc, node);
  const auto target = LocateNode(doc, anchor);
  if (!source || !target) {
    return EditResult::Fail(FailureReason::InvalidTarget,
                            "move target not found");
  }

  // ---- A block: drop it into the anchor's container, after the anchor. ----
  if (source->block) {
    // A block anchor means "right after me, in my container"; a heading
    // anchor means "at the start of that heading's own block list".
    NodeId parent;
    std::optional<size_t> index;
    const NodeId *anchor_parent = nullptr;
    if (target->block) {
      switch (target->kind) {
      case NodeKind::Subsubsection:
        anchor_parent = &doc.body()
                             .sections[*target->section]
                             .subsections[*target->subsection]
                             .subsubsections[*target->subsubsection]
                             .id;
        break;
      case NodeKind::Subsection:
        anchor_parent = &doc.body()
                             .sections[*target->section]
                             .subsections[*target->subsection]
                             .id;
        break;
      default:
        anchor_parent = &doc.body().sections[*target->section].id;
        break;
      }
      index = *target->block + 1;
      // Moving an earlier block out of the same list shifts the anchor.
      if (source->section == target->section &&
          source->subsection == target->subsection &&
          source->subsubsection == target->subsubsection &&
          *source->block < *target->block) {
        --*index;
      }
    } else {
      // Anchor is a heading: its own blocks are the destination.
      switch (target->kind) {
      case NodeKind::Subsubsection:
        anchor_parent = &doc.body()
                             .sections[*target->section]
                             .subsections[*target->subsection]
                             .subsubsections[*target->subsubsection]
                             .id;
        break;
      case NodeKind::Subsection:
        anchor_parent = &doc.body()
                             .sections[*target->section]
                             .subsections[*target->subsection]
                             .id;
        break;
      case NodeKind::Section:
        anchor_parent = &doc.body().sections[*target->section].id;
        break;
      default:
        return EditResult::Fail(FailureReason::InvalidTarget,
                                "move target not found");
      }
      index = 0;
    }
    MoveBlockPayload payload;
    payload.node = node;
    payload.new_parent = *anchor_parent;
    payload.new_index = index;
    return ExecuteAndNotify(std::move(payload));
  }

  // ---- A subsubsection moves inside its own subsection. ----
  if (source->kind == NodeKind::Subsubsection) {
    if (source->section != target->section ||
        source->subsection != target->subsection) {
      return EditResult::Fail(
          FailureReason::InvalidTarget,
          "a subsubsection can only be reordered inside its subsection");
    }
    const auto &subsubsections = doc.body()
                                     .sections[*source->section]
                                     .subsections[*source->subsection]
                                     .subsubsections;
    // After a sibling subsubsection: right behind it. Otherwise append.
    const size_t to = target->kind == NodeKind::Subsubsection
                          ? *target->subsubsection + 1
                          : subsubsections.size();
    MoveSubsubsectionPayload payload;
    payload.section_index = *source->section;
    payload.subsection_index = *source->subsection;
    payload.from = *source->subsubsection;
    payload.to = to;
    return ExecuteAndNotify(std::move(payload));
  }

  // ---- A subsection moves inside its own section. ----
  if (source->kind == NodeKind::Subsection) {
    if (source->section != target->section) {
      return EditResult::Fail(
          FailureReason::InvalidTarget,
          "a subsection can only be reordered inside its section");
    }
    // MoveSubsection takes an insert-before index in the original list.
    // After a subsection: right behind it. After a block of the section
    // itself: the first subsection slot, which is the earliest the model
    // can render. After the section title: append.
    const size_t to =
        target->kind == NodeKind::Subsection
            ? *target->subsection + 1
            : doc.body().sections[*source->section].subsections.size();
    MoveSubsectionPayload payload;
    payload.section_index = *source->section;
    payload.from = *source->subsection;
    payload.to = to;
    return ExecuteAndNotify(std::move(payload));
  }

  // ---- A section moves among sections. ----
  if (*source->section == *target->section) {
    return EditResult::Fail(FailureReason::InvalidTarget, "already in place");
  }
  MoveSectionPayload payload;
  payload.from = *source->section;
  payload.to = *target->section + 1;
  return ExecuteAndNotify(std::move(payload));
}
EditResult ProjectController::MoveNode(const NodeId &node, int direction) {
  if (direction != -1 && direction != 1) {
    return EditResult::Fail(FailureReason::ConstraintViolation,
                            "direction must be -1 or +1");
  }
  const auto &sections = session_->state().document().body().sections;
  for (size_t si = 0; si < sections.size(); ++si) {
    if (sections[si].id == node) {
      const auto destination = static_cast<long long>(si) + direction;
      if (destination < 0 ||
          destination >= static_cast<long long>(sections.size())) {
        return EditResult::Fail(FailureReason::InvalidTarget,
                                "section is already at the edge");
      }
      MoveSectionPayload payload;
      payload.from = si;
      payload.to = direction > 0 ? si + 2 : si - 1;
      return ExecuteAndNotify(std::move(payload));
    }
    for (size_t bi = 0; bi < sections[si].blocks.size(); ++bi) {
      const auto id = std::visit([](const auto &block) { return block.id; },
                                 sections[si].blocks[bi]);
      if (id == node) {
        const auto destination = static_cast<long long>(bi) + direction;
        if (destination < 0 ||
            destination >= static_cast<long long>(sections[si].blocks.size())) {
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
      const auto &subsection = sections[si].subsections[ui];
      if (subsection.id == node) {
        const auto destination = static_cast<long long>(ui) + direction;
        if (destination < 0 ||
            destination >=
                static_cast<long long>(sections[si].subsections.size())) {
          return EditResult::Fail(FailureReason::InvalidTarget,
                                  "subsection is already at the edge");
        }
        MoveSubsectionPayload payload;
        payload.section_index = si;
        payload.from = ui;
        payload.to = direction > 0 ? ui + 2 : ui - 1;
        return ExecuteAndNotify(std::move(payload));
      }
      for (size_t bi = 0; bi < subsection.blocks.size(); ++bi) {
        const auto id = std::visit([](const auto &block) { return block.id; },
                                   subsection.blocks[bi]);
        if (id != node)
          continue;
        const auto destination = static_cast<long long>(bi) + direction;
        if (destination < 0 ||
            destination >= static_cast<long long>(subsection.blocks.size())) {
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
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
  return r;
}

// The old GUI-side InsertCitation / InsertCrossReference helpers were removed
// (citation plan §5): citations now enter the document exclusively through
// InlineEditor::InsertCitationObject -> ParagraphContentEdited ->
// EditParagraphRich, so the picker commit and the document can never diverge.

void ProjectController::Undo() {
  auto r = session_->Undo();
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
}

void ProjectController::Redo() {
  auto r = session_->Redo();
  if (r.status == EditStatus::Applied)
    EmitDocumentChanged();
}

void ProjectController::ChangeTemplate(const QString &template_id) {
  session_->ChangeTemplate(ToStd(template_id));
  emit templateChanged(template_id);
  EmitDocumentChanged();
}

void ProjectController::RequestBuild(bool manual) {
  session_->RequestBuild(manual);
}

void ProjectController::CancelBuild() { session_->CancelBuild(); }

BibliographyImportResult
ProjectController::ImportBibliographyText(const QString &bibtex) {
  auto r = session_->ImportBibliography(ToStd(bibtex));
  if (r.status == BibliographyImportResult::Status::Ok) {
    // The bibliography is a project resource: ProjectSession persists it
    // atomically into <project>/references.bib on a successful import, so
    // an import failure can never destroy the previous file (citation
    // plan §7).
    EmitDocumentChanged();
  }
  return r;
}

CitationSearchResult
ProjectController::SearchCitations(const QString &query) const {
  return session_->SearchCitations(ToStd(query));
}

std::shared_ptr<const pf::CitationNumberResolver>
ProjectController::CitationNumbers() const {
  if (!session_)
    return nullptr;
  return std::make_shared<const pf::CitationNumberResolver>(
      pf::CitationNumberResolver::Build(session_->state().document(),
                                        session_->bibliography()));
}

} // namespace pf::gui
