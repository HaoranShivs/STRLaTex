#include "app/BlockEditor.h"

#include <QCheckBox>
#include <QColor>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <functional>
#include <initializer_list>

#include "app/MathPreviewRenderer.h"
#include "app/math/MathRenderService.h"
#include "app/Theme.h"
#include "document/InlineText.h"
#include "math/MathValidator.h"

namespace pf::gui {

namespace {
using theme::kAccent;

QString ToQ(const std::string &s) { return QString::fromStdString(s); }
std::string ToStd(const QString &s) { return s.toStdString(); }

// 重排序使用私有 mime 类型，因此编辑器会忽略来自外部的拖拽
// （文件、文本），其他应用也会忽略我们的拖拽。
constexpr const char *kBlockMime = "application/x-paperforge-block";

// 图片预览标签。普通 QLabel 会保留给定的任意 pixmap，因此一旦图片被缩放到
// 固定框，编辑器列变窄时就会被裁剪（即用户看到的上下截断）。本标签则保留源
// pixmap，并按自身宽度重新缩放且保持宽高比，从而始终适配编辑器列并显示
// 完整图像。
class FigureImageLabel : public QLabel {
public:
  explicit FigureImageLabel(QWidget *parent = nullptr) : QLabel(parent) {
    setAlignment(Qt::AlignCenter);
    // 水平方向 Ignored：由卡片的布局决定宽度（编辑器列）。垂直方向 Minimum：
    // 高度是下限而非上限，因此布局无法把标签压缩到缩放后图像高度以下。
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    setMinimumWidth(1);
  }

  void SetSourcePixmap(const QPixmap &pixmap) {
    source_ = pixmap;
    last_width_ = -1;
    if (source_.isNull()) {
      setText(QStringLiteral("Image preview unavailable"));
      setMinimumHeight(0);
      return;
    }
    Rescale();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    Rescale();
  }

private:
  void Rescale() {
    if (source_.isNull())
      return;
    const int target = qMax(1, width());
    if (target == last_width_)
      return;
    last_width_ = target;
    const QPixmap scaled = source_.scaled(
        target, QWIDGETSIZE_MAX, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    setPixmap(scaled);
    // 将缩放后图像的高度设为硬下限。否则当列较矮时，外层 QVBoxLayout 会把
    // 标签压缩到其最小尺寸，QLabel 随后会居中裁剪 pixmap（即用户看到的上下
    // 截断）。该下限还会迫使滚动区域的宿主增高，从而让编辑器滚动而非裁剪
    // 图片。
    const int needed = qMax(120, scaled.height());
    if (minimumHeight() != needed)
      setMinimumHeight(needed);
  }

  QPixmap source_;
  int last_width_ = -1;
};

// 插入入口提供的块类型，按菜单顺序排列。
// 已被 InsertOptionsFor() 取代，后者按位置和模板能力过滤；此处仅保留用于
// 块菜单的通用列表。
struct InsertEntry {
  const char *label;
  const char *kind;
};

std::vector<InsertEntry> InsertEntries(bool body_empty) {
  if (body_empty) {
    // 文本块需要有标题作为宿主，而节本身不需要锚点，
    // 因此空正文只能从节开始。
    return {{"Section Title", "section"}};
  }
  return {{"Text", "text"},
          {"Section Title", "section"},
          {"Subsection Title", "subsection"},
          {"Equation", "equation"},
          {"Figure", "figure"},
          {"Import Table…", "table"}};
}

// 内容是一段连续散文的行：它们会按块宽度重新排布，因此硬换行粘贴的内容
// 必须被柔化（见 ReflowHardWrappedText）。
bool IsProseRole(const QString &role) {
  return role == QLatin1String("abstract") || role == QLatin1String("caption");
}

// 显示类型 -> 排版角色（UI 方案 §3）。卡片外观（边距、标签）和编辑器字体
// 共用这一个映射。
theme::BlockVisualRole RoleForKind(const QString &kind) {
  using R = theme::BlockVisualRole;
  if (kind == QLatin1String("Title"))
    return R::Title;
  if (kind == QLatin1String("Authors"))
    return R::Authors;
  if (kind == QLatin1String("Institution"))
    return R::Affiliations;
  if (kind == QLatin1String("Abstract"))
    return R::Abstract;
  if (kind == QLatin1String("Keywords"))
    return R::Keywords;
  if (kind == QLatin1String("Section Title"))
    return R::SectionTitle;
  if (kind == QLatin1String("Subsection Title"))
    return R::SubsectionTitle;
  if (kind == QLatin1String("Subsubsection Title"))
    return R::SubsubsectionTitle;
  if (kind == QLatin1String("Equation"))
    return R::EquationSource;
  return R::Body;
}

// 按角色设置的卡片内边距（UI 方案 §2、§3）：正文用纤细外观，标题周围以及
// 前置信息各条目之间保留刻意的留白。宿主布局间距为 0，因此这些边距本身
// 就是垂直节奏。
QMargins CardMarginsFor(theme::BlockVisualRole role) {
  using R = theme::BlockVisualRole;
  const int h = theme::spacing::kBlockPaddingH;
  const int v = theme::spacing::kBlockPaddingV;
  switch (role) {
  case R::Title:
    return {h, 8, h, 12};
  case R::Authors:
    return {h, 0, h, 2};
  case R::Affiliations:
    return {h, 0, h, 14};
  case R::Abstract:
    return {h, 10, h, 8};
  case R::Keywords:
    return {h, 0, h, 20};
  case R::SectionTitle:
    return {h, 16, h, 6};
  case R::SubsectionTitle:
    return {h, 10, h, 4};
  case R::SubsubsectionTitle:
    return {h, 8, h, 3};
  default:
    return {h, v, h, v};
  }
}

// 表头标签文本。Abstract 和 Keywords 保持常驻的小型大写标识（UI 方案 §8）；
// 其他标签则原样显示类型。
QString DisplayLabelFor(const QString &kind) {
  if (kind == QLatin1String("Abstract"))
    return QStringLiteral("ABSTRACT");
  if (kind == QLatin1String("Keywords"))
    return QStringLiteral("KEYWORDS");
  return kind;
}

// 子控件（编辑器、表头按钮）所属的块卡片。
QFrame *CardOf(const QObject *widget) {
  for (const QObject *p = widget; p != nullptr; p = p->parent()) {
    if (auto *frame = qobject_cast<QFrame *>(const_cast<QObject *>(p));
        frame && frame->objectName() == QLatin1String("blockCard")) {
      return frame;
    }
  }
  return nullptr;
}

// 相对 `container_kind` 允许插入的内容，依据模板的标题能力（方案 §3、§9）。
// 名称是 EditorItemKind 的机器名；MainWindow 将其映射到编辑命令。
struct InsertOption {
  const char *label;
  const char *kind;  // EditorItemKindName
  const char *group; // 菜单分组：Structure / Content
};

std::vector<InsertOption> InsertOptionsFor(bool body_empty,
                                           NodeKind container_kind,
                                           int max_heading_depth) {
  const bool in_section = container_kind == NodeKind::Section;
  const bool in_subsection = container_kind == NodeKind::Subsection;
  const bool in_subsubsection = container_kind == NodeKind::Subsubsection;

  if (body_empty) {
    // 文本块需要有标题作为宿主，而节本身不需要锚点，
    // 因此空正文只能从节开始。
    return {{"Section Title", "section", "Structure"}};
  }

  std::vector<InsertOption> options;
  // 标题只能插入到容器能够承载的层级，
  // 且仅当模板支持该深度时。
  const int container_depth = HeadingDepth(container_kind);
  if (!in_subsubsection && 1 <= max_heading_depth) {
    options.push_back({"Section Title", "section", "Structure"});
  }
  if (!in_subsubsection && 2 <= max_heading_depth) {
    options.push_back({"Subsection Title", "subsection", "Structure"});
  }
  if ((in_section || in_subsection) && 3 <= max_heading_depth) {
    options.push_back({"Subsubsection Title", "subsubsection", "Structure"});
  }
  (void)container_depth;
  options.push_back({"Text", "text", "Content"});
  options.push_back({"Equation", "equation", "Content"});
  options.push_back({"Figure", "figure", "Content"});
  options.push_back({"Import Table…", "table", "Content"});
  return options;
}

// 锚点节点所处的结构容器，以便插入菜单只提供该处模型所能表达的内容。
// 块继承其所在容器；标题自身就是一个容器。
NodeKind ContainerKindFor(const Document &doc, const QString &anchor) {
  if (anchor.isEmpty())
    return NodeKind::Section;
  auto address = LocateNode(doc, NodeId(anchor.toStdString()));
  if (!address)
    return NodeKind::Section;
  switch (address->kind) {
  case NodeKind::Section:
  case NodeKind::Paragraph:
  case NodeKind::Figure:
  case NodeKind::Table:
  case NodeKind::Equation:
    // 节内的块（或节标题本身）意味着下一行仍归属于该节。
    return NodeKind::Section;
  case NodeKind::Subsection:
    return NodeKind::Subsection;
  case NodeKind::Subsubsection:
    return NodeKind::Subsubsection;
  }
  return NodeKind::Section;
}

// 自适应内容高度、并对外暴露 / 和 @ 按键事件的编辑器。
class BlockEdit : public QPlainTextEdit {
  Q_OBJECT

public:
  void setReflowOnPaste(bool enabled) { reflow_on_paste_ = enabled; }

  explicit BlockEdit(bool single_line, QWidget *parent = nullptr)
      : QPlainTextEdit(parent), single_line_(single_line) {
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::WordWrap);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(theme::spacing::kEditorDocMargin);
    // NOTE：刻意不设空闲/「输入中」计时器。最后一次按键后几百毫秒的自动提交
    // 会让每一次输入停顿都重写文档，从而重建每一行并销毁仍在输入的文本。
    // 编辑只在失焦、Enter 和 Ctrl+Enter 时提交（设计：不做隐式内容刷新）。
    connect(this, &QPlainTextEdit::textChanged, this, [this]() {
      Resize();
      if (!loading_)
        dirty_ = true;
    });
    // 因其他任何原因导致布局变化时重新适配：宽度变化引起文本重新换行，
    // 或字体/缩放变化。
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF &) { Resize(); });
    Resize();
  }

  // 角色字体 + 比例行高（UI 方案 §5）。程序性设置：绝不会将行标脏，且每次
  // setPlainText 重新加载后都会重新应用行高，因为该操作会重置块格式。
  void SetTypography(const QFont &font, int line_height_percent) {
    line_height_percent_ = line_height_percent;
    loading_ = true;
    setFont(font);
    theme::ApplyDocumentTypography(document(), font, line_height_percent);
    loading_ = false;
    Resize();
  }

  // 从文档进行的程序性加载：绝不把行标脏，也绝不触发提交。
  void SetInitialText(const QString &text) {
    loading_ = true;
    setPlainText(text);
    ApplyLineHeight();
    loading_ = false;
    dirty_ = false;
  }

  bool IsDirty() const { return dirty_; }
  void MarkClean() { dirty_ = false; }
  void MarkDirty() { dirty_ = true; }
  // 包裹程序性格式化（对齐、字体、提示），使其不会被误认为用户输入。
  void BeginProgrammaticEdit() { loading_ = true; }
  void EndProgrammaticEdit() {
    loading_ = false;
    dirty_ = false;
  }

