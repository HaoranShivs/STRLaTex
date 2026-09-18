#include "app/MainWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>

#include "app/BlockEditor.h"
#include "app/OutlinePanel.h"
#include "app/PdfPreview.h"
#include "app/ProblemsPanel.h"
#include "app/Theme.h"
#include "app/WelcomePage.h"
#include "document/InlineText.h"
#include "template/TemplateRegistry.h"

namespace pf::gui {

namespace {
QString ToQ(const std::string &s) { return QString::fromStdString(s); }
constexpr const char *kSettingsKey = "recent_projects";

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  // GUI typography lives in Theme.h; the app font is the chrome baseline
  // and no stylesheet pins a font-size over per-widget setFont() (UI plan
  // §7). Set it before any widget exists so all inherit it.
  QApplication::setFont(theme::AppFont());

  controller_ = new ProjectController(this);

  QSettings settings;
  recent_projects_ = settings.value(kSettingsKey).toStringList();

  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: before BuildUi\n");
  BuildUi();
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: after BuildUi\n");
  BuildMenus();
  WireEditor();
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: after WireEditor\n");

  connect(controller_, &ProjectController::documentChanged, this,
          &MainWindow::RefreshDocumentView);
  connect(editor_, &BlockEditor::RowCommitted, this, [this]() {
    if (pending_structural_refresh_)
      RefreshDocumentView();
  });
  connect(controller_, &ProjectController::previewUpdated, this,
          &MainWindow::OnPreviewUpdated);
  connect(controller_, &ProjectController::saveFinished, this,
          &MainWindow::OnSaveFinished);
  // P0-06: the authoritative persistence/preview/revision projection drives
  // the state label; nothing in the UI keeps a second dirty flag.
  connect(controller_, &ProjectController::stateChanged, this,
          &MainWindow::RenderProjectState);
  // Problems and Build Log consume the structured pipeline directly
  // (Build Diagnostics plan §37): MainWindow wires the components, it never
  // parses logs, computes source mappings or creates Diagnostics.
  connect(controller_, &ProjectController::buildCompleted, this,
          &MainWindow::OnBuildCompleted);
  connect(controller_, &ProjectController::buildEvent, this,
          &MainWindow::OnBuildEvent);
  connect(problems_, &ProblemsPanel::DiagnosticActivated, this,
          [this](const pf::Diagnostic &diagnostic) {
            OnProblemActivated(diagnostic);
          });
  connect(controller_, &ProjectController::buildStatusChanged, this,
          &MainWindow::OnBuildStatusChanged);

