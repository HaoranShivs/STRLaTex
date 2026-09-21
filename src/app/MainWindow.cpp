#include "app/MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QElapsedTimer>
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
  // GUI 排版统一放在 Theme.h：应用字体是整个界面框架的基准字体，
  // 且没有任何样式表会用 font-size 覆盖各控件自身的 setFont()
  // （UI 方案 §7）。必须在任何控件创建之前设置，以便它们全部继承。
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
  // P0-06：由权威的 persistence/preview/revision 投影驱动状态标签；
  // UI 中不保留第二份 dirty 标志。
  connect(controller_, &ProjectController::stateChanged, this,
          &MainWindow::RenderProjectState);
  // Problems 与 Build Log 直接消费结构化流水线（Build Diagnostics 方案
  // §37）：MainWindow 只负责连接各组件，绝不解析日志、计算源码映射或
  // 创建 Diagnostic。
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

// ---------------- UI 构建 ----------------

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
    // P0-01：最近项目入口同样属于破坏性导航，必须走同一个守卫，
    // 绝不能直接切换项目。
    OpenProjectDir(path);
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
    // P0-04（第二道检查）：preview 会读取该路径，因此要通过项目信任
    // 边界解析它，避免畸形的 asset 路径让 GUI 打开项目之外的文件。
    auto resolved = pf::ResolveUntrustedProjectPath(
        controller_->session().paths().assets_dir, metadata->relative_path);
    if (!resolved.ok())
      return QString();
    return ToQ(resolved.value().string());
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
  main_splitter_->setCollapsible(1, false); // 编辑器永不折叠

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

  // 专注模式开关（UI 方案 §11）：一个低调的文字控件，视觉上绝不比
  // Build 按钮更抢眼，用于在长时间写作时隐藏侧边面板。
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
    // 隐藏大纲与 preview+problems 列；编辑器占据整个宽度
    // （QSplitter 会把空间重新分配给可见面板）。
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
  file_menu->addAction("&Close Project", this, &MainWindow::OnCloseProject);
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
  // 来自 InlineEditor 行的 rich commit：标记、引用、交叉引用与行内公式
  // 以 InlineContent 形式到达（方案 §4.1）。正文文本进入文档只有唯一
  // 路径——旧的 ParagraphEdited /「[cite:key]」文本编码已被移除
  // （引用方案 §5）。
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
          // 没有锚点：正文尚无任何块，此时唯一有意义的插入
          // 就是第一个 section。
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
  // 旧有的 InsertCitationRequested / InsertCrossRefRequested 迂回路径已
  // 移除（引用方案 §5）：所有引用都通过聚焦行的 InlineEditor 进入，
  // 并作为 rich content 提交。
  connect(outline_, &OutlinePanel::NodeActivated, this,
          [this](QString node) { editor_->RevealNode(node); });
  // 反向同步（UI 方案 §10）：光标移动 → 大纲高亮所属 section，
  // 因此长稿件始终能显示你所在的位置。
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
            // 与工具栏选择器相同的 rich 路径：在该行光标处插入
            // Citation 对象并立即提交。
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

// ---------------- 动作 ----------------