  // 采纳模型文本，同时大致保持光标位置不变。
  void SetTextFromModel(const QString &text) {
    loading_ = true;
    const int position = textCursor().position();
    setPlainText(text);
    ApplyLineHeight();
    QTextCursor cursor = textCursor();
    cursor.setPosition(qMin(position, text.length()));
    setTextCursor(cursor);
    loading_ = false;
    dirty_ = false;
  }

  void SetMinimumContentHeight(int px) {
    min_height_ = px;
    Resize();
  }

  // 在原始 setPlainText（Reflow Text 动作）之后恢复比例行高。调用方必须已经
  // 持有程序性编辑守卫；此处不得将其解除。
  void ReapplyLineHeight() {
    ApplyLineHeight();
    Resize();
  }

  // 精确地增长/收缩以适配换行后的文本。这里不做任何滚动：唯一的垂直滚动条
  // 位于包裹所有块的编辑器窗格上。换行后文本的像素高度。
  //
  // 这里藏着两个 Qt 陷阱。QPlainTextDocumentLayout 的 documentSize() 和
  // QTextDocument::size() 报告的是*行数*（1、2、3……），而非高度；而逐块的
  // 包围矩形只对布局实际处理过的块有意义，因此读取最后一块的底边会让粘贴的
  // 多行摘要只得到一行。用控件自身字体测量纯文本是可靠的；已布局块高之和则
  // 是字体度量无法看到的因素（制表位、逐块格式）的第二重佐证。
  qreal ContentHeight() const {
    QTextDocument *doc = document();
    const QAbstractTextDocumentLayout *layout = doc->documentLayout();
    qreal per_block = 0.0;
    for (QTextBlock block = doc->begin(); block.isValid();
         block = block.next()) {
      per_block += layout->blockBoundingRect(block).height();
    }
    const int wrap_width = qMax(1, viewport()->width());
    const qreal measured = fontMetrics()
                               .boundingRect(QRect(0, 0, wrap_width, 0),
                                             Qt::TextWordWrap, toPlainText())
                               .height();
    return qMax(measured, per_block) + 2.0 * doc->documentMargin();
  }

  void Resize() {
    const qreal doc_height = ContentHeight();
    const int target =
        qMax(qMax(static_cast<int>(qCeil(doc_height)) + 2 * frameWidth() + 6,
                  fontMetrics().height() + 12),
             min_height_);
    if (height() != target)
      setFixedHeight(target);
    updateGeometry();
  }

signals:
  void TriggerSlash();
  void CommitRequested();
  void NewBlockAfter();

protected:
  void keyPressEvent(QKeyEvent *event) override {
    // 在行首且上下文基本为空时输入「/」会打开块菜单。
    if (property("commands_enabled").toBool() &&
        event->text() == QStringLiteral("/")) {
      if (toPlainText().trimmed().isEmpty()) {
        emit TriggerSlash();
        return;
      }
    }
    // 旧的「@」引用菜单属于 [cite:key] 文本编码路径的一部分，现已移除：
    // 引用是通过工具栏选择器插入的语义对象（引用方案 §5）。
    if ((event->modifiers() & Qt::ControlModifier) &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
      emit CommitRequested();
      emit NewBlockAfter();
      return;
    }
    if (single_line_ &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
      emit CommitRequested();
      return;
    }
    QPlainTextEdit::keyPressEvent(event);
    if (document()->blockCount() >= 1)
      Resize();
  }

  void focusOutEvent(QFocusEvent *event) override {
    QPlainTextEdit::focusOutEvent(event);
    emit CommitRequested();
  }

  void resizeEvent(QResizeEvent *event) override {
    QPlainTextEdit::resizeEvent(event);
    Resize();
  }

  // 撤销从 PDF 复制时带来的硬换行，使段落可以按块宽度重新排布，而不是
  // 保持固定的列宽。
  void insertFromMimeData(const QMimeData *source) override {
    if (reflow_on_paste_ && source && source->hasText()) {
      const QString pasted = source->text();
      const QString reflowed = ToQ(pf::ReflowHardWrappedText(ToStd(pasted)));
      if (reflowed != pasted) {
        QMimeData adjusted;
        adjusted.setText(reflowed);
        QPlainTextEdit::insertFromMimeData(&adjusted);
        return;
      }
    }
    QPlainTextEdit::insertFromMimeData(source);
  }

private:
  // 在 setPlainText 重新加载丢弃块格式之后，重新应用已存储的比例行高
  // （调用方持有 loading_）。
  void ApplyLineHeight() {
    if (line_height_percent_ > 0) {
      theme::ApplyDocumentTypography(document(), document()->defaultFont(),
                                     line_height_percent_);
    }
  }

  bool single_line_ = false;
  bool loading_ = false;
  bool dirty_ = false;           // 用户已输入但尚未写入文档的内容
  bool reflow_on_paste_ = false; // 长文本行会重新排布粘贴的文本
  int min_height_ = 0;
  int line_height_percent_ = 0;
};

// 卡片表头中的「\u22ee\u22ee」抓手。拖动它会开始一次重排序；
// 块之间的间隙负责接受放置。
class DragHandle : public QWidget {
  Q_OBJECT

public:
  DragHandle(const QString &node_id, QWidget *parent)
      : QWidget(parent), node_id_(node_id) {
    setFixedWidth(18);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Drag to move this block"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(theme::kDisabledText), 1.4));
    // 抓手的两个竖列。
    const int cx = width() / 2;
    for (int dx : {-2, 2}) {
      painter.drawLine(cx + dx, 4, cx + dx, height() - 4);
    }
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      press_pos_ = event->pos();
      pressed_ = true;
    }
    QWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (!pressed_ || node_id_.isEmpty())
      return;
    if ((event->pos() - press_pos_).manhattanLength() < 6)
      return;
    pressed_ = false;

    auto *mime = new QMimeData();
    mime->setData(kBlockMime, node_id_.toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    // 一个小 pixmap，让光标显示正在搬运的内容。
    QPixmap preview(120, 18);
    preview.fill(QColor(theme::kAccentSoft));
    QPainter painter(&preview);
    painter.setPen(QColor(theme::kAccent));
    painter.drawRect(0, 0, preview.width() - 1, preview.height() - 1);
    painter.drawText(6, 13, QStringLiteral("moving block"));
    drag->setPixmap(preview);
    drag->exec(Qt::MoveAction);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    pressed_ = false;
    QWidget::mouseReleaseEvent(event);
  }

private:
  QString node_id_;
  QPoint press_pos_;
  bool pressed_ = false;
};

// 两个块之间的条带。它始终保留自身高度（因此指针到达时不会发生跳动），
// 并在悬停时显现一个插入按钮，这样添加块时无需四处寻找工具栏。最后一个块
// 之后也有一个，空正文在前置信息之后也会有一个。
class BlockGap : public QWidget {
  Q_OBJECT

public:
  BlockGap(std::function<void(QWidget *)> on_activate,
           std::function<void(const QString &)> on_drop, QWidget *parent)
      : QWidget(parent), on_activate_(std::move(on_activate)),
        on_drop_(std::move(on_drop)) {
    setAcceptDrops(true);
    setFixedHeight(theme::spacing::kBlockGap);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Insert a block here"));
    setAttribute(Qt::WA_Hover, true);
  }

protected:
  void enterEvent(QEnterEvent *event) override {
    QWidget::enterEvent(event);
    hovered_ = true;
    update();
  }

  void leaveEvent(QEvent *event) override {
    QWidget::leaveEvent(event);
    hovered_ = false;
    update();
  }

  void paintEvent(QPaintEvent *) override {
    if (!hovered_ && !drop_active_)
      return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int mid = height() / 2;
    // 横贯内容宽度的一条细线，按钮居中。
    const int inset = 12;
    QColor line(theme::kAccent);
    line.setAlpha(drop_active_ ? 255 : 70);
    painter.setPen(QPen(line, drop_active_ ? 2 : 1));
    painter.drawLine(inset, mid, width() - inset, mid);
    if (drop_active_)
      return; // 仅凭这条线即可标示放置目标

    const QPoint centre(width() / 2, mid);
    const int radius = 7;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(theme::kAccent));
    painter.drawEllipse(centre, radius, radius);
    painter.setPen(QPen(Qt::white, 1.6));
    painter.drawLine(centre.x() - 3, centre.y(), centre.x() + 3, centre.y());
    painter.drawLine(centre.x(), centre.y() - 3, centre.x(), centre.y() + 3);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (on_activate_)
      on_activate_(this);
    event->accept();
  }

  void dragEnterEvent(QDragEnterEvent *event) override {
    if (event->mimeData()->hasFormat(kBlockMime)) {
      drop_active_ = true;
      update();
      event->acceptProposedAction();
    }
  }

  void dragMoveEvent(QDragMoveEvent *event) override {
    if (event->mimeData()->hasFormat(kBlockMime)) {
      event->acceptProposedAction();
    }
  }

  void dragLeaveEvent(QDragLeaveEvent *event) override {
    drop_active_ = false;
    update();
    QWidget::dragLeaveEvent(event);
  }

  void dropEvent(QDropEvent *event) override {
    drop_active_ = false;
    update();
    if (!event->mimeData()->hasFormat(kBlockMime))
      return;
    event->acceptProposedAction();
    applyDrop(QString::fromUtf8(event->mimeData()->data(kBlockMime)));
  }

public slots:
  // 完成放置后要做什么。单独拆出，以便在不依赖 Qt 拖拽管理器（需要真实
  // 指针设备）的情况下驱动重排序。
  void applyDrop(const QString &node_id) {
    if (on_drop_)
      on_drop_(node_id);
  }

private:
  std::function<void(QWidget *)> on_activate_;
  std::function<void(const QString &)> on_drop_;
  bool hovered_ = false;
  bool drop_active_ = false;
};

} // namespace

#include "app/BlockEditor.moc"

// ---------------- BlockEditor ----------------

BlockEditor::BlockEditor(QWidget *parent) : QWidget(parent) {
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  host_ = new QWidget(scroll_);
  host_->setStyleSheet(
      QString("background: %1;").arg(theme::kEditorBackground));
  auto *host_layout = new QVBoxLayout(host_);
  // 间距 0：垂直节奏由按角色设置的卡片边距和 kBlockGap 条带承担，因此正文块
  // 排列紧凑，标题则留有呼吸空间（UI 方案 §2、§3）。
  host_layout->setContentsMargins(24, 12, 24, 20);
  host_layout->setSpacing(0);
  host_layout->addStretch(1);
  scroll_->setWidget(host_);
  outer->addWidget(scroll_);
  // 稿件列宽以 kContentWidth 为上限，并在窗格更宽时居中（专注模式，或较宽的
  // 三窗格窗口）：长行有损阅读，而这一列是阅读辅助，并非 PDF。
  scroll_->viewport()->installEventFilter(this);
  CenterContentColumn();
}