  setWindowTitle("PaperForge");
  resize(1500, 920);
  ShowWorkspace(false);
}

// ---------------- UI construction ----------------

void MainWindow::BuildUi() {
  central_stack_ = new QStackedWidget(this);
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: stack created\n");

  welcome_ = new WelcomePage(central_stack_);
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: welcome created\n");
  connect(welcome_, &WelcomePage::NewProjectRequested, this,
          &MainWindow::OnNewProject);
  connect(welcome_, &WelcomePage::OpenProjectRequested, this,
          &MainWindow::OnOpenProject);
  connect(welcome_, &WelcomePage::RecentActivated, this, [this](QString path) {
    bool recovered = false;
    if (controller_->OpenProjectWithRecovery(path, &recovered)) {
      controller_->StartAutosave();
      ShowWorkspace(true);
      statusBar()->showMessage("Opened project: " + path);
    }
  });
  for (const QString &path : recent_projects_) {
    welcome_->AddRecent(QFileInfo(path).fileName(), path);
  }
  central_stack_->addWidget(welcome_);

  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: welcome wired\n");
  workspace_ = new QWidget(central_stack_);
  auto *workspace_layout = new QVBoxLayout(workspace_);
  workspace_layout->setContentsMargins(0, 0, 0, 0);
  workspace_layout->setSpacing(0);
  workspace_layout->addWidget(BuildHeader());

  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: workspace created\n");
  main_splitter_ = new QSplitter(Qt::Horizontal, workspace_);
  outline_ = new OutlinePanel(main_splitter_);
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: outline created\n");
  editor_ = new BlockEditor(main_splitter_);
  editor_->SetAssetPathResolver([this](const AssetId &id) {
    if (!controller_->has_project())
      return QString();
    const auto *metadata = controller_->session().assets().registry().Find(id);
    if (!metadata)
      return QString();
    return ToQ(
        (controller_->session().paths().assets_dir / metadata->relative_path)
            .string());
  });
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: editor created\n");

  vertical_splitter_ = new QSplitter(Qt::Vertical, main_splitter_);
  preview_ = new PdfPreview(vertical_splitter_);
  preview_container_ = preview_;
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: preview created\n");
  problems_ = new ProblemsPanel(vertical_splitter_);
  if (!qEnvironmentVariable("PF_TRACE").isEmpty())
    fprintf(stderr, "TRACE: problems created\n");
  vertical_splitter_->addWidget(preview_container_);
  vertical_splitter_->addWidget(problems_);
  vertical_splitter_->setStretchFactor(0, 3);
  vertical_splitter_->setStretchFactor(1, 1);
  vertical_splitter_->setSizes({420, 200});

  main_splitter_->addWidget(outline_);
  main_splitter_->addWidget(editor_);
  main_splitter_->addWidget(vertical_splitter_);
  main_splitter_->setSizes({270, 780, 450});
  main_splitter_->setCollapsible(1, false); // the editor never collapses

  workspace_layout->addWidget(main_splitter_, 1);
  central_stack_->addWidget(workspace_);

  setCentralWidget(central_stack_);
  setStyleSheet(theme::AppStyleSheet());

  save_state_label_ = new QLabel("Saved", this);
  build_state_label_ = new QLabel("Build: Ready", this);
  word_count_label_ = new QLabel("0 words", this);
  statusBar()->addWidget(save_state_label_);
  statusBar()->addWidget(build_state_label_);
  statusBar()->addPermanentWidget(word_count_label_);
}

QWidget *MainWindow::BuildHeader() {
  auto *header = new QWidget(workspace_);
  header->setObjectName("headerBar");
  header->setFixedHeight(46);
  header->setStyleSheet(
      QString(
          "QWidget#headerBar { background: %1; border-bottom: 1px solid %2; }")
          .arg(theme::kEditorBackground, theme::kDivider));
  auto *layout = new QHBoxLayout(header);
  layout->setContentsMargins(16, 6, 16, 6);

  auto *logo = new QLabel("PaperForge", header);
  logo->setStyleSheet(
      QString("font-weight: 700; color: %1;").arg(theme::kPrimaryText));
  layout->addWidget(logo);
  layout->addSpacing(12);

  template_combo_ = new QComboBox(header);
  for (const auto &def : TemplateRegistry::Instance().All()) {
    template_combo_->addItem(ToQ(def.name), ToQ(def.id));
  }
  template_combo_->setStyleSheet(
      QString("QComboBox { border: none; background: transparent; color: %1; }")
          .arg(theme::kSecondaryText));
  connect(template_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
    if (controller_->has_project()) {
      OnChangeTemplate(template_combo_->currentData().toString());
    }
  });
  layout->addWidget(template_combo_);
  layout->addStretch(1);

  // Focus mode toggle (UI plan §11): a quiet text control, never louder
  // than the Build button, that hides the side panels for long writing.
  focus_button_ = new QPushButton(QStringLiteral("⤢  Focus"), header);
  focus_button_->setObjectName("focusToggle");
  focus_button_->setCheckable(true);
  focus_button_->setFixedHeight(30);
  focus_button_->setCursor(Qt::PointingHandCursor);
  focus_button_->setToolTip(
      "Hide the outline and PDF preview to write full width");
  focus_button_->setStyleSheet(
      QString(
          "QPushButton#focusToggle { background: transparent; border: 1px "
          "solid %1;"
          " color: %2; border-radius: 6px; padding: 5px 12px; }"
          "QPushButton#focusToggle:hover { background: %3; border-color: %4;"
          " color: %4; }"
          "QPushButton#focusToggle:checked { background: %4; border-color: %4;"
          " color: white; }")
          .arg(theme::kDivider, theme::kSecondaryText, theme::kAccentSoft,
               theme::kAccent));
  connect(focus_button_, &QPushButton::toggled, this,
          &MainWindow::OnToggleFocusMode);
  layout->addWidget(focus_button_);
  layout->addSpacing(8);

  build_button_ = new QPushButton("Build  ▶", header);
  build_button_->setObjectName("primary");
  build_button_->setFixedHeight(30);
  build_button_->setCursor(Qt::PointingHandCursor);
  build_button_->setToolTip("Build document (Ctrl+B)");
  connect(build_button_, &QPushButton::clicked, this, &MainWindow::OnBuild);
  layout->addWidget(build_button_);

  return header;
}

void MainWindow::OnToggleFocusMode(bool on) {
  if (!main_splitter_)
    return;
  focus_mode_ = on;
  if (on) {
    pre_focus_sizes_ = main_splitter_->sizes();
    // Hide the outline and the preview+problems column; the editor keeps
    // the whole width (the QSplitter redistributes to visible panes).
    outline_->setVisible(false);
    vertical_splitter_->setVisible(false);
  } else {
    outline_->setVisible(true);
    vertical_splitter_->setVisible(true);
    if (!pre_focus_sizes_.isEmpty())
      main_splitter_->setSizes(pre_focus_sizes_);
  }
  if (focus_button_) {
    focus_button_->setText(on ? QStringLiteral("⤡  Exit Focus")
                              : QStringLiteral("⤢  Focus"));
  }
}

