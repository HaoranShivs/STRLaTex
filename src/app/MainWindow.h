#pragma once
// PaperForge 主窗口，遵循 GUI 设计文档：大纲 | 编辑器 | 预览
//（18% / 52% / 30%），头部放置 Build 按钮，状态栏显示保存/build
// 状态与字数。未打开项目时由欢迎页取代工作区（设计 #1、#34、#51、#80）。

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
    // 程序化打开（供测试/工具使用）：切换到工作区。
    bool OpenProjectDir(const QString& dir);
    // 程序化预览缩放/滚动（供 UI 验证工具使用）。
    void ZoomPreviewForTest(double zoom, double scroll_x = 0.0,
                            double scroll_y = 0.0);

    // P0-01：唯一的未保存更改守卫。所有破坏性导航
    //（关闭、新建、打开、最近项目、切换项目）都必须经过它，
    // 任何调用点都不得自行实现 dirty 检查。
    enum class DestructiveNavigationDecision : std::uint8_t {
        Proceed,
        Cancel,
    };
    // 固定顺序：提交聚焦行、泵出已完成的异步事件、读取
    // 权威持久化状态，然后询问用户
    //（保存 / 丢弃 / 取消，或等待 / 丢弃 / 取消，或
    // 重试保存 / 丢弃 / 取消）。仅当目标 revision
    // 确实为 Clean，或用户明确选择丢弃时才继续。
    DestructiveNavigationDecision MaybeSaveBeforeDestructiveNavigation();
    // 阻塞直至当前 revision 的在途保存落地，其间泵出事件
    // 以应用其完成结果。超时返回 false。
    bool WaitForUserSaveCompletion(int timeout_ms = 10000);

protected:
    // P0-01：关闭窗口同样是破坏性导航，必须经过同一个守卫，
    // 而不能依赖析构时的尽力而为 CommitFocused()。
    void closeEvent(QCloseEvent* event) override;

private slots:
    void OnNewProject();
    void OnOpenProject();
    // P0-01：关闭当前项目（返回欢迎页）属于破坏性导航，
    // 共用同一个未保存更改守卫。
    void OnCloseProject();
    void OnSave();
    void OnUndo();
    void OnRedo();
    void OnBuild();
    void OnImportBibliography();
    void OnChangeTemplate(const QString& template_id);

    void RefreshDocumentView();
    void RefreshSidePanels();
    // 带类型的预览事件：携带该 PDF 的 project/build/revision 身份标识。
    void OnPreviewUpdated(const pf::PreviewUpdate& update);
    // P0-06：仅报告原始写入结果；此处不设置状态标签
    //（状态标签改为从权威 session 状态渲染）。
    void OnSaveFinished(bool success, const QString& detail);
    // P0-06：保存/预览状态标签的唯一渲染器。连接到
    // ProjectController::stateChanged，并由所有影响状态的路径调用；
    // 只读取 ProjectSession::persistence_state() /
    // preview_state() / current_revision()。不存在 GUI dirty 标志。
    void RenderProjectState();
    // 已接受 build 的结构化诊断（Build Diagnostics 方案
    // §36-§37）：MainWindow 只把面板接到数据上，此处不做日志解析。
    void OnBuildCompleted(const pf::BuildResult& result);
    // 当前 build 的一条流式 build 日志事件。
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
    // 专注编辑模式（UI 方案 §11）：折叠大纲 + 预览，让编辑器
    // 在长时间写作时独占整个窗口；再次点击恢复先前的分隔条尺寸。
    void OnToggleFocusMode(bool on);
    // Problem -> Block 导航（Build Diagnostics 方案 §25/§28/§48）：
    // 聚焦所属 Block，否则回退到 Build Log；Block 已被删除时给出报告，
    // 绝不崩溃。
    void OnProblemActivated(const pf::Diagnostic& diagnostic);

    ProjectController* controller_;

    // 头部
    QPushButton* build_button_;
    QComboBox* template_combo_ = nullptr;
    QPushButton* focus_button_ = nullptr;
    // 工作区
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
    // 状态栏
    QLabel* save_state_label_;
    QLabel* build_state_label_;
    QLabel* word_count_label_;
    QString current_pdf_path_;
    int last_build_revision_ = -1;
    // 最近一次完成 build 的 revision，用于过期标记（方案 §49）。
    pf::ProjectRevision last_built_revision_;
    // 最近一次 build 的结果：OnPreviewUpdated 读取它，使 Cancelled
    // 尝试报告「cancelled」而非「failed」（方案 §34）。
    pf::BuildResult::Outcome last_outcome_ = pf::BuildResult::Outcome::Failure;
    bool has_built_ = false;
    // 从 BuildStarted 到终止事件期间为 true：此时 Build 按钮
    // 充当取消（方案 §39），Problems 显示「Building...」。
    bool building_ = false;
    // 窗口开始销毁后置位：此时必须完全忽略编辑器信号与文档刷新。
    bool shutting_down_ = false;
    // 因用户正在输入而不得不跳过刷新时置位。
    bool pending_structural_refresh_ = false;
    // 最近项目（由 QSettings 支持）
    QStringList recent_projects_;
};

}  // namespace pf::gui
