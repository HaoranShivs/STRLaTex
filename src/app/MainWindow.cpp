#include "app/MainWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QPushButton>
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
QString ToQ(const std::string& s) { return QString::fromStdString(s); }
constexpr const char* kSettingsKey = "recent_projects";

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    controller_ = new ProjectController(this);

    QSettings settings;
    recent_projects_ = settings.value(kSettingsKey).toStringList();

    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: before BuildUi\n");
    BuildUi();
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: after BuildUi\n");
    BuildMenus();
    WireEditor();
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: after WireEditor\n");

    connect(controller_, &ProjectController::documentChanged, this,
            &MainWindow::RefreshDocumentView);
    connect(editor_, &BlockEditor::RowCommitted, this, [this]() {
        if (pending_structural_refresh_) RefreshDocumentView();
    });
    connect(controller_, &ProjectController::previewUpdated, this,
            &MainWindow::OnPreviewUpdated);
    connect(controller_, &ProjectController::saveFinished, this,
            &MainWindow::OnSaveFinished);
    connect(controller_, &ProjectController::diagnosticsUpdated, this,
            &MainWindow::OnDiagnosticsUpdated);
    connect(controller_, &ProjectController::buildStatusChanged, this,
            &MainWindow::OnBuildStatusChanged);

    setWindowTitle("PaperForge");
    resize(1500, 920);
    ShowWorkspace(false);
}

// ---------------- UI construction ----------------

void MainWindow::BuildUi() {
    central_stack_ = new QStackedWidget(this);
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: stack created\n");

    welcome_ = new WelcomePage(central_stack_);
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: welcome created\n");
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
    for (const QString& path : recent_projects_) {
        welcome_->AddRecent(QFileInfo(path).fileName(), path);
    }
    central_stack_->addWidget(welcome_);

    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: welcome wired\n");
    workspace_ = new QWidget(central_stack_);
    auto* workspace_layout = new QVBoxLayout(workspace_);
    workspace_layout->setContentsMargins(0, 0, 0, 0);
    workspace_layout->setSpacing(0);
    workspace_layout->addWidget(BuildHeader());

    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: workspace created\n");
    auto* splitter = new QSplitter(Qt::Horizontal, workspace_);
    outline_ = new OutlinePanel(splitter);
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: outline created\n");
    editor_ = new BlockEditor(splitter);
    editor_->SetAssetPathResolver([this](const AssetId& id) {
        if (!controller_->has_project()) return QString();
        const auto* metadata =
            controller_->session().assets().registry().Find(id);
        if (!metadata) return QString();
        return ToQ((controller_->session().paths().assets_dir /
                    metadata->relative_path)
                       .string());
    });
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: editor created\n");

    vertical_splitter_ = new QSplitter(Qt::Vertical, splitter);
    preview_ = new PdfPreview(vertical_splitter_);
    preview_container_ = preview_;
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: preview created\n");
    problems_ = new ProblemsPanel(vertical_splitter_);
    if (!qEnvironmentVariable("PF_TRACE").isEmpty()) fprintf(stderr, "TRACE: problems created\n");
    vertical_splitter_->addWidget(preview_container_);
    vertical_splitter_->addWidget(problems_);
    vertical_splitter_->setStretchFactor(0, 3);
    vertical_splitter_->setStretchFactor(1, 1);
    vertical_splitter_->setSizes({420, 200});

    splitter->addWidget(outline_);
    splitter->addWidget(editor_);
    splitter->addWidget(vertical_splitter_);
    splitter->setSizes({270, 780, 450});

    workspace_layout->addWidget(splitter, 1);
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

QWidget* MainWindow::BuildHeader() {
    auto* header = new QWidget(workspace_);
    header->setObjectName("headerBar");
    header->setFixedHeight(46);
    header->setStyleSheet(QString(
        "QWidget#headerBar { background: %1; border-bottom: 1px solid %2; }")
        .arg(theme::kEditorBackground, theme::kDivider));
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(16, 6, 16, 6);

    auto* logo = new QLabel("PaperForge", header);
    logo->setStyleSheet(QString("font-weight: 700; color: %1;")
                            .arg(theme::kPrimaryText));
    layout->addWidget(logo);
    layout->addSpacing(12);

    template_combo_ = new QComboBox(header);
    for (const auto& def : TemplateRegistry::Instance().All()) {
        template_combo_->addItem(ToQ(def.name), ToQ(def.id));
    }
    template_combo_->setStyleSheet(QString(
        "QComboBox { border: none; background: transparent; color: %1; }")
        .arg(theme::kSecondaryText));
    connect(template_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (controller_->has_project()) {
            OnChangeTemplate(template_combo_->currentData().toString());
        }
    });
    layout->addWidget(template_combo_);
    layout->addStretch(1);

    build_button_ = new QPushButton("Build  ▶", header);
    build_button_->setObjectName("primary");
    build_button_->setFixedHeight(30);
    build_button_->setCursor(Qt::PointingHandCursor);
    build_button_->setToolTip("Build document (Ctrl+B)");
    connect(build_button_, &QPushButton::clicked, this, &MainWindow::OnBuild);
    layout->addWidget(build_button_);

    return header;
}

