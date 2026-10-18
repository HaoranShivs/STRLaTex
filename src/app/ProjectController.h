#pragma once
// ProjectController：UI 与领域层之间的 Qt 侧适配器。
// 持有 ProjectSession，把领域事件重新发出为 Qt 信号。
// （Qt 隔离规则：领域层对本文件一无所知。）

#include <QObject>
#include <QString>
#include <memory>

#include "build/BuildCoordinator.h"
#include "build/BuildEvent.h"
#include "numbering/CitationNumberResolver.h"
#include "numbering/DocumentNumberer.h"
#include "project/PreviewUpdate.h"
#include "project/ProjectSession.h"

class QTimer;

namespace pf {
struct VerifiedTemplatePackage;
}

namespace pf::gui {

class ProjectController : public QObject {
    Q_OBJECT

  public:
    explicit ProjectController(QObject* parent = nullptr);
    ~ProjectController() override;

    ProjectSession& session() {
        return *session_;
    }
    const ProjectSession& session() const {
        return *session_;
    }
    bool has_project() const {
        return session_ && session_->lifecycle_state() == LifecycleState::Open;
    }
    ProjectRevision current_revision() const {
        return session_ ? session_->current_revision() : ProjectRevision{};
    }
    PersistenceState persistence_state() const {
        return session_ ? session_->persistence_state() : PersistenceState::Clean;
    }
    PreviewState preview_state() const {
        return session_ ? session_->preview_state() : PreviewState::NoPreview;
    }

    // 生命周期操作（异步工作留在 session 内部）。
    bool NewProject(const QString& dir, std::string* error = nullptr);
    bool OpenProject(const QString& dir);
    bool OpenProjectWithRecovery(const QString& dir, bool* recovered);
    void CloseProject();

    void Save();
    // 阻塞直到排队的保存全部写入（仅供测试／工具使用）。
    void FlushSaves();
    void StartAutosave();
    void StopAutosave();

    // 编辑操作——根据 UI 输入构造 EditCommand。
    EditResult SetTitle(const QString& text);
    EditResult SetAbstract(const QString& text);
    // "Alice, Bob" -> 作者；空字符串表示清空。
    EditResult SetAuthorsText(const QString& comma_separated);
    // "University A; University B" -> 单位；按作者行内 ¹²³ 标记的顺序
    // 重新关联已有作者（作者原有的有效关联保持不变）。
    EditResult SetAffiliationsText(const QString& semicolon_separated);