void MainWindow::BuildMenus() {
  QMenu *file_menu = menuBar()->addMenu("&File");
  file_menu->addAction("&New Project…", this, &MainWindow::OnNewProject,
                       QKeySequence::New);
  file_menu->addAction("&Open Project…", this, &MainWindow::OnOpenProject,
                       QKeySequence::Open);
  file_menu->addAction("&Save", this, &MainWindow::OnSave, QKeySequence::Save);
  file_menu->addSeparator();
  file_menu->addAction("&Import Bibliography (.bib)…", this,
                       &MainWindow::OnImportBibliography);
  file_menu->addSeparator();
  file_menu->addAction("E&xit", this, &QWidget::close, QKeySequence::Quit);

  QMenu *edit_menu = menuBar()->addMenu("&Edit");
  edit_menu->addAction("&Undo", this, &MainWindow::OnUndo, QKeySequence::Undo);
  edit_menu->addAction("&Redo", this, &MainWindow::OnRedo, QKeySequence::Redo);

  QMenu *build_menu = menuBar()->addMenu("&Build");
  build_menu->addAction("&Build Now", this, &MainWindow::OnBuild,
                        QKeySequence("Ctrl+B"));
}

void MainWindow::WireEditor() {
  connect(editor_, &BlockEditor::TitleEdited, this,
          [this](QString t) {
            controller_->SetTitle(std::move(t));
          });
  connect(editor_, &BlockEditor::AuthorsEdited, this,
          [this](QString t) {
            controller_->SetAuthorsText(std::move(t));
          });
  connect(editor_, &BlockEditor::AffiliationsEdited, this,
          [this](QString t) {
            controller_->SetAffiliationsText(std::move(t));
          });
  connect(editor_, &BlockEditor::AbstractEdited, this,
          [this](QString t) {
            controller_->SetAbstract(std::move(t));
          });
  connect(editor_, &BlockEditor::KeywordsEdited, this,
          [this](QString t) {
            controller_->SetKeywordsText(std::move(t));
          });
  // Rich commit from an InlineEditor row: marks, citations, cross
  // references and inline equations arrive as InlineContent (plan §4.1).
  // Body text has exactly one path into the document - the old
  // ParagraphEdited / "[cite:key]" text encoding is gone (citation plan §5).
  connect(editor_, &BlockEditor::ParagraphContentEdited, this,
          [this](QString node, const InlineContent &content) {
            if (shutting_down_)
              return;
            controller_->EditParagraphRich(NodeId(node.toStdString()), content);
          });
  connect(editor_, &BlockEditor::EquationEdited, this,
          [this](QString node, QString math, bool numbered,
                               QString label) {
            controller_->EditEquation(NodeId(node.toStdString()),
                                      std::move(math), numbered,
                                      std::move(label));
          });
  connect(editor_, &BlockEditor::SectionRenamed, this,
          [this](QString node, QString text) {
            controller_->RenameSection(NodeId(node.toStdString()),
                                       std::move(text));
          });
  connect(editor_, &BlockEditor::SubsectionRenamed, this,
          [this](QString node, QString text) {
            controller_->RenameSubsection(NodeId(node.toStdString()),
                                          std::move(text));
          });
  connect(editor_, &BlockEditor::SubsubsectionRenamed, this,
          [this](QString node, QString text) {
            controller_->RenameSubsubsection(NodeId(node.toStdString()),
                                             std::move(text));
          });
  connect(
      editor_, &BlockEditor::AuthorAffiliationToggled, this,
      [this](int author_index, QString affiliation, bool linked) {
        if (shutting_down_)
          return;
        const auto result = controller_->SetAuthorAffiliation(
            static_cast<size_t>(author_index),
            AffiliationId(affiliation.toStdString()), linked);
        if (result.status != pf::EditStatus::Applied) {
          statusBar()->showMessage(ToQ(result.detail), 3000);
        }
      });
  connect(editor_, &BlockEditor::MoveBlockToRequested, this,
          [this](QString node, QString anchor) {
            if (shutting_down_)
              return;
            const auto result = controller_->MoveNodeAfter(
                NodeId(node.toStdString()), NodeId(anchor.toStdString()));
            if (result.status != pf::EditStatus::Applied) {
              statusBar()->showMessage(ToQ(result.detail), 3000);
            }
          });
  connect(
      editor_, &BlockEditor::InsertBlockRequested, this,
      [this](QString type, QString after) {
        if (after.isEmpty()) {
          // No anchor: the body has no blocks yet, so the only
          // meaningful insert is the first section.
          if (type == "section") {
            const EditResult created = controller_->InsertSection(QString());
            if (created.status == EditStatus::Applied &&
                !created.created_node.empty()) {
              editor_->RevealNode(
                  QString::fromStdString(created.created_node.value()));
            }
          }
          return;
        }
        const NodeId anchor(after.toStdString());
        EditResult result;
        if (type == "section") {
          result = controller_->InsertSectionAfter(anchor, "");
        } else if (type == "subsection") {
          result = controller_->InsertSubsectionAfter(anchor, "");
        } else if (type == "subsubsection") {
          result = controller_->InsertSubsubsectionAfter(anchor, "");
        } else if (type == "text") {
          result = controller_->InsertParagraphAfter(anchor, "");
        } else if (type == "equation") {
          result = controller_->InsertEquationAfter(anchor, "", true);
        } else if (type == "table") {
          result = controller_->InsertTableAfter(anchor);
        } else if (type == "figure") {
          QString path = QFileDialog::getOpenFileName(
              this, "Insert Figure", {}, "Images (*.png *.jpg *.jpeg *.gif)");
          if (!path.isEmpty()) {
            result = controller_->InsertFigureAfter(anchor, path);
          } else {
            return;
          }
        } else {
          return;
        }
        if (result.status != pf::EditStatus::Applied) {
          statusBar()->showMessage(
              "Could not insert block: " + ToQ(result.detail), 5000);
        }
      });
  connect(editor_, &BlockEditor::CaptionEdited, this,
          [this](QString node, QString caption) {
            controller_->EditCaption(NodeId(node.toStdString()), caption);
          });
  connect(editor_, &BlockEditor::FigureSpanChanged, this,
          [this](QString node, bool double_column) {
            if (shutting_down_)
              return;
            const auto result = controller_->EditFigureSpan(
                NodeId(node.toStdString()), double_column);
            if (result.status != pf::EditStatus::Applied) {
              statusBar()->showMessage(ToQ(result.detail), 3000);
            }
          });
  connect(editor_, &BlockEditor::DeleteBlockRequested, this,
          [this](QString node) {
            auto result = controller_->DeleteNode(NodeId(node.toStdString()));
            if (result.status != pf::EditStatus::Applied) {
              statusBar()->showMessage(
                  "Could not delete block: " + ToQ(result.detail), 5000);
            }
          });
  connect(editor_, &BlockEditor::MoveBlockRequested, this,
          [this](QString node, int direction) {
            auto result =
                controller_->MoveNode(NodeId(node.toStdString()), direction);
            if (result.status != pf::EditStatus::Applied) {
              statusBar()->showMessage(ToQ(result.detail), 3000);
            }
          });
  // The legacy InsertCitationRequested / InsertCrossRefRequested detour is
  // gone (citation plan §5): every citation enters through the focused
  // row's InlineEditor and commits as rich content.
  connect(outline_, &OutlinePanel::NodeActivated, this,
          [this](QString node) { editor_->RevealNode(node); });
  // Reverse sync (UI plan §10): the caret moves -> the outline highlights
  // the owning section, so a long manuscript always says where you are.
  connect(editor_, &BlockEditor::FocusOutlineChanged, outline_,
          &OutlinePanel::SelectNode);
  connect(outline_, &OutlinePanel::CitationChosen, this,
          [this](QString key) {
            auto focused = editor_->FocusedNodeId();
            if (!focused) {
              statusBar()->showMessage(
                  "Click into a Text block first, then pick a reference", 4000);
              return;
            }
            // Same rich path as the toolbar picker: insert the Citation object
            // at the row's caret and commit immediately.
            if (editor_->InsertCitationIntoParagraph(*focused, key)) {
            } else {
              statusBar()->showMessage(
                  "The focused block is not a text row - citations attach to "
                  "paragraphs",
                  4000);
            }
          });
}