void BlockEditor::CenterContentColumn() {
  auto *host_layout = qobject_cast<QVBoxLayout *>(host_->layout());
  if (!host_layout)
    return;
  const int side =
      qMax(24, (scroll_->viewport()->width() - theme::kContentWidth) / 2);
  const QMargins current = host_layout->contentsMargins();
  if (current.left() != side || current.right() != side) {
    host_layout->setContentsMargins(side, current.top(), side,
                                    current.bottom());
  }
}

QWidget *BlockEditor::BuildFormatToolbar(InlineEditor *editor) {
  auto *bar = new QWidget(this);
  bar->setProperty("formatBar", true);
  auto *layout = new QHBoxLayout(bar);
  layout->setContentsMargins(2, 2, 2, 2);
  layout->setSpacing(4);

  const QString style =
      QString("QToolButton { background: transparent; color: %1; border: none;"
              " border-radius: 4px; padding: 2px 7px; font-weight: 600; }"
              "QToolButton:hover { background: %2; color: %3; }")
          .arg(theme::kSecondaryText, theme::kAccentSoft, theme::kAccent);

  auto *bold = new QToolButton(bar);
  bold->setText(QStringLiteral("B"));
  bold->setToolTip(QStringLiteral("Bold (Ctrl+B)"));
  bold->setAutoRaise(true);
  bold->setStyleSheet(style);
  connect(bold, &QToolButton::clicked, editor,
          [editor]() { editor->ToggleBold(); });

  auto *italic = new QToolButton(bar);
  italic->setText(QStringLiteral("I"));
  italic->setToolTip(QStringLiteral("Italic (Ctrl+I)"));
  italic->setAutoRaise(true);
  italic->setStyleSheet(QString(style) + "QToolButton { font-style: italic; }");
  connect(italic, &QToolButton::clicked, editor,
          [editor]() { editor->ToggleItalic(); });

  auto *math = new QToolButton(bar);
  math->setText(QStringLiteral("Inline Math"));
  math->setToolTip(QStringLiteral("Insert an inline equation"));
  math->setAutoRaise(true);
  math->setStyleSheet(style);
  connect(math, &QToolButton::clicked, editor,
          [editor]() { editor->BeginInlineMath(); });

  auto *citation = new QToolButton(bar);
  citation->setText(QStringLiteral("Citation"));
  citation->setToolTip(QStringLiteral("Insert a citation"));
  citation->setAutoRaise(true);
  citation->setStyleSheet(style);
  connect(citation, &QToolButton::clicked, this,
          [this, editor]() { ShowCitationPicker(editor); });

  auto *reference = new QToolButton(bar);
  reference->setText(QStringLiteral("Reference"));
  reference->setToolTip(QStringLiteral("Insert a cross reference"));
  reference->setAutoRaise(true);
  reference->setStyleSheet(style);
  connect(reference, &QToolButton::clicked, this,
          [this, editor]() { ShowReferencePicker(editor); });

  // 这些按钮绝不能获得焦点。否则点击它会先结束该行的编辑、提交格式化前的
  // 内容，并在点击处理器运行之前重建每一张卡片——于是标记落在已被销毁的
  // 编辑器上，该行也在编辑中途被重新布局（即「大块空白区域」问题）。
  bold->setFocusPolicy(Qt::NoFocus);
  italic->setFocusPolicy(Qt::NoFocus);
  math->setFocusPolicy(Qt::NoFocus);
  citation->setFocusPolicy(Qt::NoFocus);
  reference->setFocusPolicy(Qt::NoFocus);

  layout->addWidget(bold);
  layout->addWidget(italic);
  layout->addSpacing(6);
  layout->addWidget(math);
  layout->addWidget(citation);
  layout->addWidget(reference);
  layout->addStretch(1);
  bar->setStyleSheet(QString("background: %1;").arg(theme::kEditorBackground));
  return bar;
}

void BlockEditor::ShowCitationPicker(InlineEditor *editor) {
  if (!editor)
    return;
  std::vector<PopupList::Item> items;
  for (const auto &item : reference_items_) {
    if (!item.payload.startsWith(QStringLiteral("cite:")))
      continue;
    PopupList::Item entry;
    entry.label = item.label;
    entry.detail = item.detail;
    entry.group = QStringLiteral("Citations");
    entry.payload = item.payload.mid(5);
    entry.search = item.label.toLower();
    items.push_back(std::move(entry));
  }
  if (items.empty()) {
    PopupList::Item empty;
    empty.label = QStringLiteral("No references imported");
    empty.detail = QStringLiteral("Import a .bib file first");
    items.push_back(empty);
  }

  // 引用方案 §4：选择器作用于弹出层获得焦点*之前*用户所在的光标位置。弹出层
  // 的焦点往返绝不能被误认为正文编辑的结束（BeginProtectedInsert 会抑制
  // focusOut 提交），而选中一项会立即完成语义插入 + 提交——不存在「稍后某次
  // focusOut 会提交它」这种情况。
  const QString node_id = editor->property("row_node").toString();
  const int caret = editor->textCursor().position();
  editor->BeginProtectedInsert();
  QPointer<InlineEditor> guard(editor);

  auto *popup = new PopupList(this);
  connect(
      popup, &PopupList::chosen, this,
      [this, guard, node_id, caret](QString payload) {
        const QString key = payload.trimmed();
        if (guard)
          guard->EndProtectedInsert();
        if (key.isEmpty() || !guard)
          return;
        // 将光标恢复到用户原来的位置，在那里插入引用对象，
        // 并立即提交该行完整的富文本内容。
        QTextCursor cursor(guard->document());
        cursor.setPosition(
            qMax(0, qMin(caret, guard->document()->characterCount() - 1)));
        guard->setTextCursor(cursor);
        guard->InsertCitationObject({key});
        guard->setFocus(Qt::OtherFocusReason);
        CommitInlineRow(guard.data());
        // 上面的提交会用新的引用编号重建各行；
        // 重新附着到（新的）行上，以便紧接着插入的 pill 继续输入。
        const int after = caret + 1;
        QTimer::singleShot(0, this, [this, node_id, after]() {
          for (auto &block : blocks_) {
            if (block.inline_editor && block.node_id == node_id) {
              QTextCursor c(block.inline_editor->document());
              c.setPosition(qMax(
                  0,
                  qMin(after,
                       block.inline_editor->document()->characterCount() - 1)));
              block.inline_editor->setTextCursor(c);
              block.inline_editor->setFocus(Qt::OtherFocusReason);
              return;
            }
          }
        });
      });
  auto end_protection = [guard]() {
    if (guard)
      guard->EndProtectedInsert();
  };
  connect(popup, &PopupList::dismissed, this, end_protection);
  connect(popup, &QObject::destroyed, this, end_protection);
  popup->popup(editor->mapToGlobal(QPoint(24, editor->height() + 4)), items);
}

void BlockEditor::ShowReferencePicker(InlineEditor *editor) {
  if (!editor)
    return;
  std::vector<PopupList::Item> items;
  // 交叉引用指向文档节点，因此这些条目来自上一次重建的引用列表，
  // 在那里它们被标记为 node:…。
  for (const auto &item : reference_items_) {
    if (!item.payload.startsWith(QStringLiteral("xref:")))
      continue;
    PopupList::Item entry;
    entry.label = item.label;
    entry.detail = item.detail;
    entry.group = QStringLiteral("Cross References");
    entry.payload = item.payload.mid(5);
    entry.search = item.label.toLower();
    items.push_back(std::move(entry));
  }
  if (items.empty()) {
    PopupList::Item empty;
    empty.label = QStringLiteral("Nothing to reference yet");
    empty.detail = QStringLiteral("Add a section, figure or equation first");
    items.push_back(empty);
  }
  const QString node_id = editor->property("row_node").toString();
  const int caret = editor->textCursor().position();
  editor->BeginProtectedInsert();
  QPointer<InlineEditor> guard(editor);
  auto *popup = new PopupList(this);
  connect(
      popup, &PopupList::chosen, this,
      [this, guard, node_id, caret](QString payload) {
        const QString target = payload.trimmed();
        if (guard)
          guard->EndProtectedInsert();
        if (target.isEmpty() || !guard)
          return;
        QTextCursor cursor(guard->document());
        cursor.setPosition(
            qMax(0, qMin(caret, guard->document()->characterCount() - 1)));
        guard->setTextCursor(cursor);
        guard->InsertCrossReferenceObject(target);
        guard->setFocus(Qt::OtherFocusReason);
        CommitInlineRow(guard.data());
        const int after = caret + 1;
        QTimer::singleShot(0, this, [this, node_id, after]() {
          for (auto &block : blocks_) {
            if (block.inline_editor && block.node_id == node_id) {
              QTextCursor c(block.inline_editor->document());
              c.setPosition(qMax(
                  0,
                  qMin(after,
                       block.inline_editor->document()->characterCount() - 1)));
              block.inline_editor->setTextCursor(c);
              block.inline_editor->setFocus(Qt::OtherFocusReason);
              return;
            }
          }
        });
      });
  auto end_protection = [guard]() {
    if (guard)
      guard->EndProtectedInsert();
  };
  connect(popup, &PopupList::dismissed, this, end_protection);
  connect(popup, &QObject::destroyed, this, end_protection);
  popup->popup(editor->mapToGlobal(QPoint(24, editor->height() + 4)), items);
}

// 立即提交一个富文本行：选择器不得依赖稍后的 focusOut。文档通过
// ParagraphContentEdited 同步更新。
void BlockEditor::CommitInlineRow(InlineEditor *editor) {
  if (!editor)
    return;
  for (auto &block : blocks_) {
    if (block.inline_editor != editor)
      continue;
    const InlineContent content = editor->Content();
    if (content == block.committed_content) {
      editor->MarkClean();
      return;
    }
    block.committed_content = content;
    block.committed_text = ToQ(pf::InlineToPlainText(content));
    editor->MarkClean();
    emit ParagraphContentEdited(block.node_id, content);
    emit RowCommitted();
    return;
  }
}

bool BlockEditor::InsertCitationIntoParagraph(const QString &node_id,
                                              const QString &citation_key,
                                              int insert_offset) {
  for (auto &block : blocks_) {
    if (block.node_id != node_id || !block.inline_editor)
      continue;
    InlineEditor *editor = block.inline_editor;
    if (insert_offset >= 0) {
      QTextCursor cursor(editor->document());
      cursor.setPosition(
          qMin(insert_offset, editor->document()->characterCount() - 1));
      editor->setTextCursor(cursor);
    }
    editor->InsertCitationObject({citation_key});
    editor->setFocus(Qt::OtherFocusReason);
    CommitInlineRow(editor);
    return true;
  }
  return false;
}

void BlockEditor::SetCitationNumbers(
    std::shared_ptr<const pf::CitationNumberResolver> numbers) {
  if (citation_numbers_ == numbers)
    return;
  citation_numbers_ = std::move(numbers);
  for (auto &block : blocks_) {
    if (block.inline_editor) {
      block.inline_editor->SetCitationNumbers(citation_numbers_);
    }
  }
}

std::map<QString, QString> BlockEditor::CrossReferenceLabels() const {
  std::map<QString, QString> labels;
  for (const auto &item : reference_items_) {
    if (!item.payload.startsWith(QStringLiteral("xref:")))
      continue;
    const QString node = item.payload.mid(5);
    if (!node.isEmpty() && !item.label.isEmpty()) {
      labels[node] = item.label;
    }
  }
  return labels;
}