    // 图形化的作者 <-> 机构绑定：关联或取消关联某位作者与某个机构，
    // 同时保持文档的机构顺序。
    EditResult SetAuthorAffiliation(size_t author_index, const AffiliationId& affiliation, bool linked);
    EditResult SetKeywordsText(const QString& comma_separated);
    EditResult InsertSection(const QString& title);
    EditResult InsertSectionAfter(const NodeId& anchor, const QString& title);
    EditResult InsertSubsection(size_t section_index, const QString& title);
    EditResult InsertSubsectionAfter(const NodeId& anchor, const QString& title);
    EditResult InsertSubsubsection(size_t section_index, size_t subsection_index, const QString& title);
    EditResult InsertSubsubsectionAfter(const NodeId& anchor, const QString& title);
    EditResult RenameSubsubsection(const NodeId& subsubsection, const QString& title);
    EditResult DeleteSubsubsection(const NodeId& subsubsection);
    EditResult InsertParagraph(const NodeId& parent, const QString& text);
    EditResult InsertParagraphAfter(const NodeId& anchor, const QString& text);
    EditResult InsertEquation(const NodeId& parent, const QString& math, bool numbered,
                              const QString& label = QString());
    EditResult InsertEquationAfter(const NodeId& anchor, const QString& math, bool numbered,
                                   const QString& label = QString());
    EditResult InsertFigureAfter(const NodeId& anchor, const QString& image_path);
    EditResult ReplaceFigureAsset(const NodeId& figure, const QString& image_path);
    EditResult InsertTableAfter(const NodeId& anchor, size_t rows = 3, size_t columns = 3, bool has_header_row = true);
    EditResult InsertListAfter(const NodeId& anchor, ListStyle style = ListStyle::Bullet);
    EditResult InsertListItem(const NodeId& list, size_t index, const InlineContent& content);
    EditResult EditListItem(const NodeId& list, size_t index, const InlineContent& content);
    EditResult DeleteListItem(const NodeId& list, size_t index);
    EditResult MoveListItem(const NodeId& list, size_t from, size_t to);
    EditResult SetListStyle(const NodeId& list, ListStyle style);
    EditResult InsertFootnote(const NodeId& paragraph, std::optional<size_t> at_index, const InlineContent& body);
    EditResult EditFootnote(const NodeId& paragraph, size_t at_index, const InlineContent& body);
    EditResult DeleteFootnote(const NodeId& paragraph, size_t at_index);
    EditResult InsertTheoremAfter(const NodeId& anchor, TheoremKind kind, const InlineContent& title,
                                  bool numbered, const QString& label = QString());
    EditResult EditTheorem(const NodeId& theorem, const InlineContent& title, const InlineContent& body,
                           bool numbered, const QString& label, TheoremKind kind = TheoremKind::Theorem);
    EditResult InsertCodeAfter(const NodeId& anchor, const QString& language, const QString& code,
                               bool line_numbers, const QString& label = QString());
    EditResult EditCode(const NodeId& code, const QString& language, const QString& text,
                        const InlineContent& caption, bool line_numbers, const QString& label);
    // InlineEditor 使用的结构化提交：粗体／斜体片段、引文、
    // 交叉引用和行内公式都能完整往返。正文进入文档的数据路径
    // 有且仅有一条（引用方案 §5）。
    EditResult EditParagraphRich(const NodeId& paragraph, const InlineContent& content);
    EditResult EditEquation(const NodeId& equation, const QString& math, bool numbered, const QString& label);
    EditResult RenameSection(const NodeId& section, const QString& title);
    EditResult RenameSubsection(const NodeId& subsection, const QString& title);
    EditResult EditCaption(const NodeId& block, const QString& caption);
    EditResult EditCaptionRich(const NodeId& block, const InlineContent& caption);
    EditResult SetTableCell(const NodeId& table, size_t row, size_t column, const InlineContent& content);
    EditResult InsertTableRow(const NodeId& table, size_t index);
    EditResult DeleteTableRow(const NodeId& table, size_t index);
    EditResult InsertTableColumn(const NodeId& table, size_t index, ColumnAlignment alignment = ColumnAlignment::Left);
    EditResult DeleteTableColumn(const NodeId& table, size_t index);
    EditResult SetTableColumnAlignment(const NodeId& table, size_t column, ColumnAlignment alignment);
    EditResult SetTableHeaderRow(const NodeId& table, bool has_header_row);
    EditResult EditFigureWidth(const NodeId& figure, FigureWidth width);
    // 单栏与双栏图。无论使用哪种模板都会保存；只有双栏模板
    // 才会体现差异（通栏浮动体）。
    EditResult EditFigureSpan(const NodeId& figure, bool double_column);
    EditResult EditFigureLabel(const NodeId& figure, const QString& label);
    EditResult EditTableLabel(const NodeId& table, const QString& label);
    EditResult DeleteBlock(const NodeId& block);
    EditResult DeleteNode(const NodeId& node);
    EditResult MoveNode(const NodeId& node, int direction);

    // 拖放重排：把 `node` 直接放到 `anchor` 之后（`anchor` 可以是块、
    // 子节或节）。其他位置的文档顺序保持不变。
    EditResult MoveNodeAfter(const NodeId& node, const NodeId& anchor);
    EditResult DeleteSection(size_t index);
    void Undo();
    void Redo();
    void ChangeTemplate(const QString& template_id);
    Result<void> InstallTemplate(const ::pf::VerifiedTemplatePackage& package,
                                 const QString& profile_id);
    void RequestBuild(bool manual = true);
    void CancelBuild();

    // 引用方案 §3 / WP-01：编辑器 pill 绘制与 picker 共用的 resolver。
    // 根据实时文档与导入的参考文献构建；命中缓存时返回同一个 shared_ptr，
    // 供 BlockEditor 通过指针比较跳过全量刷新。没有打开项目时为 nullptr。
    std::shared_ptr<const pf::CitationNumberResolver> CitationResolver() const;
    // 兼容旧调用；等价于 CitationResolver()。
    std::shared_ptr<const pf::CitationNumberResolver> CitationNumbers() const;
    // picker / References 面板的条目视图（author/year/title/venue/cited/number）。
    std::vector<pf::CitationView> CitationViews() const;