void MainWindow::ShowWorkspace(bool show) {
  central_stack_->setCurrentWidget(show ? workspace_ : welcome_);
}

void MainWindow::UpdateRequiredHints() {
  if (!controller_->has_project())
    return;
  BlockEditor::RequiredHints hints;
  const auto *def = TemplateRegistry::Instance().Find(
      controller_->session().state().template_selection());
  if (def) {
    hints.title = def->required.title;
    hints.authors = def->required.authors;
    hints.affiliations =
        def->required.affiliations || def->required.author_affiliations;
    hints.abstract_text = def->required.abstract_text;
    hints.keywords = def->required.keywords;
  }
  editor_->SetRequiredHints(hints);
}

void MainWindow::RefreshReferenceItems() {
  if (!controller_->has_project())
    return;
  std::vector<PopupList::Item> items;
  for (const auto &entry : controller_->session().bibliography().Entries()) {
    PopupList::Item item;
    QString authors = entry.authors.empty()
                          ? QString()
                          : ToQ(entry.authors.front()) + " et al.";
    item.label = authors.isEmpty() ? ToQ(entry.title)
                                   : authors + " · " + ToQ(entry.year);
    item.detail = "Reference";
    item.group = "References";
    item.payload = "cite:" + ToQ(entry.key);
    items.push_back(std::move(item));
  }
  const Document &doc = controller_->session().state().document();
  for (const auto &section : doc.body().sections) {
    PopupList::Item item;
    item.label = ToQ(pf::InlineToPlainText(section.title));
    if (item.label.isEmpty())
      item.label = "Untitled Section";
    item.detail = "Section";
    item.group = "Sections";
    item.payload = "xref:" + ToQ(section.id.value());
    items.push_back(std::move(item));
    for (const auto &block : section.blocks) {
      if (const auto *fig = std::get_if<pf::Figure>(&block)) {
        PopupList::Item fig_item;
        fig_item.label = ToQ(pf::InlineToPlainText(fig->caption));
        if (fig_item.label.isEmpty()) {
          fig_item.label = "Untitled figure";
        }
        fig_item.detail = "Figure";
        fig_item.group = "Figures";
        fig_item.payload = "xref:" + ToQ(fig->id.value());
        items.push_back(std::move(fig_item));
      }
    }
  }
  editor_->SetReferenceItems(std::move(items));
}