void MainWindow::BuildMenus() {
    QMenu* file_menu = menuBar()->addMenu("&File");
    file_menu->addAction("&New Project…", this, &MainWindow::OnNewProject,
                         QKeySequence::New);
    file_menu->addAction("&Open Project…", this, &MainWindow::OnOpenProject,
                         QKeySequence::Open);
    file_menu->addAction("&Save", this, &MainWindow::OnSave,
                         QKeySequence::Save);
    file_menu->addSeparator();
    file_menu->addAction("&Import Bibliography (.bib)…", this,
                         &MainWindow::OnImportBibliography);
    file_menu->addSeparator();
    file_menu->addAction("E&xit", this, &QWidget::close, QKeySequence::Quit);

    QMenu* edit_menu = menuBar()->addMenu("&Edit");
    edit_menu->addAction("&Undo", this, &MainWindow::OnUndo,
                         QKeySequence::Undo);
    edit_menu->addAction("&Redo", this, &MainWindow::OnRedo,
                         QKeySequence::Redo);

    QMenu* build_menu = menuBar()->addMenu("&Build");
    build_menu->addAction("&Build Now", this, &MainWindow::OnBuild,
                          QKeySequence("Ctrl+B"));
}

void MainWindow::WireEditor() {
    auto mark_unsaved = [this]() {
        save_state_label_->setText("● Unsaved changes");
        save_state_label_->setStyleSheet("");
    };
    connect(editor_, &BlockEditor::TitleEdited, this, [this, mark_unsaved](QString t) {
        controller_->SetTitle(std::move(t));
        mark_unsaved();
    });
    connect(editor_, &BlockEditor::AuthorsEdited, this,
            [this, mark_unsaved](QString t) {
                controller_->SetAuthorsText(std::move(t));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::AffiliationsEdited, this,
            [this, mark_unsaved](QString t) {
                controller_->SetAffiliationsText(std::move(t));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::AbstractEdited, this,
            [this, mark_unsaved](QString t) {
                controller_->SetAbstract(std::move(t));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::KeywordsEdited, this,
            [this, mark_unsaved](QString t) {
                controller_->SetKeywordsText(std::move(t));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::ParagraphEdited, this,
            [this, mark_unsaved](QString node, QString text) {
                if (shutting_down_) return;
                controller_->EditParagraph(NodeId(node.toStdString()),
                                           std::move(text));
                mark_unsaved();
            });
    // Rich commit from an InlineEditor row: marks, citations, cross
    // references and inline equations arrive as InlineContent (plan §4.1).
    connect(editor_, &BlockEditor::ParagraphContentEdited, this,
            [this, mark_unsaved](QString node, const InlineContent& content) {
                if (shutting_down_) return;
                controller_->EditParagraphRich(NodeId(node.toStdString()),
                                               content);
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::EquationEdited, this,
            [this, mark_unsaved](QString node, QString math, bool numbered,
                                 QString label) {
                controller_->EditEquation(NodeId(node.toStdString()),
                                          std::move(math), numbered,
                                          std::move(label));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::SectionRenamed, this,
            [this, mark_unsaved](QString node, QString text) {
                controller_->RenameSection(NodeId(node.toStdString()),
                                           std::move(text));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::SubsectionRenamed, this,
            [this, mark_unsaved](QString node, QString text) {
                controller_->RenameSubsection(NodeId(node.toStdString()),
                                              std::move(text));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::SubsubsectionRenamed, this,
            [this, mark_unsaved](QString node, QString text) {
                controller_->RenameSubsubsection(NodeId(node.toStdString()),
                                                 std::move(text));
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::AuthorAffiliationToggled, this,
            [this, mark_unsaved](int author_index, QString affiliation,
                                 bool linked) {
                if (shutting_down_) return;
                const auto result = controller_->SetAuthorAffiliation(
                    static_cast<size_t>(author_index),
                    AffiliationId(affiliation.toStdString()), linked);
                if (result.status == EditStatus::Applied) {
                    mark_unsaved();
                } else {
                    statusBar()->showMessage(ToQ(result.detail), 3000);
                }
            });
    connect(editor_, &BlockEditor::MoveBlockToRequested, this,
            [this, mark_unsaved](QString node, QString anchor) {
                if (shutting_down_) return;
                const auto result = controller_->MoveNodeAfter(
                    NodeId(node.toStdString()), NodeId(anchor.toStdString()));
                if (result.status == EditStatus::Applied) {
                    mark_unsaved();
                } else {
                    statusBar()->showMessage(ToQ(result.detail), 3000);
                }
            });
    connect(editor_, &BlockEditor::InsertBlockRequested, this,
            [this, mark_unsaved](QString type, QString after) {
                if (after.isEmpty()) {
                    // No anchor: the body has no blocks yet, so the only
                    // meaningful insert is the first section.
                    if (type == "section") {
                        const EditResult created =
                            controller_->InsertSection(QString());
                        mark_unsaved();
                        if (created.status == EditStatus::Applied &&
                            !created.created_node.empty()) {
                            editor_->RevealNode(
                                QString::fromStdString(
                                    created.created_node.value()));
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
                        this, "Insert Figure", {},
                        "Images (*.png *.jpg *.jpeg *.gif)");
                    if (!path.isEmpty()) {
                        result = controller_->InsertFigureAfter(anchor, path);
                    } else {
                        return;
                    }
                } else {
                    return;
                }
                if (result.status == EditStatus::Applied) {
                    mark_unsaved();
                } else {
                    statusBar()->showMessage(
                        "Could not insert block: " + ToQ(result.detail), 5000);
                }
            });
    connect(editor_, &BlockEditor::CaptionEdited, this,
            [this, mark_unsaved](QString node, QString caption) {
                auto result = controller_->EditCaption(
                    NodeId(node.toStdString()), caption);
                if (result.status == EditStatus::Applied) mark_unsaved();
            });
    connect(editor_, &BlockEditor::DeleteBlockRequested, this,
            [this, mark_unsaved](QString node) {
                auto result =
                    controller_->DeleteNode(NodeId(node.toStdString()));
                if (result.status == EditStatus::Applied) {
                    mark_unsaved();
                } else {
                    statusBar()->showMessage(
                        "Could not delete block: " + ToQ(result.detail), 5000);
                }
            });
    connect(editor_, &BlockEditor::MoveBlockRequested, this,
            [this, mark_unsaved](QString node, int direction) {
                auto result = controller_->MoveNode(
                    NodeId(node.toStdString()), direction);
                if (result.status == EditStatus::Applied) {
                    mark_unsaved();
                } else {
                    statusBar()->showMessage(ToQ(result.detail), 3000);
                }
            });
    connect(editor_, &BlockEditor::InsertCitationRequested, this,
            [this, mark_unsaved](QString paragraph, QString key) {
                controller_->InsertCitation(NodeId(paragraph.toStdString()),
                                            {key});
                mark_unsaved();
            });
    connect(editor_, &BlockEditor::InsertCrossRefRequested, this,
            [this, mark_unsaved](QString paragraph, QString target) {
                auto result = controller_->InsertCrossReference(
                    NodeId(paragraph.toStdString()),
                    NodeId(target.toStdString()));
                if (result.status == EditStatus::Applied) mark_unsaved();
            });
    connect(outline_, &OutlinePanel::NodeActivated, this,
            [this](QString node) { editor_->RevealNode(node); });
    connect(outline_, &OutlinePanel::CitationChosen, this, [this](QString key) {
        auto focused = editor_->FocusedNodeId();
        if (focused) {
            controller_->InsertCitation(NodeId(focused->toStdString()), {key});
        }
    });
}

void MainWindow::ShowWorkspace(bool show) {
    central_stack_->setCurrentWidget(show ? workspace_ : welcome_);
}

void MainWindow::UpdateRequiredHints() {
    if (!controller_->has_project()) return;
    BlockEditor::RequiredHints hints;
    const auto* def = TemplateRegistry::Instance().Find(
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
    if (!controller_->has_project()) return;
    std::vector<PopupList::Item> items;
    for (const auto& entry : controller_->session().bibliography().Entries()) {
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
    const Document& doc = controller_->session().state().document();
    for (const auto& section : doc.body().sections) {
        PopupList::Item item;
        item.label = ToQ(pf::InlineToPlainText(section.title));
        if (item.label.isEmpty()) item.label = "Untitled Section";
        item.detail = "Section";
        item.group = "Sections";
        item.payload = "xref:" + ToQ(section.id.value());
        items.push_back(std::move(item));
        for (const auto& block : section.blocks) {
            if (const auto* fig = std::get_if<pf::Figure>(&block)) {
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
    if (!controller_->has_project()) return 0;
    const Document& doc = controller_->session().state().document();
    QString text = ToQ(pf::InlineToPlainText(doc.front_matter().title));
    if (doc.front_matter().abstract_text) {
        text += " " + ToQ(pf::InlineToPlainText(*doc.front_matter().abstract_text));
    }
    for (const auto& section : doc.body().sections) {
        text += " " + ToQ(pf::InlineToPlainText(section.title));
        for (const auto& block : section.blocks) {
            if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                text += " " + ToQ(pf::InlineToPlainText(para->content));
            }
        }
        for (const auto& sub : section.subsections) {
            for (const auto& block : sub.blocks) {
                if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
                    text += " " + ToQ(pf::InlineToPlainText(para->content));
                }
            }
        }
    }
    return text.split(QRegularExpression(QStringLiteral(R"(\s+)")),
                      Qt::SkipEmptyParts)
        .size();
}

// ---------------- Actions ----------------

void MainWindow::OnNewProject() {
    QString dir = QFileDialog::getExistingDirectory(this,
                                                    "New Project Directory");
    if (dir.isEmpty()) return;
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
        while (recent_projects_.size() > 8) recent_projects_.removeLast();
        settings.setValue(kSettingsKey, recent_projects_);
    }
    controller_->StartAutosave();
    ShowWorkspace(true);
    statusBar()->showMessage("Created project: " + dir);
}

void MainWindow::OnOpenProject() {
    QString dir = QFileDialog::getExistingDirectory(this,
                                                    "Open Project Directory");
    if (dir.isEmpty()) return;
    OpenProjectDir(dir);
}

bool MainWindow::OpenProjectDir(const QString& dir) {
    bool recovered = false;
    if (!controller_->OpenProjectWithRecovery(dir, &recovered)) {
        return false;
    }
    QSettings settings;
    if (!recent_projects_.contains(dir)) {
        recent_projects_.prepend(dir);
        while (recent_projects_.size() > 8) recent_projects_.removeLast();
        settings.setValue(kSettingsKey, recent_projects_);
    }
    controller_->StartAutosave();
    if (recovered) {
        QMessageBox::information(
            this, "PaperForge",
            "Recovered unsaved changes from the autosave snapshot.");
    }
    ShowWorkspace(true);
    statusBar()->showMessage("Opened project: " + dir);
    return true;
}

void MainWindow::OnSave() {
    if (!controller_->has_project()) return;
    save_state_label_->setText("Saving…");
    // Save is asynchronous: the snapshot is captured now and written by the
    // save worker; OnSaveFinished reports the outcome on the app thread.
    auto result = controller_->session().Save();
    if (result.status != SaveResult::Status::Queued) {
        save_state_label_->setText("! Save failed — " + ToQ(result.detail));
        save_state_label_->setStyleSheet(
            QString("color: %1;").arg(theme::kError));
    }
}

void MainWindow::OnSaveFinished(bool success, const QString& detail) {
    if (shutting_down_) return;
    if (success) {
        save_state_label_->setText("✓ Saved");
        save_state_label_->setStyleSheet("");
    } else {
        save_state_label_->setText("! Save failed — " + detail);
        save_state_label_->setStyleSheet(
            QString("color: %1;").arg(theme::kError));
    }
}

// Undo/redo restore a whole document snapshot, so rows are re-read from the
// document (their structure may be identical while the text is not).
void MainWindow::OnUndo() { controller_->Undo(); RefreshDocumentView(); }
void MainWindow::OnRedo() { controller_->Redo(); RefreshDocumentView(); }

void MainWindow::OnBuild() {
    if (!controller_->has_project()) return;
    build_button_->setText("Building ◌");
    controller_->RequestBuild(true);
}

void MainWindow::OnImportBibliography() {
    if (!controller_->has_project()) return;
    QString path = QFileDialog::getOpenFileName(this, "Import BibTeX", {},
                                                "BibTeX (*.bib)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QString text = QString::fromUtf8(file.readAll());
    if (controller_->ImportBibliographyText(text)) {
        statusBar()->showMessage("Bibliography imported: " + path);
        RefreshReferenceItems();
    } else {
        QMessageBox::warning(this, "PaperForge",
                             "Failed to parse BibTeX file.");
    }
}

void MainWindow::OnChangeTemplate(const QString& template_id) {
    controller_->ChangeTemplate(template_id);
    UpdateRequiredHints();
}

// ---------------- Refresh ----------------

MainWindow::~MainWindow() {
    shutting_down_ = true;
    // Best effort: keep whatever the user last typed.
    if (editor_) editor_->CommitFocused();
    // Late signals from widgets that are being destroyed must not reach us.
    if (editor_) disconnect(editor_, nullptr, this, nullptr);
    if (controller_) {
        controller_->StopAutosave();
        disconnect(controller_, nullptr, this, nullptr);
    }
}

void MainWindow::RefreshDocumentView() {
    if (shutting_down_) return;
    if (!controller_->has_project()) return;
    const Document& doc = controller_->session().state().document();
    // The insert and "/" menus are filtered by what the current template can
    // express (plan §9).
    if (const auto* tpl = TemplateRegistry::Instance().Find(
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
    if (shutting_down_) return;
    outline_->RebuildFromDocument(controller_->session().state().document(),
                                  controller_->session()
                                      .bibliography()
                                      .Entries());
    UpdateRequiredHints();
    editor_->RefreshHints();
    RefreshReferenceItems();
    word_count_label_->setText(QString::number(CountWords()) + " words");
}

void MainWindow::OnPreviewUpdated(const pf::PreviewUpdate& update) {
    if (shutting_down_) return;
    // The session's preview gate already dropped stale/foreign results; the
    // identity is still asserted here so the pane never shows a PDF from a
    // different project or revision.
    if (!controller_->has_project()) return;
    if (update.project_id != controller_->session().state().id()) return;
    if (update.revision != controller_->current_revision()) return;

    const int revision = static_cast<int>(update.revision.value);
    if (update.success && update.pdf.valid()) {
        build_button_->setText("✓ Built");
        build_button_->setStyleSheet(QString(
            "QPushButton { background: %1; color: white; border: none;"
            " font-weight: 600; }")
            .arg(theme::kOk));
        build_state_label_->setText(
            QString("Build: ✓ Revision %1").arg(revision));
        current_pdf_path_ = ToQ(update.pdf.path.string());
        last_build_revision_ = revision;
        RefreshPreview();
    } else {
        build_button_->setText("! Build failed");
        build_state_label_->setText("Build: failed");
        build_button_->setStyleSheet(QString(
            "QPushButton { background: %1; color: white; border: none;"
            " font-weight: 600; }")
            .arg(theme::kError));
    }
    QTimer::singleShot(3000, this, [this]() {
        build_button_->setText("Build  ▶");
        build_button_->setStyleSheet("");
    });
}

void MainWindow::OnDiagnosticsUpdated(const QList<QString>& problems) {
    (void)problems;  // rendered by ProblemsPanel via its own data path
}

void MainWindow::OnBuildStatusChanged(const QString& status) {
    build_state_label_->setText("Build: " + status);
}

void MainWindow::ZoomPreviewForTest(double zoom, double scroll_x,
                                    double scroll_y) {
    // zoom <= 0 keeps the current zoom and only scrolls.
    if (zoom > 0.0) preview_->SetZoom(zoom);
    preview_->ScrollTo(scroll_x, scroll_y);
}

void MainWindow::RefreshPreview() {
    if (current_pdf_path_.isEmpty()) return;
    // Keep the current zoom and scroll position; only the page changes.
    preview_->SetDocument(current_pdf_path_);
}

}  // namespace pf::gui