QWidget *BlockEditor::MakeTextCard(const QString &node_id,
                                   const InlineContent &content,
                                   const QString &outline_key) {
  auto *card = MakeCard(node_id, QStringLiteral("Text"),
                        QStringLiteral("paragraph"), true);
  auto *card_layout = qobject_cast<QVBoxLayout *>(card->layout());

  auto *editor = new InlineEditor(card);
  // 顺序很重要（UI 方案 §6）：在加载内容*之前*先建立 12pt/150% 的字体环境，
  // 因为 InsertMathObject 在插入时依据 document()->defaultFont() 来确定行内
  // 数学公式的尺寸与对齐。
  editor->SetBodyTypography(theme::EditorFont(theme::BlockVisualRole::Body),
                            theme::typography::kBodyLineHeight);
  editor->SetContent(content);
  editor->setProperty("row_node", node_id);
  editor->setProperty("row_focus_key", node_id);
  editor->setProperty("row_outline_key", outline_key);
  editor->setPlaceholderText(
      QStringLiteral("Write text…  Ctrl+Enter adds a new block"));
  // Pill 使用文档范围的编号/标签映射进行渲染（引用方案 §3）：行内的 Citation
  // 对象保持语义化，仅其绘制依赖这些映射。
  editor->SetCitationNumbers(citation_numbers_);
  editor->SetCrossReferenceLabels(CrossReferenceLabels());
  editor->installEventFilter(this); // 焦点状态 + 大纲同步

  connect(editor, &InlineEditor::Committed, this, [this, editor, node_id]() {
    for (auto &block : blocks_) {
      if (block.inline_editor != editor)
        continue;
      const InlineContent content = editor->Content();
      if (content == block.committed_content) {
        editor->MarkClean();
        emit RowCommitted();
        return;
      }
      block.committed_content = content;
      editor->MarkClean();
      emit ParagraphContentEdited(node_id, content);
      emit RowCommitted();
      return;
    }
  });
  connect(editor, &InlineEditor::NewBlockAfter, this, [this, node_id]() {
    emit InsertBlockRequested(QStringLiteral("text"), node_id);
  });

  card_layout->addWidget(editor);
  // 格式条属于块的装饰，而非其内容：它在悬停/聚焦时出现，使空闲的段落读起来
  // 就是纯文本（UI 方案 §2）。以属性形式存储，方便 UpdateCardState 切换。
  QWidget *format_bar = BuildFormatToolbar(editor);
  card_layout->addWidget(format_bar);
  card->setProperty("format_bar", QVariant::fromValue(format_bar));
  format_bar->setVisible(false);

  Block block;
  block.node_id = node_id;
  block.kind = QStringLiteral("Text");
  block.card = card;
  block.inline_editor = editor;
  block.commit_role = QStringLiteral("paragraph");
  block.committed_text = ToQ(pf::InlineToPlainText(content));
  block.committed_content = content;
  block.outline_key = outline_key;
  blocks_.push_back(std::move(block));
  return card;
}

QWidget *BlockEditor::MakeEquationCard(const QString &node_id,
                                       const pf::EquationBlock &equation,
                                       const QString &outline_key) {
  // 设计 §4：公式行拥有 LaTeX Source、Preview、Numbered 和 Label。
  // 用户始终只编辑数学正文。
  auto *card = MakeCard(node_id, QStringLiteral("Equation"),
                        QStringLiteral("equation"), true);
  auto *card_layout = qobject_cast<QVBoxLayout *>(card->layout());

  auto *source = NewEditor(card, ToQ(equation.expression.latex), 2,
                           theme::BlockVisualRole::EquationSource);
  source->setPlaceholderText(
      QStringLiteral("LaTeX body, e.g. \\frac{\\partial u}{\\partial t}"));
  source->setProperty("row_node", node_id);
  source->setProperty("row_focus_key", node_id);
  source->setProperty("row_outline_key", outline_key);
  source->setProperty("commands_enabled", true);
  if (auto *block_edit = qobject_cast<BlockEdit *>(source)) {
    block_edit->setReflowOnPaste(false);
    block_edit->MarkClean();
  }

  auto *preview = new QLabel(card);
  preview->setAlignment(Qt::AlignCenter);
  preview->setMinimumHeight(56);
  preview->setStyleSheet(
      QString("background: %1; border: 1px solid %2; border-radius: 6px;"
              " color: %3;")
          .arg(theme::kEditorBackground, theme::kDivider,
               theme::kSecondaryText));
  card_layout->addWidget(preview);

  auto *status = new QLabel(card);
  status->setWordWrap(true);
  card_layout->addWidget(status);

  auto *controls = new QWidget(card);
  auto *controls_layout = new QHBoxLayout(controls);
  controls_layout->setContentsMargins(0, 0, 0, 0);
  controls_layout->setSpacing(6);
  auto *numbered = new QCheckBox(QStringLiteral("Numbered"), controls);
  numbered->setChecked(equation.numbered);
  numbered->setProperty("row_node", node_id);
  numbered->setToolTip(
      QStringLiteral("Numbered equations get an equation number and a label"));
  auto *label_caption = new QLabel(QStringLiteral("Label"), controls);
  label_caption->setStyleSheet(
      QString("color: %1;").arg(theme::kSecondaryText));
  auto *label_edit = new QLineEdit(controls);
  label_edit->setPlaceholderText(QStringLiteral("eq:energy"));
  label_edit->setText(ToQ(equation.label));
  label_edit->setProperty("row_node", node_id);
  label_edit->setToolTip(
      QStringLiteral("LaTeX label used by cross references, e.g. eq:energy"));
  controls_layout->addWidget(numbered);
  controls_layout->addWidget(label_caption);
  controls_layout->addWidget(label_edit, 1);
  card_layout->addWidget(controls);

  // P0-07：逐卡片的异步渲染状态。`card_id` 每张卡片唯一，并向服务注册；
  // generation 用于防范在同一卡片已有更新请求之后才到达的应答。
  const QString card_id = QStringLiteral("equation-card-") + node_id;
  // generation 存放在堆上：下面的服务连接会存活到本函数返回之后，
  // 因此捕获栈局部变量的引用会让 lambda 读取已释放的内存
  // （ASan: stack-use-after-return）。
  auto preview_generation = std::make_shared<std::uint64_t>(0);

  // 源码变化 -> 校验 -> 请求渲染 -> 预览（设计 §7）。
  auto refresh = [preview, status, card_id, preview_generation](
                     const QString &latex) {
    const pf::MathValidation validation =
        pf::ValidateMath(latex.toStdString(), pf::MathFlavor::Display);
    if (validation.invalid()) {
      status->setText(QStringLiteral("Invalid — %1")
                          .arg(QString::fromStdString(validation.error)));
      status->setStyleSheet(QString("color: %1;").arg(theme::kError));
    } else if (validation.pending()) {
      status->setText(QStringLiteral("Empty equation"));
      status->setStyleSheet(QString("color: %1;").arg(theme::kSecondaryText));
    } else {
      status->setText(QString());
      status->setStyleSheet(QString());
    }
    MathRenderStyle style;
    style.font_px = 22;
    // P0-07：在共享 worker 线程上渲染。卡片注册自己的客户端，以便应答被路由
    // 回该标签，而针对旧正文的迟到应答会被服务的 generation 检查丢弃。
    preview->setText(QStringLiteral("…"));
    preview->setPixmap(QPixmap());
    *preview_generation = MathRenderService::Shared()->Request(
        card_id, QStringLiteral("display"), latex, style);
  };
  // P0-07：仅将异步结果应用到本卡片。服务已经丢弃了来自被取代 generation 的
  // 应答；此处的 id + generation 检查可防止被复用的卡片显示出别的公式。
  MathRenderService* math_service = MathRenderService::Shared();
  math_service->RegisterClient(card_id, preview);
  connect(math_service, &MathRenderService::mathRendered, preview,
          [preview, card_id, preview_generation](
              const MathRenderResponse& response) {
            if (response.editor_id != card_id)
              return;
            if (response.generation != *preview_generation)
              return;
            const QPixmap pixmap = PixmapFromMathResult(response.result);
            if (pixmap.isNull()) {
              preview->setPixmap(QPixmap());
              preview->setText(QStringLiteral("—"));
              return;
            }
            preview->setText(QString());
            preview->setPixmap(pixmap);
          });
  refresh(ToQ(equation.expression.latex));

  auto *timer = new QTimer(card);
  timer->setSingleShot(true);
  timer->setInterval(160);
  connect(source, &QPlainTextEdit::textChanged, timer,
          [timer]() { timer->start(); });
  connect(timer, &QTimer::timeout, card,
          [source, refresh]() { refresh(source->toPlainText()); });

  Block block;
  block.node_id = node_id;
  block.kind = QStringLiteral("Equation");
  block.card = card;
  block.editor = source;
  block.commit_role = QStringLiteral("equation");
  block.committed_text = ToQ(equation.expression.latex);
  block.equation_numbered = equation.numbered;
  block.equation_label = ToQ(equation.label);
  block.outline_key = outline_key;
  blocks_.push_back(std::move(block));

  BlockEdit *block_edit = qobject_cast<BlockEdit *>(source);
  connect(block_edit, &BlockEdit::CommitRequested, this, [this, source]() {
    for (auto &candidate : blocks_) {
      if (candidate.editor == source) {
        CommitBlock(candidate);
        break;
      }
    }
  });

  // Numbered / label 的变更是独立属性；它们会立即提交。块通过其编辑器查找，
  // 因为文档其余部分仍在重建期间 `blocks_` 可能重新分配。
  connect(numbered, &QCheckBox::toggled, this,
          [this, source, label_edit](bool on) {
            for (auto &candidate : blocks_) {
              if (candidate.editor != source)
                continue;
              candidate.equation_numbered = on;
              emit EquationEdited(candidate.node_id, source->toPlainText(), on,
                                  label_edit->text());
              break;
            }
          });
  connect(label_edit, &QLineEdit::editingFinished, this,
          [this, source, numbered]() {
            const QLineEdit *edit = qobject_cast<QLineEdit *>(sender());
            const QString label = edit ? edit->text() : QString();
            for (auto &candidate : blocks_) {
              if (candidate.editor != source)
                continue;
              candidate.equation_label = label;
              emit EquationEdited(candidate.node_id, source->toPlainText(),
                                  numbered->isChecked(), label);
              break;
            }
          });

  return card;
}

// ---------------- P0-05：共享块卡片工厂 ----------------
//
// 在这次拆分之前，Section 循环认识全部四种块类型，而 Subsection 和
// Subsubsection 循环只处理 Paragraph 和 Equation，因此在 Section 之下嵌套的
// Figure 或 Table 无处渲染：文档持有它，大纲列出它，但编辑器什么都不显示。
// 现在一个工厂加一条追加路径即可服务所有标题层级。