int MainWindow::CountWords() const {
  if (!controller_->has_project())
    return 0;
  const Document &doc = controller_->session().state().document();
  QString text = ToQ(pf::InlineToPlainText(doc.front_matter().title));
  if (doc.front_matter().abstract_text) {
    text += " " + ToQ(pf::InlineToPlainText(*doc.front_matter().abstract_text));
  }
  for (const auto &section : doc.body().sections) {
    text += " " + ToQ(pf::InlineToPlainText(section.title));
    for (const auto &block : section.blocks) {
      if (const auto *para = std::get_if<pf::Paragraph>(&block)) {
        text += " " + ToQ(pf::InlineToPlainText(para->content));
      }
    }
    for (const auto &sub : section.subsections) {
      for (const auto &block : sub.blocks) {
        if (const auto *para = std::get_if<pf::Paragraph>(&block)) {
          text += " " + ToQ(pf::InlineToPlainText(para->content));
        }
      }
    }
  }
  return text
      .split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts)
      .size();
}

// ---------------- Actions ----------------

void MainWindow::OnNewProject() {
  // Flush the row the user is editing before the old document is replaced
  // (citation plan §6); the autosave then captures the final state.
  if (controller_->has_project())
    editor_->CommitFocused();
  QString dir =
      QFileDialog::getExistingDirectory(this, "New Project Directory");
  if (dir.isEmpty())
    return;
  if (std::filesystem::exists(dir.toStdString() + "/project.paper")) {
    QMessageBox::warning(this, "PaperForge",
                         "Directory already contains a project.");
    return;
  }
  if (!controller_->NewProject(dir)) {
    QMessageBox::critical(this, "PaperForge", "Failed to create project.");
    return;
  }
  QSettings settings;
  if (!recent_projects_.contains(dir)) {
    recent_projects_.prepend(dir);
    while (recent_projects_.size() > 8)
      recent_projects_.removeLast();
    settings.setValue(kSettingsKey, recent_projects_);
  }
  controller_->StartAutosave();
  ShowWorkspace(true);
  RenderProjectState();  // P0-06: a new project is Dirty from the start.
  statusBar()->showMessage("Created project: " + dir);
}

void MainWindow::OnOpenProject() {
  QString dir =
      QFileDialog::getExistingDirectory(this, "Open Project Directory");
  if (dir.isEmpty())
    return;
  OpenProjectDir(dir);
}

bool MainWindow::OpenProjectDir(const QString &dir) {
  bool recovered = false;
  if (!controller_->OpenProjectWithRecovery(dir, &recovered)) {
    return false;
  }
  QSettings settings;
  if (!recent_projects_.contains(dir)) {
    recent_projects_.prepend(dir);
    while (recent_projects_.size() > 8)
      recent_projects_.removeLast();
    settings.setValue(kSettingsKey, recent_projects_);
  }
  controller_->StartAutosave();
  if (recovered) {
    QMessageBox::information(
        this, "PaperForge",
        "Recovered unsaved changes from the autosave snapshot.");
  }
  ShowWorkspace(true);
  RenderProjectState();  // P0-06: render the actual state of the loaded project.
  statusBar()->showMessage("Opened project: " + dir);
  return true;
}