void MainWindow::OnNewProject() {
  // P0-01：先弹出目录选择器（这样取消不会有任何代价），但在当前项目
  // 被替换之前先执行未保存更改守卫。所有破坏性导航共用这一个守卫。
  QString dir =
      QFileDialog::getExistingDirectory(this, "New Project Directory");
  if (dir.isEmpty())
    return;
  if (std::filesystem::exists(dir.toStdString() + "/project.paper")) {
    QMessageBox::warning(this, "PaperForge",
                         "Directory already contains a project.");
    return;
  }
  if (MaybeSaveBeforeDestructiveNavigation() ==
      DestructiveNavigationDecision::Cancel) {
    return;
  }
  std::string error;
  if (!controller_->NewProject(dir, &error)) {
    QMessageBox::critical(
        this, "PaperForge",
        "Failed to create project: " +
            (error.empty() ? QString("unknown error")
                           : QString::fromStdString(error)));
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
  RenderProjectState();  // P0-06：新项目从一开始就是 Dirty。
  statusBar()->showMessage("Created project: " + dir);
}

void MainWindow::OnCloseProject() {
  // P0-01：与其他破坏性导航相同的守卫。只有当用户的工作已安全
  // （Clean、被显式丢弃或已保存）时，项目才真正关闭。
  if (!controller_->has_project())
    return;
  if (MaybeSaveBeforeDestructiveNavigation() ==
      DestructiveNavigationDecision::Cancel) {
    return;
  }
  controller_->StopAutosave();
  controller_->CloseProject();
  ShowWorkspace(false);
  RenderProjectState();
  statusBar()->showMessage("Project closed");
}

void MainWindow::OnOpenProject() {
  QString dir =
      QFileDialog::getExistingDirectory(this, "Open Project Directory");
  if (dir.isEmpty())
    return;
  OpenProjectDir(dir);
}

bool MainWindow::OpenProjectDir(const QString &dir) {
  // P0-01：打开操作会切换项目，因此先对当前项目执行守卫。
  // 必须绕过它的程序化调用方直接走
  // MaybeSaveBeforeDestructiveNavigation。
  if (MaybeSaveBeforeDestructiveNavigation() ==
      DestructiveNavigationDecision::Cancel) {
    return false;
  }
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
  RenderProjectState();  // P0-06：渲染已加载项目的真实状态。
  statusBar()->showMessage("Opened project: " + dir);
  return true;
}

// ---------------- P0-01：未保存更改守卫 ----------------

bool MainWindow::WaitForUserSaveCompletion(int timeout_ms) {
  if (!controller_->has_project())
    return true;
  auto& session = controller_->session();
  QElapsedTimer timer;
  timer.start();
  // save worker 会投递完成事件；泵送应用事件队列可应用该事件，
  // 并更新权威的持久化状态。
  while (timer.elapsed() < timeout_ms) {
    session.ProcessApplicationEvents();
    if (session.persistence_state() != PersistenceState::Saving)
      return true;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return session.persistence_state() != PersistenceState::Saving;
}

MainWindow::DestructiveNavigationDecision
MainWindow::MaybeSaveBeforeDestructiveNavigation() {
  // 固定顺序（整改方案 P0-01）：
  //   1. 提交用户正在编辑的行，使 snapshot 完整；
  //   2. 泵送已经完成的异步事件；
  //   3. 读取权威的持久化状态，并据此询问。
  if (editor_)
    editor_->CommitFocused();
  if (!controller_->has_project())
    return DestructiveNavigationDecision::Proceed;
  auto& session = controller_->session();
  session.ProcessApplicationEvents();

  const auto ask = [this](const QString& text, const QString& informative,
                          const QString& save_label,
                          const QString& discard_label,
                          const QString& cancel_label) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("PaperForge");
    box.setText(text);
    box.setInformativeText(informative);
    QPushButton* primary = box.addButton(save_label, QMessageBox::AcceptRole);
    QPushButton* discard =
        box.addButton(discard_label, QMessageBox::DestructiveRole);
    box.addButton(cancel_label, QMessageBox::RejectRole);
    box.setDefaultButton(primary);
    box.exec();
    if (box.clickedButton() == primary)
      return 0;
    if (box.clickedButton() == discard)
      return 1;
    return 2;  // 取消
  };

  switch (session.persistence_state()) {
    case PersistenceState::Clean:
      return DestructiveNavigationDecision::Proceed;

    case PersistenceState::Dirty: {
      const int choice = ask("This project has unsaved changes.",
                             "Save them before continuing?", "Save",
                             "Discard", "Cancel");
      if (choice == 2)
        return DestructiveNavigationDecision::Cancel;
      if (choice == 1)
        return DestructiveNavigationDecision::Proceed;  // 显式丢弃
      // 保存：入队并等待对应的 revision 落盘。
      auto result = session.Save();
      if (result.status != SaveResult::Status::Queued) {
        QMessageBox::critical(this, "PaperForge",
                              "Could not save: " + ToQ(result.detail));
        return DestructiveNavigationDecision::Cancel;
      }
      if (!WaitForUserSaveCompletion()) {
        QMessageBox::warning(
            this, "PaperForge",
            "The save is still running. Cancel the navigation and try again.");
        return DestructiveNavigationDecision::Cancel;
      }
      RenderProjectState();
      // 只有项目现在确实为 Clean 才继续；保存失败（或被更新的编辑取代）
      // 时必须把用户留在此处。
      return session.persistence_state() == PersistenceState::Clean
                 ? DestructiveNavigationDecision::Proceed
                 : DestructiveNavigationDecision::Cancel;
    }

    case PersistenceState::Saving: {
      const int choice = ask("A save is still running.",
                             "Wait for it to finish before continuing?",
                             "Wait", "Discard", "Cancel");
      if (choice == 2)
        return DestructiveNavigationDecision::Cancel;
      if (choice == 1)
        return DestructiveNavigationDecision::Proceed;
      if (!WaitForUserSaveCompletion()) {
        QMessageBox::warning(this, "PaperForge",
                             "The save did not finish in time.");
        return DestructiveNavigationDecision::Cancel;
      }
      RenderProjectState();
      return DestructiveNavigationDecision::Proceed;
    }

    case PersistenceState::SaveFailed: {
      const int choice = ask("The last save failed.",
                             "Retry saving before continuing?", "Retry Save",
                             "Discard", "Cancel");
      if (choice == 2)
        return DestructiveNavigationDecision::Cancel;
      if (choice == 1)
        return DestructiveNavigationDecision::Proceed;
      auto result = session.Save();
      if (result.status != SaveResult::Status::Queued) {
        QMessageBox::critical(this, "PaperForge",
                              "Could not save: " + ToQ(result.detail));
        return DestructiveNavigationDecision::Cancel;
      }
      if (!WaitForUserSaveCompletion()) {
        QMessageBox::warning(this, "PaperForge",
                             "The retry did not finish in time.");
        return DestructiveNavigationDecision::Cancel;
      }
      RenderProjectState();
      return session.persistence_state() == PersistenceState::Clean
                 ? DestructiveNavigationDecision::Proceed
                 : DestructiveNavigationDecision::Cancel;
    }
  }
  return DestructiveNavigationDecision::Cancel;
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // P0-01：关闭窗口属于破坏性导航。析构函数中尽力而为的
  // CommitFocused() 已不再是保护用户工作的手段。
  if (MaybeSaveBeforeDestructiveNavigation() ==
      DestructiveNavigationDecision::Cancel) {
    event->ignore();
    return;
  }
  event->accept();
}

void MainWindow::OnSave() {
  if (!controller_->has_project())
    return;
  // 引用方案 §6：任何读取 Document 的操作都必须先冲刷聚焦行，
  // 否则 snapshot 会静默漏掉 GUI 已经显示的内容。
  editor_->CommitFocused();
  // 保存是异步的：snapshot 此刻捕获，由 save worker 写入。P0-06：
  // 标签跟随权威状态——入队成功时 session 切换为 Saving，完成处理器
  // 渲染最终结果；入队被拒绝则显示失败。
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
  // P0-06：完成事件本身只提供信息；标签会从 session 状态重新推导
  // （每次完成之后都会发出 stateChanged，从而调用 RenderProjectState）。
  if (!success && !detail.isEmpty())
    statusBar()->showMessage("Save: " + detail, 5000);
  RenderProjectState();
}

void MainWindow::RenderProjectState() {
  if (shutting_down_)
    return;
  // P0-06：单一事实来源。该标签是 ProjectSession 的
  // persistence/preview 状态与当前 revision 的纯投影。
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

// Undo/Redo 会恢复整个文档 snapshot，因此各行需要从文档重新读取
// （行结构可能相同，但文本未必）。
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
    // build 期间该按钮兼作 Cancel（方案 §34/§39）。worker 会以
    // Cancelled 结束本次尝试；它的 preview 与 Problems 结果会被 gate
    // 掉，日志则保持可见，直到被替换。
    controller_->CancelBuild();
    build_button_->setText("Cancelling \u25EF");
    return;
  }
  // 引用方案 §6：Commit → 提交路径同步执行
  // （ParagraphContentEdited → EditParagraphRich → EditingSystem →
  // Document 此时已持有新内容且 revision 已递增）→ 之后才 RequestBuild，
  // 这样 build snapshot 绝不会落后于用户所见——正是这个失败模式曾让
  // 刚选中的 Citation 从 PDF 中消失。
  editor_->CommitFocused();
  build_button_->setText("Building ◌");
  controller_->RequestBuild(true);
}