QWidget *BlockEditor::MakeFigureCard(const pf::Figure &figure,
                                     const QString &outline_key) {
  const QString node_id = ToQ(figure.id.value());
  QWidget *card = MakeCard(node_id, "Figure", "caption", true);
  auto *card_layout = qobject_cast<QVBoxLayout *>(card->layout());
  auto *image = new FigureImageLabel(card);
  image->setStyleSheet(
      QString("background: %1; border: 1px solid %2; border-radius: 6px;"
              "color: %3;")
          .arg(theme::kSidePanel, theme::kDivider, theme::kSecondaryText));
  const QString path =
      asset_path_resolver_ ? asset_path_resolver_(figure.asset_id) : QString();
  // 缩放到编辑器列宽，而非固定框：标签保持完整图像可见，
  // 并在窗格尺寸变化时重新适配。
  image->SetSourcePixmap(path.isEmpty() ? QPixmap() : QPixmap(path));
  card_layout->addWidget(image);
  auto *caption = NewEditor(card,
                            ToQ(pf::ReflowHardWrappedText(
                                pf::InlineToPlainText(figure.caption))),
                            1, theme::BlockVisualRole::Caption);
  if (auto *caption_edit = qobject_cast<BlockEdit *>(caption)) {
    caption_edit->setReflowOnPaste(true);
  }
  caption->setPlaceholderText("Figure caption");
  caption->setProperty("row_node", node_id);
  caption->setProperty("row_focus_key", node_id);
  caption->setProperty("row_outline_key", outline_key);
  caption->setProperty("commands_enabled", true);
  // 单栏 vs 双栏图片。无论模板是什么都记录在文档上；
  // 只有双栏模板才会导出差异（见 LatexRenderer：DoubleColumn 会输出带星号的
  // 浮动体）。
  auto *span_box = new QCheckBox(
      QStringLiteral("Span both columns (double-column figure)"), card);
  span_box->setChecked(figure.span == pf::FigureSpan::DoubleColumn);
  span_box->setToolTip(QStringLiteral(
      "Double-column figure. Has no visible effect until the paper template "
      "uses two columns."));
  span_box->setProperty("row_node", node_id);
  span_box->setProperty("row_focus_key", node_id);
  span_box->setProperty("row_outline_key", outline_key);
  card_layout->addWidget(span_box);
  connect(span_box, &QCheckBox::toggled, this,
          [this, node = node_id](bool double_column) {
            emit FigureSpanChanged(node, double_column);
          });
  Block gui_block;
  gui_block.node_id = node_id;
  gui_block.kind = QStringLiteral("Figure");
  gui_block.card = card;
  gui_block.editor = caption;
  gui_block.commit_role = QStringLiteral("caption");
  gui_block.committed_text = ToQ(pf::InlineToPlainText(figure.caption));
  gui_block.outline_key = outline_key;
  auto *block_edit = qobject_cast<BlockEdit *>(caption);
  connect(block_edit, &BlockEdit::CommitRequested, this,
          [this, caption]() {
            for (auto &candidate : blocks_) {
              if (candidate.editor == caption) {
                CommitBlock(candidate);
                break;
              }
            }
          });
  blocks_.push_back(std::move(gui_block));
  return card;
}

QWidget *BlockEditor::MakeTableCard(const pf::Table &table,
                                    const QString &outline_key) {
  const QString node_id = ToQ(table.id.value());
  QWidget *card = MakeCard(node_id, "Table", "caption", true);
  auto *grid = new QTableWidget(static_cast<int>(table.RowCount()),
                                static_cast<int>(table.ColumnCount()), card);
  grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
  grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  grid->verticalHeader()->setVisible(false);
  grid->setMaximumHeight(qMin(260, 34 + 32 * grid->rowCount()));
  for (int row = 0; row < grid->rowCount(); ++row) {
    for (int column = 0; column < grid->columnCount(); ++column) {
      grid->setItem(row, column,
                    new QTableWidgetItem(ToQ(pf::InlineToPlainText(
                        table.cells[row][column].content))));
    }
  }
  qobject_cast<QVBoxLayout *>(card->layout())->addWidget(grid);
  auto *caption = NewEditor(card,
                            ToQ(pf::ReflowHardWrappedText(
                                pf::InlineToPlainText(table.caption))),
                            1, theme::BlockVisualRole::Caption);
  if (auto *caption_edit = qobject_cast<BlockEdit *>(caption)) {
    caption_edit->setReflowOnPaste(true);
  }
  caption->setPlaceholderText("Table caption");
  caption->setProperty("row_node", node_id);
  caption->setProperty("row_focus_key", node_id);
  caption->setProperty("row_outline_key", outline_key);
  caption->setProperty("commands_enabled", true);
  Block gui_block;
  gui_block.node_id = node_id;
  gui_block.kind = QStringLiteral("Table");
  gui_block.card = card;
  gui_block.editor = caption;
  gui_block.commit_role = QStringLiteral("caption");
  gui_block.committed_text = ToQ(pf::InlineToPlainText(table.caption));
  gui_block.outline_key = outline_key;
  auto *block_edit = qobject_cast<BlockEdit *>(caption);
  connect(block_edit, &BlockEdit::CommitRequested, this,
          [this, caption]() {
            for (auto &candidate : blocks_) {
              if (candidate.editor == caption) {
                CommitBlock(candidate);
                break;
              }
            }
          });
  blocks_.push_back(std::move(gui_block));
  return card;
}

QWidget *BlockEditor::CreateBlockCard(const pf::Block &block,
                                      const QString &outline_key) {
  if (const auto *para = std::get_if<pf::Paragraph>(&block)) {
    return MakeTextCard(ToQ(para->id.value()), para->content, outline_key);
  }
  if (const auto *eq = std::get_if<pf::EquationBlock>(&block)) {
    return MakeEquationCard(ToQ(eq->id.value()), *eq, outline_key);
  }
  if (const auto *figure = std::get_if<pf::Figure>(&block)) {
    return MakeFigureCard(*figure, outline_key);
  }
  if (const auto *table = std::get_if<pf::Table>(&block)) {
    return MakeTableCard(*table, outline_key);
  }
  return nullptr;
}

void BlockEditor::AppendBlocks(const std::vector<pf::Block> &blocks,
                               const QString &outline_key) {
  auto *host_layout = qobject_cast<QVBoxLayout *>(host_->layout());
  if (host_layout == nullptr)
    return;
  for (const auto &block : blocks) {
    QWidget *card = CreateBlockCard(block, outline_key);
    if (card == nullptr)
      continue;
    host_layout->insertWidget(host_layout->count() - 1, card);
    // 每个正文块之后都跟一个间隙（包括最后一个），因此指针无需移动到工具栏
    // 就能添加下一个块。
    const QString anchor = std::visit(
        [](const auto &typed) { return QString::fromStdString(typed.id.value()); },
        block);
    host_layout->insertWidget(host_layout->count() - 1, MakeGap(anchor));
  }
}

QWidget *BlockEditor::MakeCard(const QString &node_id, const QString &kind,
                               const QString &commit_role, bool header_inline) {
  auto *card = new QFrame(host_);
  card->setObjectName("blockCard");
  card->setProperty("row_node", node_id);
  card->setProperty("row_kind", kind);
  card->setProperty("commit_role", commit_role);

  auto *card_layout = new QVBoxLayout(card);
  // 纤细外观（UI 方案 §2）：卡片内边距来自主题，并按角色保留留白，
  // 使标题和前置信息保持各自的辨识度。
  card_layout->setContentsMargins(CardMarginsFor(RoleForKind(kind)));
  card_layout->setSpacing(2);

  // 悬停表头（始终预留空间以避免布局跳动，设计 #61）。
  auto *header = new QWidget(card);
  header->setFixedHeight(theme::spacing::kBlockHeaderHeight);
  auto *header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(0, 0, 0, 0);
  header_layout->setSpacing(6);
  auto *handle = new DragHandle(node_id, header);
  auto *type_label = new QLabel(DisplayLabelFor(kind), header);
  type_label->setStyleSheet(
      QString("color: %1; font-size: 8pt; font-weight: 600;"
              " letter-spacing: 0.4px;")
          .arg(theme::kSecondaryText));
  // 阅读正文时 Text 行的标签纯属噪音（UI 方案 §2）：它仅在悬停/聚焦时出现。
  // Abstract/Keywords 则永久保留其小型大写标识标签（UI 方案 §8）。
  const bool hover_only_label = kind == QLatin1String("Text");
  type_label->setProperty("hover_only", hover_only_label);
  header_layout->addWidget(handle);
  header_layout->addWidget(type_label);
  header_layout->addStretch(1);

  // More（「⋯」）按钮，带上下文菜单。
  auto *more = new QToolButton(header);
  more->setText("⋯");
  more->setAutoRaise(true);
  more->setFixedSize(22, theme::spacing::kBlockHeaderHeight);
  more->setStyleSheet(
      QString("QToolButton { color: %1; border: none; }"
              "QToolButton:hover { background: %2; border-radius: 4px; }")
          .arg(theme::kSecondaryText, theme::kAccentSoft));
  const bool can_restructure = !node_id.isEmpty();
  const bool can_reflow = IsProseRole(commit_role);
  more->setVisible(false);
  connect(more, &QToolButton::clicked, this,
          [this, more, card, node_id, can_restructure, can_reflow]() {
            if (!can_restructure && !can_reflow)
              return;
            QMenu menu(this);
            QMenu *insert_menu = nullptr;
            QAction *up = nullptr;
            QAction *down = nullptr;
            QAction *del = nullptr;
            QAction *reflow = nullptr;
            if (can_restructure) {
              insert_menu = menu.addMenu("Insert Block Below");
              for (const auto &entry : InsertEntries(false)) {
                QAction *action =
                    insert_menu->addAction(QString::fromUtf8(entry.label));
                action->setData(QString::fromUtf8(entry.kind));
              }
            }
            if (can_reflow) {
              reflow = menu.addAction("Reflow Text");
              reflow->setToolTip(
                  "Undo the hard line breaks of a pasted paragraph so it "
                  "wraps to the block width");
            }
            if (can_restructure) {
              menu.addSeparator();
              up = menu.addAction("Move Up");
              down = menu.addAction("Move Down");
              menu.addSeparator();
              del = menu.addAction("Delete");
            }
            QAction *chosen_action =
                menu.exec(more->mapToGlobal(QPoint(0, more->height())));
            if (chosen_action == nullptr)
              return;
            if (insert_menu && chosen_action->parent() == insert_menu) {
              emit InsertBlockRequested(chosen_action->data().toString(),
                                        node_id);
            } else if (chosen_action == up) {
              emit MoveBlockRequested(node_id, -1);
            } else if (chosen_action == down) {
              emit MoveBlockRequested(node_id, +1);
            } else if (chosen_action == del) {
              emit DeleteBlockRequested(node_id);
            } else if (chosen_action == reflow) {
              ReflowRow(card, node_id);
            }
          });
  header_layout->addWidget(more);

  card_layout->addWidget(header);
  card->setProperty("header",
                    QVariant::fromValue(static_cast<QWidget *>(header)));

  header->setVisible(true);
  handle->setVisible(false);
  type_label->setVisible(header_inline && !hover_only_label);
  more->setVisible(false);

  // 悬停 / 聚焦 / 缺失 / 闪烁外观（设计 #4，UI 方案 §9）。
  card->setProperty("card_hover", false);
  card->setProperty("card_focus", false);
  card->setProperty("card_missing", false);
  card->setProperty("card_flash", false);
  card->installEventFilter(this);
  UpdateCardState(card);
  return card;
}