void MainWindow::OnSave() {
  if (!controller_->has_project())
    return;
  // Citation plan §6: anything that reads the Document must first flush the
  // focused row, or the snapshot silently misses what the GUI already shows.
  editor_->CommitFocused();
  // Save is asynchronous: the snapshot is captured now and written by the
  // save worker. P0-06: the label follows the authoritative state - the
  // session flips to Saving on a successful enqueue and the completion
  // handler renders the outcome; a rejected enqueue shows the failure.
  auto result = controller_->session().Save();
  if (result.status != SaveResult::Status::Queued) {
    save_state_label_->setText("! Save failed — " + ToQ(result.detail));
    save_state_label_->setStyleSheet(QString("color: %1;").arg(theme::kError));
  } else {
    RenderProjectState();
  }
}

void MainWindow::OnSaveFinished(bool success, const QString &detail) {
  if (shutting_down_)
    return;
  // P0-06: the completion itself is informational; the label re-derives
  // from the session state (RenderProjectState is invoked by the
  // stateChanged emission that follows every completion).
  if (!success && !detail.isEmpty())
    statusBar()->showMessage("Save: " + detail, 5000);
  RenderProjectState();
}

void MainWindow::RenderProjectState() {
  if (shutting_down_)
    return;
  // P0-06: single source of truth. The label is a pure projection of
  // ProjectSession's persistence/preview state and current revision.
  if (!controller_->has_project()) {
    save_state_label_->setText("");
    save_state_label_->setStyleSheet("");
    return;
  }
  const auto persistence = controller_->persistence_state();
  switch (persistence) {
  case PersistenceState::Clean:
    save_state_label_->setText("✓ Saved");
    save_state_label_->setStyleSheet("");
    break;
  case PersistenceState::Dirty:
    save_state_label_->setText("● Unsaved changes");
    save_state_label_->setStyleSheet("");
    break;
  case PersistenceState::Saving:
    save_state_label_->setText("Saving…");
    save_state_label_->setStyleSheet("");
    break;
  case PersistenceState::SaveFailed:
    save_state_label_->setText("! Save failed — retry (Ctrl+S)");
    save_state_label_->setStyleSheet(QString("color: %1;").arg(theme::kError));
    break;
  }
}

// Undo/redo restore a whole document snapshot, so rows are re-read from the
// document (their structure may be identical while the text is not).
void MainWindow::OnUndo() {
  controller_->Undo();
  RefreshDocumentView();
}
void MainWindow::OnRedo() {
  controller_->Redo();
  RefreshDocumentView();
}

void MainWindow::OnBuild() {
  if (!controller_->has_project())
    return;
  if (building_) {
    // The button doubles as Cancel during a build (plan §34/§39). The
    // worker finishes the attempt as Cancelled; its preview and Problems
    // results are gated out, its log stays visible until replaced.
    controller_->CancelBuild();
    build_button_->setText("Cancelling \u25EF");
    return;
  }
  // Citation plan §6: Commit → the commit path runs synchronously
  // (ParagraphContentEdited → EditParagraphRich → EditingSystem → the
  // Document now holds the fresh content and its revision was bumped) →
  // only then RequestBuild, so the build snapshot can never lag behind
  // what the user sees - the exact failure mode that made a freshly
  // chosen Citation vanish from the PDF.
  editor_->CommitFocused();
  build_button_->setText("Building ◌");
  controller_->RequestBuild(true);
}

void MainWindow::OnImportBibliography() {
  if (!controller_->has_project())
    return;
  // Flush first: the import triggers a validation + build, and validation
  // reads the Document (citation plan §6/§9).
  editor_->CommitFocused();
  QString path =
      QFileDialog::getOpenFileName(this, "Import BibTeX", {}, "BibTeX (*.bib)");
  if (path.isEmpty())
    return;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return;
  QString text = QString::fromUtf8(file.readAll());
  auto result = controller_->ImportBibliographyText(text);
  if (result.status == BibliographyImportResult::Status::Ok) {
    QString message = QString("Bibliography imported: %1 entries from %2")
                          .arg(result.entry_count)
                          .arg(path);
    if (!result.duplicate_keys.empty()) {
      // Citation plan §9: a duplicate key is surfaced, never silently
      // merged. BibTeX keeps the last definition; so do we.
      QStringList dups;
      for (const auto &key : result.duplicate_keys) {
        dups << QString::fromStdString(key);
      }
      message += QString(" - WARNING: duplicate keys: %1").arg(dups.join(", "));
      statusBar()->showMessage(message, 10000);
      QMessageBox::warning(
          this, "PaperForge",
          "The imported file contains duplicate BibTeX keys:\n" +
              dups.join("\n") +
              "\n\nThe last definition of each duplicate was kept.");
    } else {
      statusBar()->showMessage(message);
    }
    RefreshReferenceItems();
  } else {
    // A failed import leaves the previous bibliography and its
    // references.bib untouched (citation plan §7).
    QMessageBox::warning(this, "PaperForge",
                         "Failed to parse BibTeX file: nothing was "
                         "imported; the current bibliography is unchanged.");
  }
}