void MainWindow::OnImportBibliography() {
  if (!controller_->has_project())
    return;
  // 先冲刷：导入会触发 validation + build，而 validation 会读取
  // Document（引用方案 §6/§9）。
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
      // 引用方案 §9：重复 key 会被显式提示，绝不静默合并。
      // BibTeX 保留最后一个定义；我们也如此。
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
    // 导入失败时，原有 bibliography 及其 references.bib 保持不变
    // （引用方案 §7）。
    QMessageBox::warning(this, "PaperForge",
                         "Failed to parse BibTeX file: nothing was "
                         "imported; the current bibliography is unchanged.");
  }
}

void MainWindow::OnChangeTemplate(const QString &template_id) {
  // 更换模板会依据 Document 重新渲染全部内容；先冲刷聚焦行
  // （引用方案 §6）。
  editor_->CommitFocused();
  controller_->ChangeTemplate(template_id);
  UpdateRequiredHints();
}

// ---------------- 刷新 ----------------

MainWindow::~MainWindow() {
  shutting_down_ = true;
  // 尽力而为：保留用户最后输入的内容。
  if (editor_)
    editor_->CommitFocused();
  // 正在销毁的控件发出的迟到信号绝不能到达此处。
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
  // Diagnostic 归属于产生它的那次 build：文档一旦继续变化，就把可见集合
  // 标记为 Outdated（方案 §49）。diagnostic 的内容不会被改写——下一次
  // build 会整体替换该集合。
  if (has_built_) {
    const auto revision = controller_->current_revision();
    problems_->SetStale(
        revision != last_built_revision_,
        static_cast<int>(revision.value > last_built_revision_.value
                             ? revision.value - last_built_revision_.value
                             : 0));
  }
  // 引用方案 §3：在重建各行之前，把文档级的 key -> number 映射交给
  // 编辑器。GUI 中的「[1]」pill 与 PDF 中的编号是同一套引用顺序策略
  // 的同一种投影。
  editor_->SetCitationNumbers(controller_->CitationNumbers());
  // 插入菜单与「/」菜单按当前模板能表达的内容进行过滤（方案 §9）。
  if (const auto *tpl = TemplateRegistry::Instance().Find(
          controller_->session().state().template_selection())) {
    editor_->SetMaxHeadingDepth(tpl->capabilities.max_heading_depth);
  } else {
    editor_->SetMaxHeadingDepth(3);
  }
  // 用户正在编辑时绝不拆除各行：重建会重新创建每个编辑控件，
  // 从而丢弃正在输入的文本。此时改为把刷新推迟到下一次提交。
  if (editor_->HasUncommittedFocus()) {
    pending_structural_refresh_ = true;
    RefreshSidePanels();
    return;
  }
  pending_structural_refresh_ = false;
  editor_->RebuildFromDocument(doc);
  RefreshSidePanels();
}