// 组合块卡片视觉状态的唯一场所（UI 方案 §9）：
//   idle     透明的 2px 左侧线，无背景
//   hover    #FAFBFC 淡色
//   focused  2px 强调色线，近白色背景
//   missing  2px 错误色线，淡淡的 ErrorSoft 淡色
//   flash    2px 强调色线 + AccentSoft 淡色（问题导航，短暂）
// 表头外观（抓手 / 仅悬停标签 / more 按钮）遵循同样的 hover || focus 条件。
// 绝不绘制完整的矩形边框：视觉焦点始终停留在文本上，而非控件上。
void BlockEditor::UpdateCardState(QWidget *card) {
  if (!card)
    return;
  const bool missing = card->property("card_missing").toBool();
  const bool hover = card->property("card_hover").toBool();
  const bool focus = card->property("card_focus").toBool();
  const bool flash = card->property("card_flash").toBool();

  const char *line = "transparent";
  const char *background = "transparent";
  if (flash) {
    line = theme::kAccent;
    background = theme::kAccentSoft;
  } else if (missing) {
    line = theme::kError;
    background = theme::kErrorSoft;
  } else if (focus) {
    line = theme::kAccent;
    background = theme::kEditorBackground;
  } else if (hover) {
    background = theme::kBlockHover;
  }
  card->setStyleSheet(QString("QFrame#blockCard { border-left: %1px solid %2;"
                              " background: %3; border-radius: %4px; }")
                          .arg(theme::spacing::kFocusLine)
                          .arg(QLatin1String(line), QLatin1String(background))
                          .arg(theme::spacing::kCardRadius));

  const bool chrome = hover || focus;
  // 格式条是外观的一部分：聚焦的行会显示其编辑工具，空闲的行只显示文本
  // （UI 方案 §2）。
  if (auto *bar = card->property("format_bar").value<QWidget *>()) {
    bar->setVisible(chrome);
  }
  auto *header = card->property("header").value<QWidget *>();
  if (!header)
    return;
  const bool structural = !card->property("row_node").toString().isEmpty();
  for (QObject *child : header->children()) {
    if (auto *grip = qobject_cast<DragHandle *>(child)) {
      grip->setVisible(chrome && structural);
    } else if (auto *button = qobject_cast<QToolButton *>(child)) {
      button->setVisible(chrome && structural);
    } else if (auto *label = qobject_cast<QLabel *>(child)) {
      if (label->property("hover_only").toBool()) {
        label->setVisible(chrome);
      }
    }
  }
}

void BlockEditor::AddEditorToCard(QWidget *card, QPlainTextEdit *edit) {
  auto *card_layout = qobject_cast<QVBoxLayout *>(card->layout());
  card_layout->addWidget(edit);
}

QPlainTextEdit *BlockEditor::NewEditor(QWidget *card, const QString &text,
                                       int min_lines,
                                       theme::BlockVisualRole role,
                                       bool single_line) {
  auto *edit = new BlockEdit(single_line, card);
  // 先设置字体环境，再设置文本（UI 方案 §6）：度量、行高和占位符最小高度
  // 都源自角色的字体。
  edit->SetTypography(theme::EditorFont(role), theme::LineHeightFor(role));
  edit->SetInitialText(text);
  edit->setStyleSheet(
      QString(
          "QPlainTextEdit { background: transparent; border: none; color: %1;"
          " padding: 0; selection-background-color: %2; }")
          .arg(theme::IsSecondaryRole(role) ? theme::kSecondaryText
                                            : theme::kPrimaryText,
               theme::kAccentSoft));
  // 占位符的最小高度（abstract 要求三行），按角色的行高缩放，
  // 使空状态与已加载状态一致。
  const int line_height = theme::LineHeightFor(role);
  const qreal line_scale = line_height > 0 ? line_height / 100.0 : 1.0;
  const int min_h =
      static_cast<int>(edit->fontMetrics().height() * min_lines * line_scale) +
      12;
  if (auto *block_edit = qobject_cast<BlockEdit *>(edit)) {
    block_edit->SetMinimumContentHeight(min_h);
  }
  edit->installEventFilter(this);
  AddEditorToCard(card, edit);
  return edit;
}

void BlockEditor::ReflowRow(QWidget *card, const QString &node_id) {
  if (!card)
    return;
  QPlainTextEdit *editor = card->findChild<QPlainTextEdit *>();
  if (!editor)
    return;
  Block *target = nullptr;
  for (auto &block : blocks_) {
    if (block.editor == editor) {
      target = &block;
      break;
    }
  }
  if (!target)
    return;
  const QString before = editor->toPlainText();
  const QString after = ToQ(pf::ReflowHardWrappedText(ToStd(before)));
  if (after == before)
    return;
  const int caret = editor->textCursor().position();
  auto *block_edit = qobject_cast<BlockEdit *>(editor);
  if (block_edit)
    block_edit->BeginProgrammaticEdit();
  editor->setPlainText(after);
  if (block_edit)
    block_edit->ReapplyLineHeight();
  QTextCursor cursor = editor->textCursor();
  cursor.setPosition(qMin(caret, static_cast<int>(after.size())));
  editor->setTextCursor(cursor);
  if (block_edit)
    block_edit->EndProgrammaticEdit();
  // 立即提交：该动作的意义在于持久化重新排布的结果，
  // 而不是让它等待焦点变化。
  CommitBlock(*target);
  RevealNode(node_id);
}

void BlockEditor::CommitBlock(Block &block) {
  if (rebuilding_)
    return;
  // Text 行通过其 InlineEditor 提交（富文本内容）；其他所有行都是普通的
  // BlockEdit。
  if (block.inline_editor) {
    const InlineContent content = block.inline_editor->Content();
    if (content == block.committed_content) {
      block.inline_editor->MarkClean();
      return;
    }
    block.committed_content = content;
    block.committed_text = ToQ(pf::InlineToPlainText(content));
    block.inline_editor->MarkClean();
    emit ParagraphContentEdited(block.node_id, content);
    emit RowCommitted();
    return;
  }
  if (!block.editor)
    return;
  auto *edit = qobject_cast<BlockEdit *>(block.editor);
  QString text = block.editor->toPlainText();
  // 正文可能仍带有粘贴时的硬换行；柔化它们对输出无影响（LaTeX 将单个换行视
  // 为空格），并让段落能按块宽度重新排布。
  if (IsProseRole(block.commit_role)) {
    text = ToQ(pf::ReflowHardWrappedText(ToStd(text)));
  }
  // 自文档上次看到该行以来没有任何变化：没有编辑，因此没有 documentChanged
  // -> 没有重建。这正是阻止「重新设置样式」或仅仅聚焦某行陷入完整编辑器重建
  // 循环的原因。
  if (text == block.committed_text) {
    if (edit)
      edit->MarkClean();
    emit RowCommitted();
    return;
  }
  block.committed_text = text;
  if (edit)
    edit->MarkClean();
  const QString &role = block.commit_role;
  if (role == "title")
    emit TitleEdited(text);
  else if (role == "authors")
    emit AuthorsEdited(text);
  else if (role == "affiliations")
    emit AffiliationsEdited(text);
  else if (role == "abstract")
    emit AbstractEdited(text);
  else if (role == "keywords")
    emit KeywordsEdited(text);
  else if (role == "equation") {
    emit EquationEdited(block.node_id, text, block.equation_numbered,
                        block.equation_label);
  } else if (role == "section")
    emit SectionRenamed(block.node_id, text);
  else if (role == "subsection")
    emit SubsectionRenamed(block.node_id, text);
  else if (role == "subsubsection")
    emit SubsubsectionRenamed(block.node_id, text);
  else if (role == "caption")
    emit CaptionEdited(block.node_id, text);
  emit RowCommitted();
}