void MainWindow::OnChangeTemplate(const QString &template_id) {
  // A template change re-renders everything from the Document; flush the
  // focused row first (citation plan §6).
  editor_->CommitFocused();
  controller_->ChangeTemplate(template_id);
  UpdateRequiredHints();
}

// ---------------- Refresh ----------------

MainWindow::~MainWindow() {
  shutting_down_ = true;
  // Best effort: keep whatever the user last typed.
  if (editor_)
    editor_->CommitFocused();
  // Late signals from widgets that are being destroyed must not reach us.
  if (editor_)
    disconnect(editor_, nullptr, this, nullptr);
  if (controller_) {
    controller_->StopAutosave();
    disconnect(controller_, nullptr, this, nullptr);
  }
}

void MainWindow::RefreshDocumentView() {
  if (shutting_down_)
    return;
  if (!controller_->has_project())
    return;
  const Document &doc = controller_->session().state().document();
  // Diagnostics belong to the build that produced them: once the document
  // moves on, mark the visible set Outdated (plan §49). The content of the
  // diagnostics is not rewritten - the next build replaces the whole set.
  if (has_built_) {
    const auto revision = controller_->current_revision();
    problems_->SetStale(
        revision != last_built_revision_,
        static_cast<int>(revision.value > last_built_revision_.value
                             ? revision.value - last_built_revision_.value
                             : 0));
  }
  // Citation plan §3: hand the editor the document-wide key -> number map
  // before rebuilding rows. The "[1]" pills in the GUI and the numbers in
  // the PDF are the same projection of the same citation-order policy.
  editor_->SetCitationNumbers(controller_->CitationNumbers());
  // The insert and "/" menus are filtered by what the current template can
  // express (plan §9).
  if (const auto *tpl = TemplateRegistry::Instance().Find(
          controller_->session().state().template_selection())) {
    editor_->SetMaxHeadingDepth(tpl->capabilities.max_heading_depth);
  } else {
    editor_->SetMaxHeadingDepth(3);
  }
  // Never tear the rows down while the user is mid-edit: rebuilding
  // recreates every editor widget, which would throw away the text being
  // typed. The refresh is deferred to the next commit instead.
  if (editor_->HasUncommittedFocus()) {
    pending_structural_refresh_ = true;
    RefreshSidePanels();
    return;
  }
  pending_structural_refresh_ = false;
  editor_->RebuildFromDocument(doc);
  RefreshSidePanels();
}

// Outline, required-field hints and word count: everything except the editor
// rows, so it is always safe to run.
void MainWindow::RefreshSidePanels() {
  if (shutting_down_)
    return;
  outline_->RebuildFromDocument(
      controller_->session().state().document(),
      controller_->session().bibliography().Entries());
  UpdateRequiredHints();
  editor_->RefreshHints();
  RefreshReferenceItems();
  word_count_label_->setText(QString::number(CountWords()) + " words");
}

void MainWindow::OnPreviewUpdated(const pf::PreviewUpdate &update) {
  if (shutting_down_)
    return;
  // The session's preview gate already dropped stale/foreign results; the
  // identity is still asserted here so the pane never shows a PDF from a
  // different project or revision.
  if (!controller_->has_project())
    return;
  if (update.project_id != controller_->session().state().id())
    return;
  if (update.revision != controller_->current_revision())
    return;

  const int revision = static_cast<int>(update.revision.value);
  if (update.success && update.pdf.valid()) {
    build_button_->setText("✓ Built");
    build_button_->setStyleSheet(
        QString("QPushButton { background: %1; color: white; border: none;"
                " font-weight: 600; }")
            .arg(theme::kOk));
    build_state_label_->setText(QString("Build: ✓ Revision %1").arg(revision));
    current_pdf_path_ = ToQ(update.pdf.path.string());
    last_build_revision_ = revision;
    RefreshPreview();
  } else if (last_outcome_ == BuildResult::Outcome::Cancelled) {
    // A cancelled attempt is not a document failure (plan §34): the
    // preview keeps showing the last valid PDF and the button says so.
    build_button_->setText("Build cancelled");
    build_state_label_->setText("Build: cancelled");
  } else {
    build_button_->setText("! Build failed");
    build_state_label_->setText("Build: failed");
    build_button_->setStyleSheet(
        QString("QPushButton { background: %1; color: white; border: none;"
                " font-weight: 600; }")
            .arg(theme::kError));
  }
  QTimer::singleShot(3000, this, [this]() {
    // A newer attempt may already own the button (Cancel); don't clobber
    // its state (plan §39).
    if (building_)
      return;
    build_button_->setText("Build  ▶");
    build_button_->setStyleSheet("");
  });
}