// 大纲、必填字段提示与字数统计：除编辑器行之外的一切，
// 因此任何时候运行都是安全的。
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
  // session 的 preview gate 已经丢弃了陈旧/异源的结果；这里仍然校验
  // 身份，确保该面板绝不显示来自其他项目或 revision 的 PDF。
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
    // 被取消的尝试不属于文档失败（方案 §34）：preview 继续显示
    // 最后一个有效 PDF，按钮也如实说明。
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
    // 更新的尝试可能已经接管该按钮（Cancel）；不要覆盖它的状态
    // （方案 §39）。
    if (building_)
      return;
    build_button_->setText("Build  ▶");
    build_button_->setStyleSheet("");
  });
}

void MainWindow::OnBuildCompleted(const pf::BuildResult &result) {
  if (shutting_down_)
    return;
  // 每次完成的 build 只更新一次（方案 §30）；diagnostic 已是结构化
  // 数据——这里不检查任何日志文本。
  problems_->SetDiagnostics(result.diagnostics);
  last_built_revision_ = result.revision;
  last_outcome_ = result.outcome;
  has_built_ = true;

  if (result.outcome == BuildResult::Outcome::Cancelled) {
    // 被取消的 build 既不更新 Preview 也不改变标签页可见性；
    // 只有瞬时状态会变化（方案 §34）。
    return;
  }
  // 自动切换标签页（方案 §40）：成功绝不抢走视图；文档级失败会显示
  // Problems；运行时级失败——此时没有 Problems 条目能指向具体块——
  // 则显示 Build Log。
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
    // 尝试进行中时，Build 按钮变为 Cancel
    // （方案 §39）。
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
  // 导航（方案 §25/§28/§29/§48）：优先定位到块，其次回退到 Build Log，
  // 否则不做任何事。
  if (diagnostic.location.has_block_location()) {
    const Document &doc = controller_->session().state().document();
    if (doc.ContainsNode(diagnostic.location.node)) {
      editor_->RevealNode(ToQ(diagnostic.location.node.value()));
      return;
    }
    // 该块在本次 build 之后已被删除：给出提示，绝不崩溃。
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
  // 阶段观察者是可靠的「没有 build 在进行」信号：请求在 debounce 期间
  // 被丢弃时它也会触发。只要 coordinator 回到 Idle，就撤下 Cancel 入口
  // （方案 §39）。
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
  // zoom <= 0 时保持当前缩放，只进行滚动。
  if (zoom > 0.0)
    preview_->SetZoom(zoom);
  preview_->ScrollTo(scroll_x, scroll_y);
}

void MainWindow::RefreshPreview() {
  if (current_pdf_path_.isEmpty())
    return;
  // 保持当前缩放与滚动位置；只有页面发生变化。
  preview_->SetDocument(current_pdf_path_);
}

} // namespace pf::gui