bool BlockEditor::eventFilter(QObject *watched, QEvent *event) {
  const QEvent::Type type = event->type();
  // 窗格尺寸变化时（拖动分隔条、专注模式、窗口缩放）保持阅读列居中。
  if (scroll_ && watched == scroll_->viewport() && type == QEvent::Resize) {
    CenterContentColumn();
    return false;
  }
  // 块卡片的悬停外观（设计 #4）。样式本身在 UpdateCardState 中组合；
  // 过滤器只记录状态。
  if (type == QEvent::Enter || type == QEvent::Leave) {
    auto *card = qobject_cast<QFrame *>(watched);
    if (card && card->objectName() == "blockCard") {
      card->setProperty("card_hover", type == QEvent::Enter);
      UpdateCardState(card);
    }
    return false;
  }
  // 行聚焦：卡片上的强调色线，以及反向大纲同步（UI 方案 §9、§10）。
  // 每个行编辑器（BlockEdit 和 InlineEditor）都会被过滤。
  if (type == QEvent::FocusIn || type == QEvent::FocusOut) {
    if (auto *card = CardOf(watched)) {
      card->setProperty("card_focus", type == QEvent::FocusIn);
      UpdateCardState(card);
      if (type == QEvent::FocusIn) {
        const auto *widget = qobject_cast<const QWidget *>(watched);
        emit FocusOutlineChanged(
            widget ? widget->property("row_outline_key").toString()
                   : QString());
      }
    }
    return false;
  }

  // 来自各编辑器的按键接线。
  auto *edit = qobject_cast<BlockEdit *>(watched);
  if (edit && type == QEvent::KeyPress) {
    auto *key_event = static_cast<QKeyEvent *>(event);
    if (key_event->key() == Qt::Key_F2) {
      OpenSlashMenu(edit);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void BlockEditor::BuildAuthorBindingPanel(QWidget *card,
                                          const FrontMatter &front) {
  if (card == nullptr)
    return;
  auto *card_layout = qobject_cast<QVBoxLayout *>(card->layout());
  if (card_layout == nullptr)
    return;

  auto *panel = new QWidget(card);
  panel->setObjectName("authorBindingPanel");
  auto *panel_layout = new QVBoxLayout(panel);
  panel_layout->setContentsMargins(2, 0, 2, 2);
  panel_layout->setSpacing(1);

  auto *caption = new QLabel("Institution links", panel);
  caption->setStyleSheet(QString("color: %1; font-size: 8pt;"
                                 " font-weight: 700; letter-spacing: 0.5px;")
                             .arg(theme::kDisabledText));
  panel_layout->addWidget(caption);

  if (front.authors.empty()) {
    auto *hint = new QLabel("Add an author above to link institutions.", panel);
    hint->setStyleSheet(
        QString("color: %1; font-size: 9pt;").arg(theme::kDisabledText));
    panel_layout->addWidget(hint);
  } else if (front.affiliations.empty()) {
    auto *hint = new QLabel(
        "Add an institution above, then link each author to it here.", panel);
    hint->setStyleSheet(
        QString("color: %1; font-size: 9pt;").arg(theme::kDisabledText));
    panel_layout->addWidget(hint);
  } else {
    for (size_t i = 0; i < front.authors.size(); ++i) {
      const auto &author = front.authors[i];
      auto *row = new QWidget(panel);
      auto *row_layout = new QHBoxLayout(row);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->setSpacing(6);

      auto *name = new QLabel(ToQ(author.name), row);
      name->setMinimumWidth(120);
      name->setStyleSheet(
          QString("color: %1; font-size: 9pt;").arg(theme::kPrimaryText));
      row_layout->addWidget(name);

      // 按钮显示当前的编号，因此无需打开任何东西就能看到绑定关系。
      QStringList current;
      for (size_t a = 0; a < front.affiliations.size(); ++a) {
        for (const auto &link : author.affiliations) {
          if (link == front.affiliations[a].id) {
            static const char *kSupers[] = {"\u00b9", "\u00b2", "\u00b3",
                                            "\u2074", "\u2075", "\u2076",
                                            "\u2077", "\u2078", "\u2079"};
            current << (a < 9 ? QString::fromUtf8(kSupers[a])
                              : QString::number(a + 1));
          }
        }
      }
      auto *pick = new QToolButton(row);
      pick->setObjectName("authorAffiliationPicker");
      pick->setText(current.isEmpty() ? QStringLiteral("link…")
                                      : current.join(QLatin1Char(' ')));
      pick->setToolTip("Which institutions does this author belong to?");
      pick->setCursor(Qt::PointingHandCursor);
      pick->setFocusPolicy(Qt::NoFocus);
      pick->setStyleSheet(QString("QToolButton { color: %1; background: %2;"
                                  " border: 1px solid %3; border-radius: 9px;"
                                  " padding: 1px 8px; font-size: 9pt; }"
                                  "QToolButton:hover { border-color: %4; }")
                              .arg(theme::kSecondaryText, theme::kSidePanel,
                                   theme::kDivider, theme::kAccent));
      row_layout->addWidget(pick);
      row_layout->addStretch(1);
      panel_layout->addWidget(row);

      const int author_index = static_cast<int>(i);
      connect(pick, &QToolButton::clicked, this,
              [this, pick, front, author_index]() {
                QMenu menu(this);
                for (size_t a = 0; a < front.affiliations.size(); ++a) {
                  const auto &affiliation = front.affiliations[a];
                  const bool linked = std::any_of(
                      front.authors[author_index].affiliations.begin(),
                      front.authors[author_index].affiliations.end(),
                      [&](const AffiliationId &id) {
                        return id == affiliation.id;
                      });
                  QAction *action =
                      menu.addAction(QStringLiteral("%1  %2").arg(a + 1).arg(
                          ToQ(affiliation.name)));
                  action->setCheckable(true);
                  action->setChecked(linked);
                  const QString id = ToQ(affiliation.id.value());
                  connect(action, &QAction::triggered, this,
                          [this, author_index, id](bool checked) {
                            emit AuthorAffiliationToggled(author_index, id,
                                                          checked);
                          });
                }
                menu.exec(pick->mapToGlobal(QPoint(0, pick->height())));
              });
    }
  }
  card_layout->addWidget(panel);
}

QWidget *BlockEditor::MakeGap(const QString &anchor) {
  auto *gap = new BlockGap(
      [this, anchor](QWidget *source) { ShowInsertMenu(anchor, source); },
      [this, anchor](const QString &node) {
        if (anchor.isEmpty()) {
          // 正文为空，因此没有可重排进去的内容。
          return;
        }
        if (node == anchor)
          return;
        emit MoveBlockToRequested(node, anchor);
      },
      host_);
  gap->setProperty("gap_anchor", anchor);
  gap->setProperty("body_gap", anchor.isEmpty());
  return gap;
}

void BlockEditor::ShowInsertMenu(const QString &anchor, QWidget *source) {
  const bool body_empty = anchor.isEmpty();
  QMenu menu(this);
  if (body_empty) {
    QAction *hint = menu.addAction("The body is empty — start with a section");
    hint->setEnabled(false);
    menu.addSeparator();
  }
  const NodeKind container = ContainerKindFor(*container_document_, anchor);
  const int max_depth = max_heading_depth_ > 0 ? max_heading_depth_ : 3;
  const char *current_group = nullptr;
  for (const auto &entry : InsertOptionsFor(body_empty, container, max_depth)) {
    if (current_group == nullptr ||
        std::strcmp(current_group, entry.group) != 0) {
      if (current_group != nullptr)
        menu.addSeparator();
      QAction *group = menu.addAction(QString::fromUtf8(entry.group));
      group->setEnabled(false);
      current_group = entry.group;
    }
    QAction *action = menu.addAction(QString::fromUtf8(entry.label));
    action->setData(QString::fromUtf8(entry.kind));
  }
  QAction *chosen = menu.exec(
      source->mapToGlobal(QPoint(source->width() / 2, source->height())));
  if (chosen == nullptr || !chosen->data().isValid())
    return;
  emit InsertBlockRequested(chosen->data().toString(), anchor);
}

void BlockEditor::OpenSlashMenu(QPlainTextEdit *origin) {
  // 规则与间隙菜单相同：按该行所属的容器以及模板的标题深度进行过滤。
  QString row_node;
  for (const auto &block : blocks_) {
    if (block.editor == origin) {
      row_node = block.node_id;
      break;
    }
  }
  const NodeKind container = ContainerKindFor(*container_document_, row_node);
  const int max_depth = max_heading_depth_ > 0 ? max_heading_depth_ : 3;

  std::vector<PopupList::Item> items;
  const char *current_group = nullptr;
  for (const auto &entry :
       InsertOptionsFor(row_node.isEmpty(), container, max_depth)) {
    PopupList::Item item;
    item.label = QString::fromUtf8(entry.label);
    item.detail = "Insert";
    item.group = QString::fromUtf8(entry.group);
    item.payload = QString::fromUtf8(entry.kind);
    item.search = item.label.toLower();
    if (current_group == nullptr ||
        std::strcmp(current_group, entry.group) != 0) {
      current_group = entry.group;
    }
    items.push_back(std::move(item));
  }

  QString after;
  for (const auto &block : blocks_) {
    if (block.editor == origin) {
      after = block.node_id;
      break;
    }
  }
  auto *popup = new PopupList(this);
  connect(popup, &PopupList::chosen, this, [this, after](QString payload) {
    emit InsertBlockRequested(payload, after);
  });
  QPoint anchor = origin->mapToGlobal(QPoint(60, origin->height() + 4));
  popup->popup(anchor, items);
}

void BlockEditor::RebuildFromDocument(const Document &doc) {
  // 在下一次重建之前，插入菜单都以本文档解析锚点；MainWindow 会让 session
  // 存活至编辑器的整个生命周期，而重建始终传入当前文档。
  container_document_ = &doc;
  // 保存焦点，以及用户仍在编辑的行的当前文本。该文本优先于文档：从其他任何
  // 地方触发的重建都绝不能丢弃当前正在输入的内容。
  focus_node_.clear();
  focus_pos_ = 0;
  QString pending_key;
  QString pending_text;
  bool has_pending = false;
  // 富文本行在重建期间同样保留其光标：引用选择器通过一次行重建来提交，
  // 并期望输入精确地从插入 pill 的位置继续（引用方案 §4）。
  if (auto *rich = qobject_cast<InlineEditor *>(focusWidget())) {
    focus_node_ = rich->property("row_focus_key").toString();
    focus_pos_ = rich->textCursor().position();
  }
  if (auto *edit = qobject_cast<BlockEdit *>(focusWidget())) {
    focus_node_ = edit->property("row_focus_key").toString();
    focus_pos_ = edit->textCursor().position();
    if (edit->IsDirty()) {
      pending_key = focus_node_;
      pending_text = edit->toPlainText();
      has_pending = true;
    }
  }

  rebuilding_ = true;
  auto *host_layout = qobject_cast<QVBoxLayout *>(host_->layout());
  // 清空上一轮的所有控件，只保留末尾的 stretch。行和间隙都在每次重建时创建，
  // 若有残留控件存活，它会持续从覆盖其上的行那里抢走悬停和点击。
  for (int i = host_layout->count() - 1; i >= 0; --i) {
    QLayoutItem *item = host_layout->itemAt(i);
    if (item == nullptr || item->widget() == nullptr)
      continue;
    host_layout->takeAt(i);
    QWidget *widget = item->widget();
    widget->hide();
    widget->deleteLater();
    delete item;
  }
  blocks_.clear();
  // 每个正文块之后都跟一个间隙（包括最后一个），因此指针无需移动到工具栏
  // 就能添加下一个块。
  auto append_gap = [&](const QString &anchor) {
    host_layout->insertWidget(host_layout->count() - 1, MakeGap(anchor));
  };
  auto add = [&](const QString &node_id, const QString &kind,
                 const QString &commit_role, const QString &text,
                 const QString &outline_key = QString(),
                 bool header_inline = true, int min_lines = 1,
                 bool single_line = false) -> QWidget * {
    QWidget *card = MakeCard(node_id, kind, commit_role, header_inline);
    const QString focus_key =
        node_id.isEmpty() ? QStringLiteral("front:") + commit_role : node_id;
    const bool restore_pending = has_pending && focus_key == pending_key;
    QString row_text = restore_pending ? pending_text : text;
    if (IsProseRole(commit_role) && !restore_pending) {
      // 用户正在该行输入时绝不重新排布。
      row_text = ToQ(pf::ReflowHardWrappedText(ToStd(row_text)));
    }
    // 角色决定字体 + 行高（UI 方案 §3、§5）：标题、作者、摘要、节/小节/小小节
    // 标题以及正文，都按统一 GUI 排版表的字号和字重呈现。
    QPlainTextEdit *edit =
        NewEditor(card, row_text, min_lines, RoleForKind(kind), single_line);
    if (auto *block_edit = qobject_cast<BlockEdit *>(edit)) {
      if (restore_pending) {
        // 模型文本作为基线，因此用户的文本仍会在失焦时提交。
        block_edit->BeginProgrammaticEdit();
        block_edit->EndProgrammaticEdit();
        block_edit->MarkClean();
      }
    }
    if (auto *block_edit = qobject_cast<BlockEdit *>(edit)) {
      block_edit->setReflowOnPaste(IsProseRole(commit_role));
      block_edit->MarkClean();
    }
    edit->setProperty("row_node", node_id);
    edit->setProperty("row_focus_key", focus_key);
    edit->setProperty("row_outline_key", outline_key);
    edit->setProperty("commands_enabled", !node_id.isEmpty());
    auto center_text = [edit]() {
      auto *block_edit = qobject_cast<BlockEdit *>(edit);
      if (block_edit)
        block_edit->BeginProgrammaticEdit();
      QTextCursor cursor = edit->textCursor();
      cursor.select(QTextCursor::Document);
      QTextBlockFormat format;
      format.setAlignment(Qt::AlignHCenter);
      cursor.mergeBlockFormat(format);
      cursor.clearSelection();
      edit->setTextCursor(cursor);
      if (block_edit)
        block_edit->EndProgrammaticEdit();
    };
    // 字体 + 字重已由角色设置好；这里只按前置信息类型差异设置阅读辅助
    // （占位符、居中）。
    if (kind == "Title") {
      edit->setPlaceholderText("Untitled paper");
      center_text();
    } else if (kind == "Authors") {
      edit->setPlaceholderText("Author names — separate with commas");
      center_text();
    } else if (kind == "Institution") {
      edit->setPlaceholderText("Affiliations — separate with semicolons");
      center_text();
    } else if (kind == "Abstract") {
      edit->setPlaceholderText("Write the abstract…");
    } else if (kind == "Keywords") {
      edit->setPlaceholderText("Keywords — separate with commas");
    } else if (kind == "Section Title") {
      edit->setPlaceholderText("Section title");
    } else if (kind == "Subsection Title") {
      edit->setPlaceholderText("Subsection title");
    } else if (kind == "Subsubsection Title") {
      edit->setPlaceholderText("Subsubsection title");
    } else if (kind == "Equation") {
      edit->setPlaceholderText("LaTeX equation");
    }
    qobject_cast<BlockEdit *>(edit)->Resize();
    Block block;
    block.node_id = node_id;
    block.kind = kind;
    block.card = card;
    block.editor = edit;
    block.commit_role = commit_role;
    block.committed_text = text; // 文档为该行保存的内容
    block.outline_key = outline_key;
    if (restore_pending) {
      // 保持其为脏：用户未提交的文本仍处于待处理状态。
      if (auto *restored = qobject_cast<BlockEdit *>(edit)) {
        restored->MarkDirty();
      }
    }
    // 在 Enter 时提交（commit 信号）——BlockEdit 会在 Enter 和 focusOut 时
    // 都发出 CommitRequested；我们需要的是失焦提交，Enter 只是继续移动。
    // 接线如下：
    BlockEdit *block_edit = qobject_cast<BlockEdit *>(edit);
    connect(block_edit, &BlockEdit::CommitRequested, this, [this, edit]() {
      for (auto &b : blocks_) {
        if (b.editor == edit) {
          CommitBlock(b);
          break;
        }
      }
    });
    connect(block_edit, &BlockEdit::TriggerSlash, this,
            [this, edit]() { OpenSlashMenu(edit); });
    connect(block_edit, &BlockEdit::NewBlockAfter, this, [this, node_id]() {
      emit InsertBlockRequested("paragraph", node_id);
    });
    host_layout->insertWidget(host_layout->count() - 1, card);
    if (!node_id.isEmpty())
      append_gap(node_id);
    blocks_.push_back(std::move(block));
    return card;
  };

  // 前置信息。其行不是大纲节点，因此传入空的大纲键（Abstract 是例外：
  // 它可通过 "front:abstract" 从大纲访问）。
  const auto &fm = doc.front_matter();
  add("", "Title", "title", ToQ(pf::InlineToPlainText(fm.title)), QString(),
      true, 1, true);
  QString authors;
  for (size_t i = 0; i < fm.authors.size(); ++i) {
    if (i)
      authors += " · ";
    authors += ToQ(fm.authors[i].name);
    // 机构上标
    for (const auto &aff_id : fm.authors[i].affiliations) {
      for (size_t a = 0; a < fm.affiliations.size(); ++a) {
        if (fm.affiliations[a].id == aff_id) {
          const char *supers[] = {"¹", "²", "³", "⁴", "⁵"};
          authors += supers[a < 5 ? a : 4];
          break;
        }
      }
    }
  }
  {
    QWidget *authors_card =
        add("", "Authors", "authors", authors, QString(), true, 1, true);
    BuildAuthorBindingPanel(authors_card, fm);
  }
  QString affiliations;
  for (size_t i = 0; i < fm.affiliations.size(); ++i) {
    if (i)
      affiliations += "; ";
    const char *supers[] = {"¹ ", "² ", "³ ", "⁴ ", "⁵ "};
    affiliations += supers[i < 5 ? i : 4] + ToQ(fm.affiliations[i].name);
  }
  add("", "Institution", "affiliations", affiliations, QString(), true, 1,
      true);
  add("", "Abstract", "abstract",
      fm.abstract_text ? ToQ(pf::InlineToPlainText(*fm.abstract_text))
                       : QString(),
      QStringLiteral("front:abstract"), true, 3);
  QString keywords;
  for (size_t i = 0; i < fm.keywords.size(); ++i) {
    if (i)
      keywords += ", ";
    keywords += ToQ(fm.keywords[i]);
  }
  add("", "Keywords", "keywords", keywords, QString(), true, 1, true);

  // 正文。
  if (doc.body().sections.empty()) {
    // 尚无内容可供悬停：提示开始撰写正文。
    append_gap(QString());
  }
  for (const auto &section : doc.body().sections) {
    const QString section_key = ToQ(section.id.value());
    add(section_key, "Section Title", "section",
        ToQ(pf::InlineToPlainText(section.title)), section_key, true, 1, true);
    // P0-05：每个标题层级的所有块类型都走这一条追加路径。
    AppendBlocks(section.blocks, section_key);
    for (const auto &sub : section.subsections) {
      const QString sub_key = ToQ(sub.id.value());
      add(sub_key, "Subsection Title", "subsection",
          ToQ(pf::InlineToPlainText(sub.title)), sub_key, true, 1, true);
      AppendBlocks(sub.blocks, sub_key);
      for (const auto &subsub : sub.subsubsections) {
        const QString subsub_key = ToQ(subsub.id.value());
        add(subsub_key, "Subsubsection Title", "subsubsection",
            ToQ(pf::InlineToPlainText(subsub.title)), subsub_key, true, 1,
            true);
        AppendBlocks(subsub.blocks, subsub_key);
      }
    }
  }

  ApplyHints();

  // 恢复焦点。
  if (!focus_node_.isEmpty()) {
    for (const auto &block : blocks_) {
      // Text 行：富文本编辑器，光标钳制在其文档内。
      if (block.inline_editor &&
          block.inline_editor->property("row_focus_key").toString() ==
              focus_node_) {
        auto cursor = block.inline_editor->textCursor();
        cursor.setPosition(qMin(
            focus_pos_, block.inline_editor->document()->characterCount() - 1));
        block.inline_editor->setTextCursor(cursor);
        block.inline_editor->setFocus();
        break;
      }
      if (block.editor &&
          block.editor->property("row_focus_key").toString() == focus_node_) {
        auto cursor = block.editor->textCursor();
        cursor.setPosition(
            qMin(focus_pos_, block.editor->toPlainText().length()));
        block.editor->setTextCursor(cursor);
        block.editor->setFocus();
        break;
      }
    }
  }
  rebuilding_ = false;
}

void BlockEditor::SetRequiredHints(const RequiredHints &hints) {
  hints_ = hints;
  ApplyHints();
}

void BlockEditor::ApplyHints() {
  for (auto &block : blocks_) {
    bool missing = false;
    if (block.kind == "Title") {
      missing = hints_.title && block.editor &&
                block.editor->toPlainText().trimmed().isEmpty();
    } else if (block.kind == "Authors") {
      missing = hints_.authors && block.editor &&
                block.editor->toPlainText().trimmed().isEmpty();
    } else if (block.kind == "Institution") {
      missing = hints_.affiliations && block.editor &&
                block.editor->toPlainText().trimmed().isEmpty();
    } else if (block.kind == "Abstract") {
      missing = hints_.abstract_text && block.editor &&
                block.editor->toPlainText().trimmed().isEmpty();
    } else if (block.kind == "Keywords") {
      missing = hints_.keywords && block.editor &&
                block.editor->toPlainText().trimmed().isEmpty();
    }
    if (!block.card)
      continue;
    // 样式在 UpdateCardState 中根据此标志组合；没有逐处设置的样式表
    // （UI 方案 §9）。
    block.card->setProperty("card_missing", missing);
    UpdateCardState(block.card);
  }
}

void BlockEditor::SetReferenceItems(std::vector<PopupList::Item> items) {
  reference_items_ = std::move(items);
  // 交叉引用 pill 显示目标的标签。
  const auto labels = CrossReferenceLabels();
  for (auto &block : blocks_) {
    if (block.inline_editor) {
      block.inline_editor->SetCrossReferenceLabels(labels);
    }
  }
}

void BlockEditor::SetAssetPathResolver(
    std::function<QString(const AssetId &)> resolver) {
  asset_path_resolver_ = std::move(resolver);
}

bool BlockEditor::HasUncommittedFocus() const {
  // 模态公式编辑器会暂时拥有焦点，但它所在的行仍包含待处理的编辑，
  // 必须能在文档通知中存活。打开了引用/交叉引用选择器的行也是如此：
  // 选择器会插入到*那个*控件中并自行提交它（引用方案 §4）。
  for (const auto &block : blocks_) {
    if (block.inline_editor && (block.inline_editor->IsMathEditorOpen() ||
                                block.inline_editor->IsProtectedInsertOpen())) {
      return true;
    }
  }
  // Text 行（InlineEditor）与普通行一样带有未提交的输入；在任一方为脏时重建
  // 都会销毁正在输入的内容——对富文本行而言，还会销毁格式状态。
  if (auto *rich = qobject_cast<InlineEditor *>(focusWidget())) {
    return rich->IsDirty();
  }
  if (auto *edit = qobject_cast<BlockEdit *>(focusWidget())) {
    return edit->IsDirty();
  }
  return false;
}

void BlockEditor::RefreshHints() { ApplyHints(); }

void BlockEditor::CommitFocused() {
  if (auto *rich = qobject_cast<InlineEditor *>(focusWidget())) {
    for (auto &block : blocks_) {
      if (block.inline_editor == rich) {
        CommitBlock(block);
        return;
      }
    }
  }
  if (auto *edit = qobject_cast<BlockEdit *>(focusWidget())) {
    for (auto &block : blocks_) {
      if (block.editor == edit) {
        CommitBlock(block);
        return;
      }
    }
  }
}

std::optional<QString> BlockEditor::FocusedNodeId() const {
  if (auto *rich = qobject_cast<InlineEditor *>(focusWidget())) {
    QString node = rich->property("row_node").toString();
    if (!node.isEmpty())
      return node;
  }
  if (auto *edit = qobject_cast<BlockEdit *>(focusWidget())) {
    QString node = edit->property("row_node").toString();
    if (!node.isEmpty())
      return node;
  }
  return std::nullopt;
}

void BlockEditor::RevealNode(const QString &node_id) {
  for (const auto &block : blocks_) {
    // 前置信息行（Abstract、Title）通过其行键寻址，
    // 真实块则通过其 node id 寻址。
    const bool matches =
        block.node_id == node_id ||
        (block.editor &&
         block.editor->property("row_focus_key").toString() == node_id);
    if (matches && block.card) {
      // 问题导航会落到这里（Build Diagnostics 方案 §26，UI 方案 §9）：
      // 滚动、短暂强调色闪烁，并将焦点移入该行。闪烁是一个属性，而非手写的
      // 样式表，因此它会与聚焦/悬停状态组合，而不是覆盖它们。
      scroll_->ensureWidgetVisible(block.card, 0, 80);
      QWidget *revealed = block.card;
      revealed->setProperty("card_flash", true);
      UpdateCardState(revealed);
      if (block.inline_editor) {
        block.inline_editor->setFocus();
      } else if (block.editor) {
        block.editor->setFocus();
      } else {
        revealed->setFocus();
      }
      QPointer<QWidget> guard(revealed);
      QTimer::singleShot(1200, this, [this, guard]() {
        if (!guard)
          return;
        guard->setProperty("card_flash", false);
        UpdateCardState(guard);
      });
      return;
    }
  }
}

} // namespace pf::gui