void MainWindow::OnBuildCompleted(const pf::BuildResult &result) {
  if (shutting_down_)
    return;
  // One update per finished build (plan §30); the diagnostics are already
  // structured - nothing here inspects log text.
  problems_->SetDiagnostics(result.diagnostics);
  last_built_revision_ = result.revision;
  last_outcome_ = result.outcome;
  has_built_ = true;

  if (result.outcome == BuildResult::Outcome::Cancelled) {
    // Cancelled builds update neither Preview nor tab visibility; only
    // the transient status changes (plan §34).
    return;
  }
  // Automatic tab switch (plan §40): success never steals the view; a
  // document-level failure reveals Problems; a runtime-level failure -
  // where no Problems entry can name a block - reveals the Build Log.
  if (result.outcome == BuildResult::Outcome::Failure) {
    const bool system_failure =
        result.failure_kind == CompileFailureKind::RuntimeMissing ||
        result.failure_kind == CompileFailureKind::RuntimeCorrupted ||
        result.failure_kind == CompileFailureKind::PackageMissing ||
        result.failure_kind == CompileFailureKind::FontMissing ||
        result.failure_kind == CompileFailureKind::Timeout ||
        result.failure_kind == CompileFailureKind::InternalError;
    if (system_failure)
      problems_->ShowBuildLog();
    else
      problems_->ShowProblemsTab();
  }
}

void MainWindow::OnBuildEvent(const pf::BuildEvent &event) {
  if (shutting_down_)
    return;
  problems_->AppendEvent(event);
  switch (event.type) {
  case BuildEventType::BuildStarted:
    building_ = true;
    // Build button becomes Cancel while an attempt is in flight
    // (plan §39).
    build_button_->setText("Cancel  \u25A0");
    break;
  case BuildEventType::BuildSucceeded:
  case BuildEventType::BuildFailed:
  case BuildEventType::BuildCancelled:
    building_ = false;
    break;
  default:
    break;
  }
}

void MainWindow::OnProblemActivated(const pf::Diagnostic &diagnostic) {
  if (shutting_down_ || !controller_->has_project())
    return;
  // Navigation (plan §25/§28/§29/§48): block first, then the Build Log as
  // fallback, otherwise nothing.
  if (diagnostic.location.has_block_location()) {
    const Document &doc = controller_->session().state().document();
    if (doc.ContainsNode(diagnostic.location.node)) {
      editor_->RevealNode(ToQ(diagnostic.location.node.value()));
      return;
    }
    // The block was deleted after this build: report, never crash.
    statusBar()->showMessage(
        "Location unavailable - that block is gone; rebuild to refresh "
        "the problems list",
        5000);
    return;
  }
  if (diagnostic.location.has_file_location() ||
      !diagnostic.raw_message.empty()) {
    problems_->ShowBuildLog();
  }
}

void MainWindow::OnBuildStatusChanged(const QString &status) {
  build_state_label_->setText("Build: " + status);
  // The phase observer is the reliable "no build in flight" signal: it also
  // fires when a request is dropped during the debounce. Release the Cancel
  // affordance whenever the coordinator returns to Idle (plan §39).
  if (status == "Idle") {
    building_ = false;
    if (build_button_->text() == "Cancel  \u25A0" ||
        build_button_->text() == "Cancelling \u25EF" ||
        build_button_->text() == "Building \u25CC") {
      build_button_->setText("Build  \u25B6");
      build_button_->setStyleSheet("");
    }
  }
}

void MainWindow::ZoomPreviewForTest(double zoom, double scroll_x,
                                    double scroll_y) {
  // zoom <= 0 keeps the current zoom and only scrolls.
  if (zoom > 0.0)
    preview_->SetZoom(zoom);
  preview_->ScrollTo(scroll_x, scroll_y);
}

void MainWindow::RefreshPreview() {
  if (current_pdf_path_.isEmpty())
    return;
  // Keep the current zoom and scroll position; only the page changes.
  preview_->SetDocument(current_pdf_path_);
}

} // namespace pf::gui