    // WP-03：与 PDF 一致的文档编号（section/图表/公式），按 revision 缓存。
    std::shared_ptr<const pf::DocumentNumberer> Numbering() const;
    std::vector<pf::CrossReferenceItem> CrossReferences() const;

    // 参考文献。导入结果（条目数、重复 key）会暴露给 UI，
    // 因此重复项绝不会无声无息地消失。
    BibliographyImportResult ImportBibliographyText(const QString& bibtex);
    BibliographyImportResult AddBibliographyEntry(const BibEntry& entry);
    CitationSearchResult SearchCitations(const QString& query) const;

  signals:
    // Successful project navigation, including close. Views discard old artifacts.
    void projectChanged();
    void documentChanged();
    void buildStatusChanged(QString phase_text);
    // buildFinished(bool, pdf_path) 的类型化替代：携带 PDF 的
    // project/build/revision 身份标识，使视图能够拒绝任何
    // 不再属于当前文档的内容。
    void previewUpdated(const pf::PreviewUpdate& update);
    // 结构化的 build 诊断（Build 诊断方案 §36-§37）：当前 build
    // 被接受且最终的 BuildResult。GUI 绝不解析日志文本；
    // ProblemsPanel 直接消费这些值对象。
    void buildCompleted(const pf::BuildResult& result);
    // 当前 build 的一条 build 日志事件，在它运行期间流式发出（§4）。
    void buildEvent(const pf::BuildEvent& event);
    // P0-06：结构化的保存结果——写入的 revision、它是否真正落地
    //（Saved）、是否被更新的保存取代（Superseded）或者失败。视图
    // 始终从 persistence_state() 渲染状态，绝不依据本信号的 bool。
    void saveFinished(bool success, QString detail);
    // P0-06：权威状态投影——在每次可能改变 persistence/preview/revision
    // 的领域状态转换之后发出。视图渲染的正是这些值。
    void stateChanged(QString persistence, QString preview, QString revision);
    void templateChanged(QString template_id);

  private:
    struct InsertionPoint {
        NodeId parent;
        std::optional<size_t> index;
    };

    void EmitDocumentChanged();
    // P0-06：唯一出口，依据权威的 session 状态发出 stateChanged。
    // 每次保存完成／编辑／生命周期变更都经过这里；
    // 任何监听者都不再维护第二个 dirty 标志。
    void EmitProjectStateChanged();
    // 在应用线程上排空 ProjectSession 的事件队列。
    void PumpEvents();
    EditCommand MakeCmd(FullEditPayload payload) const;
    std::optional<InsertionPoint> ResolveInsertionPoint(const NodeId& anchor) const;
    EditResult ExecuteAndNotify(FullEditPayload payload);
    // Undo/Redo 共用：执行历史动作、同步模板选择并发出状态变化。
    void ApplyHistory(bool redo);

    // WP-13：派生数据缓存。key = (ProjectRevision, bibliography revision)。
    // 两个 revision 覆盖了文档编辑与文献导入两条失效路径。
    void InvalidateDerivedCaches();
    mutable std::shared_ptr<const pf::CitationNumberResolver> citation_cache_;
    mutable ProjectRevision citation_cache_revision_{};
    mutable std::uint64_t citation_cache_bib_revision_ = 0;
    mutable bool citation_cache_valid_ = false;
    mutable std::shared_ptr<const pf::DocumentNumberer> numbering_cache_;
    mutable ProjectRevision numbering_cache_revision_{};
    mutable bool numbering_cache_valid_ = false;

    std::unique_ptr<ProjectSession> session_;
    QTimer* pump_timer_ = nullptr;
    bool shutting_down_ = false;
};

} // namespace pf::gui

Q_DECLARE_METATYPE(pf::PreviewUpdate)
// 跨越异步边界进入 GUI 的值对象（方案 §36）。它们由队列连接
// 复制传递，这正是方案所要求的隔离：worker 绝不触碰任何 widget。
Q_DECLARE_METATYPE(pf::BuildResult)
Q_DECLARE_METATYPE(pf::BuildEvent)
