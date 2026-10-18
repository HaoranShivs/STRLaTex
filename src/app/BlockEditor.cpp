#include "app/BlockEditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QAction>
#include <QColor>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDrag>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QFileInfo>
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
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <initializer_list>

#include "app/MathCommandCompleter.h"
#include "app/InlineContentEditorDialog.h"
#include "app/MathPaletteWidget.h"
#include "app/MathPreviewRenderer.h"
#include "app/QtConvert.h"
#include "app/Theme.h"
#include "app/math/MathRenderService.h"
#include "document/InlineText.h"
#include "math/MathValidator.h"

namespace pf::gui {

namespace {
using theme::kAccent;

// QTextEdit::cursorRect() 使用 viewport 坐标；按当前排版行的实际高度
// 在光标下方留出两行，再转换为屏幕坐标供浮动列表使用。
QPoint PickerPositionBelowCaret(InlineEditor* editor) {
    const QRect caret = editor->cursorRect();
    int line_height = qMax(1, caret.height());
    const QTextCursor cursor = editor->textCursor();
    if (const QTextLayout* layout = cursor.block().layout()) {
        const QTextLine line = layout->lineForTextPosition(cursor.positionInBlock());
        if (line.isValid() && line.height() > 0)
            line_height = qMax(1, qRound(line.height()));
    }
    return editor->viewport()->mapToGlobal(QPoint(caret.left(), caret.bottom() + 2 * line_height));
}

// Block reordering stays separate from image import and ordinary text drops.
constexpr const char* kBlockMime = "application/x-paperforge-block";

QStringList AssetFiles(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls())
        return paths;
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (!url.isLocalFile() || !QStringList{"png", "jpg", "jpeg", "gif", "pdf", "svg", "eps"}.contains(suffix))
            return {};
        paths.push_back(path);
    }
    return paths;
}

bool HasAssetMime(const QMimeData* mime) {
    return mime && !mime->hasFormat(kBlockMime) && !mime->hasFormat("application/x-strlatex-inline") &&
           (!AssetFiles(mime).isEmpty() || mime->hasImage());
}

// Children such as QTextEdit own their viewports and drag handling. Filtering
// application delivery lets images reach the containing block from any of them.
// Dialogs and all other windows keep their own paste/drop behavior.
class AssetInputFilter final : public QObject {
  public:
    using Handler = std::function<bool(const QMimeData*, QWidget*, QPoint)>;
    AssetInputFilter(QWidget* root, Handler handler) : QObject(root), root_(root), handler_(std::move(handler)) {
        qApp->installEventFilter(this);
    }
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const auto type = event->type();
        if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop &&
            type != QEvent::KeyPress && type != QEvent::ShortcutOverride)
            return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || !root_->isVisible() || widget->window() != root_->window() ||
            (widget != root_ && !root_->isAncestorOf(widget)))
            return false;
        if (type == QEvent::KeyPress || type == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!key->matches(QKeySequence::Paste) || !HasAssetMime(QApplication::clipboard()->mimeData()))
                return false;
            if (type == QEvent::KeyPress)
                handler_(QApplication::clipboard()->mimeData(), widget, widget->rect().center());
            key->accept();
            return true;
        }
        auto* drop = static_cast<QDropEvent*>(event);
        if (!HasAssetMime(drop->mimeData()) || !drop->possibleActions().testFlag(Qt::CopyAction))
            return false;
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (type == QEvent::Drop)
            handler_(drop->mimeData(), widget, drop->position().toPoint());
        return true;
    }
  private:
    QWidget* root_;
    Handler handler_;
};

// 图片预览标签。普通 QLabel 会保留给定的任意 pixmap，因此一旦图片被缩放到
// 固定框，编辑器列变窄时就会被裁剪（即用户看到的上下截断）。本标签则保留源
// pixmap，并按自身宽度重新缩放且保持宽高比，从而始终适配编辑器列并显示
// 完整图像。
class FigureImageLabel : public QLabel {
  public:
    explicit FigureImageLabel(QWidget* parent = nullptr) : QLabel(parent) {
        setAlignment(Qt::AlignCenter);
        // 水平方向 Ignored：由卡片的布局决定宽度（编辑器列）。垂直方向 Minimum：
        // 高度是下限而非上限，因此布局无法把标签压缩到缩放后图像高度以下。
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
        setMinimumWidth(1);
    }

    void SetSourcePixmap(const QPixmap& pixmap) {
        source_ = pixmap;
        last_width_ = -1;
        if (source_.isNull()) {
            setText(QStringLiteral("Image preview unavailable"));
            setMinimumHeight(0);
            return;
        }
        Rescale();
    }

    void SetFigureWidth(FigureWidth width) {
        fraction_ = (static_cast<int>(width) + 1) / 4.0;
        last_width_ = -1;
        Rescale();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        Rescale();
    }

  private:
    void Rescale() {
        if (source_.isNull())
            return;
        const int target = qMax(1, qRound(width() * fraction_));
        if (target == last_width_)
            return;
        last_width_ = target;
        const QPixmap scaled = source_.scaled(target, QWIDGETSIZE_MAX, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
    double fraction_ = 1.0;
};

// 插入入口提供的块类型，按菜单顺序排列。
// 已被 InsertOptionsFor() 取代，后者按位置和模板能力过滤；此处仅保留用于
// 块菜单的通用列表。
struct InsertEntry {
    const char* label;
    const char* kind;
};

std::vector<InsertEntry> InsertEntries(bool body_empty) {
    if (body_empty) {
        // 文本块需要有标题作为宿主，而节本身不需要锚点，
        // 因此空正文只能从节开始。
        return {{"Section Title", "section"}};
    }
    return {{"Text", "text"},         {"Section Title", "section"}, {"Subsection Title", "subsection"},
            {"Equation", "equation"}, {"Figure", "figure"},         {"Table…", "table"}};
}

// 内容是一段连续散文的行：它们会按块宽度重新排布，因此硬换行粘贴的内容
// 必须被柔化（见 ReflowHardWrappedText）。
bool IsProseRole(const QString& role) {
    return role == QLatin1String("abstract") || role == QLatin1String("caption");
}

// 显示类型 -> 排版角色（UI 方案 §3）。卡片外观（边距、标签）和编辑器字体
// 共用这一个映射。
theme::BlockVisualRole RoleForKind(const QString& kind) {
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
QString DisplayLabelFor(const QString& kind) {
    if (kind == QLatin1String("Abstract"))
        return QStringLiteral("ABSTRACT");
    if (kind == QLatin1String("Keywords"))
        return QStringLiteral("KEYWORDS");
    return kind;
}

// 子控件（编辑器、表头按钮）所属的块卡片。
QFrame* CardOf(const QObject* widget) {
    for (const QObject* p = widget; p != nullptr; p = p->parent()) {
        if (auto* frame = qobject_cast<QFrame*>(const_cast<QObject*>(p));
            frame && frame->objectName() == QLatin1String("blockCard")) {
            return frame;
        }
    }
    return nullptr;
}

// 相对 `container_kind` 允许插入的内容，依据模板的标题能力（方案 §3、§9）。
// 名称是 EditorItemKind 的机器名；MainWindow 将其映射到编辑命令。
struct InsertOption {
    const char* label;
    const char* kind;  // EditorItemKindName
    const char* group; // 菜单分组：Structure / Content
};

std::vector<InsertOption> InsertOptionsFor(bool body_empty, NodeKind container_kind, int max_heading_depth) {
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
    if (!in_subsubsection && 1 <= max_heading_depth) {
        options.push_back({"Section Title", "section", "Structure"});
    }
    if (!in_subsubsection && 2 <= max_heading_depth) {
        options.push_back({"Subsection Title", "subsection", "Structure"});
    }
    if ((in_section || in_subsection) && 3 <= max_heading_depth) {
        options.push_back({"Subsubsection Title", "subsubsection", "Structure"});
    }
    options.push_back({"Text", "text", "Content"});
    options.push_back({"Equation", "equation", "Content"});
    options.push_back({"Figure", "figure", "Content"});
    options.push_back({"Table…", "table", "Content"});
    options.push_back({"List", "list", "Content"});
    options.push_back({"Theorem", "theorem", "Content"});
    options.push_back({"Proof", "proof", "Content"});
    options.push_back({"Code", "code", "Content"});
    return options;
}

// 锚点节点所处的结构容器，以便插入菜单只提供该处模型所能表达的内容。
// 块继承其所在容器；标题自身就是一个容器。
NodeKind ContainerKindFor(const Document& doc, const QString& anchor) {
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
    void setReflowOnPaste(bool enabled) {
        reflow_on_paste_ = enabled;
    }

    explicit BlockEdit(bool single_line, QWidget* parent = nullptr)
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
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
                [this](const QSizeF&) { Resize(); });
        Resize();
    }

    // 角色字体 + 比例行高（UI 方案 §5）。程序性设置：绝不会将行标脏，且每次
    // setPlainText 重新加载后都会重新应用行高，因为该操作会重置块格式。
    void SetTypography(const QFont& font, int line_height_percent) {
        line_height_percent_ = line_height_percent;
        loading_ = true;
        setFont(font);
        theme::ApplyDocumentTypography(document(), font, line_height_percent);
        loading_ = false;
        Resize();
    }

    // 从文档进行的程序性加载：绝不把行标脏，也绝不触发提交。
    void SetInitialText(const QString& text) {
        loading_ = true;
        setPlainText(text);
        ApplyLineHeight();
        loading_ = false;
        dirty_ = false;
    }

    bool IsDirty() const {
        return dirty_;
    }
    void MarkClean() {
        dirty_ = false;
    }
    void MarkDirty() {
        dirty_ = true;
    }
    // 公式行在编辑态把焦点交给 Palette/属性控件时，不应把 focus-out 当成
    // “编辑结束”而提交并触发整卡重建（那会立刻退回展示态）。离开卡片时由
    // BlockEditor::SetEquationEditing(false) 统一提交。
    void SetSuppressFocusCommit(bool suppress) {
        suppress_focus_commit_ = suppress;
    }
    // 包裹程序性格式化（对齐、字体、提示），使其不会被误认为用户输入。
    void BeginProgrammaticEdit() {
        loading_ = true;
    }
    void EndProgrammaticEdit() {
        loading_ = false;
        dirty_ = false;
    }

    // 采纳模型文本，同时大致保持光标位置不变。
    void SetTextFromModel(const QString& text) {
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
        QTextDocument* doc = document();
        const QAbstractTextDocumentLayout* layout = doc->documentLayout();
        qreal per_block = 0.0;
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            per_block += layout->blockBoundingRect(block).height();
        }
        const int wrap_width = qMax(1, viewport()->width());
        const qreal measured =
            fontMetrics().boundingRect(QRect(0, 0, wrap_width, 0), Qt::TextWordWrap, toPlainText()).height();
        return qMax(measured, per_block) + 2.0 * doc->documentMargin();
    }

    void Resize() {
        const qreal doc_height = ContentHeight();
        const int target = qMax(
            qMax(static_cast<int>(qCeil(doc_height)) + 2 * frameWidth() + 6, fontMetrics().height() + 12), min_height_);
        if (height() != target)
            setFixedHeight(target);
        updateGeometry();
    }

  signals:
    void TriggerSlash();
    void CommitRequested();
    void NewBlockAfter();

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        // 在行首且上下文基本为空时输入「/」会打开块菜单。
        if (property("commands_enabled").toBool() && event->text() == QStringLiteral("/")) {
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
        if (single_line_ && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            emit CommitRequested();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
        if (document()->blockCount() >= 1)
            Resize();
    }

    void focusOutEvent(QFocusEvent* event) override {
        QPlainTextEdit::focusOutEvent(event);
        if (!suppress_focus_commit_)
            emit CommitRequested();
    }

    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        Resize();
    }

    // 撤销从 PDF 复制时带来的硬换行，使段落可以按块宽度重新排布，而不是
    // 保持固定的列宽。
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return HasAssetMime(source) || QPlainTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData* source) override {
        if (asset_paste_handler_ && asset_paste_handler_(source))
            return;
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
            theme::ApplyDocumentTypography(document(), document()->defaultFont(), line_height_percent_);
        }
    }

    bool single_line_ = false;
    bool loading_ = false;
    bool dirty_ = false;           // 用户已输入但尚未写入文档的内容
    bool reflow_on_paste_ = false; // 长文本行会重新排布粘贴的文本
    bool suppress_focus_commit_ = false; // 公式编辑态下由卡片统一提交
    int min_height_ = 0;
    int line_height_percent_ = 0;
  public:
    std::function<bool(const QMimeData*)> asset_paste_handler_;
};

// 卡片表头中的「\u22ee\u22ee」抓手。拖动它会开始一次重排序；
// 块之间的间隙负责接受放置。
class DragHandle : public QWidget {
    Q_OBJECT

  public:
    DragHandle(const QString& node_id, QWidget* parent) : QWidget(parent), node_id_(node_id) {
        setFixedWidth(18);
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("Drag to move this block"));
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(theme::kDisabledText), 1.4));
        // 抓手的两个竖列。
        const int cx = width() / 2;
        for (int dx : {-2, 2}) {
            painter.drawLine(cx + dx, 4, cx + dx, height() - 4);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            press_pos_ = event->pos();
            pressed_ = true;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!pressed_ || node_id_.isEmpty())
            return;
        if ((event->pos() - press_pos_).manhattanLength() < 6)
            return;
        pressed_ = false;

        auto* mime = new QMimeData();
        mime->setData(kBlockMime, node_id_.toUtf8());
        auto* drag = new QDrag(this);
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

    void mouseReleaseEvent(QMouseEvent* event) override {
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
    BlockGap(std::function<void(QWidget*)> on_activate, std::function<void(const QString&)> on_drop, QWidget* parent)
        : QWidget(parent), on_activate_(std::move(on_activate)), on_drop_(std::move(on_drop)) {
        setAcceptDrops(true);
        setFixedHeight(theme::spacing::kBlockGap);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Insert a block here"));
        setAttribute(Qt::WA_Hover, true);
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        QWidget::enterEvent(event);
        hovered_ = true;
        update();
    }

    void leaveEvent(QEvent* event) override {
        QWidget::leaveEvent(event);
        hovered_ = false;
        update();
    }

    void paintEvent(QPaintEvent*) override {
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

    void mousePressEvent(QMouseEvent* event) override {
        if (on_activate_)
            on_activate_(this);
        event->accept();
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(kBlockMime)) {
            drop_active_ = true;
            update();
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasFormat(kBlockMime)) {
            event->acceptProposedAction();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        drop_active_ = false;
        update();
        QWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
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
    void applyDrop(const QString& node_id) {
        if (on_drop_)
            on_drop_(node_id);
    }

  private:
    std::function<void(QWidget*)> on_activate_;
    std::function<void(const QString&)> on_drop_;
    bool hovered_ = false;
    bool drop_active_ = false;
};

} // namespace

#include "app/BlockEditor.moc"

// ---------------- BlockEditor ----------------

BlockEditor::BlockEditor(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    host_ = new QWidget(scroll_);
    host_->setStyleSheet(QString("background: %1;").arg(theme::kEditorBackground));
    auto* host_layout = new QVBoxLayout(host_);
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
    setAcceptDrops(true);
    host_->setAcceptDrops(true);
    scroll_->viewport()->setAcceptDrops(true);
    new AssetInputFilter(this, [this](const QMimeData* mime, QWidget* target, QPoint point) {
        return HandleAssetMime(mime, AssetAnchorAt(target, point));
    });
    // 焦点离开公式卡片时收起源码/Palette，只保留公式预览与编号。
    // 公式是否退出编辑态只由 application event filter 中的实际鼠标按下决定；
    // 焦点临时离开（Palette、补全弹层、窗口切换）不再触发收起。
    CenterContentColumn();
}

BlockEditor::~BlockEditor() {
    // 析构顺序：成员（blocks_）先于 QWidget 基类销毁。基类删除子控件时
    // 可能触发 focusOut -> InlineEditor::Committed / QLineEdit::editingFinished，
    // 而它们的接收者会读取 blocks_。因此在成员销毁前断开所有子控件信号。
    shutting_down_ = true;
    if (qApp && app_filter_installed_)
        qApp->removeEventFilter(this);
    for (QObject* child : findChildren<QObject*>())
        disconnect(child, nullptr, this, nullptr);
    for (auto& block : blocks_) {
        if (block.inline_editor) {
            block.inline_editor->blockSignals(true);
            disconnect(block.inline_editor, nullptr, this, nullptr);
        }
        if (block.editor) {
            block.editor->blockSignals(true);
            disconnect(block.editor, nullptr, this, nullptr);
        }
    }
    for (InlineEditor* editor : findChildren<InlineEditor*>()) {
        editor->blockSignals(true);
        disconnect(editor, nullptr, this, nullptr);
    }
    blocks_.clear();
}

void BlockEditor::UpdateEquationEventFilter() {
    if (!qApp)
        return;
    bool any_editing = false;
    for (const auto& block : blocks_) {
        if (block.equation_editing) {
            any_editing = true;
            break;
        }
    }
    if (any_editing && !app_filter_installed_) {
        qApp->installEventFilter(this);
        app_filter_installed_ = true;
    } else if (!any_editing && app_filter_installed_) {
        qApp->removeEventFilter(this);
        app_filter_installed_ = false;
    }
}

bool BlockEditor::IsEquationEditing(QWidget* card) const {
    for (const auto& block : blocks_) {
        if (block.card == card)
            return block.equation_editing;
    }
    return false;
}

void BlockEditor::SetEquationEditing(QWidget* card, bool editing) {
    if (!card)
        return;
    for (auto& block : blocks_) {
        if (block.card != card || !block.set_equation_editing)
            continue;
        const bool was_editing = block.equation_editing;
        block.equation_editing = editing;
        block.set_equation_editing(editing);
        UpdateEquationEventFilter();
        if (was_editing && !editing && block.editor) {
            // 源码 focusOut 已被抑制；离开卡片后补提交。用 queued 回调避免
            // CommitBlock 同步触发重建时删除正在遍历的 blocks_。
            const QString node_id = block.node_id;
            QTimer::singleShot(0, this, [this, node_id]() {
                for (auto& candidate : blocks_) {
                    if (candidate.node_id == node_id && candidate.editor) {
                        CommitBlock(candidate);
                        return;
                    }
                }
            });
        }
        return;
    }
}

void BlockEditor::CenterContentColumn() {
    auto* host_layout = qobject_cast<QVBoxLayout*>(host_->layout());
    if (!host_layout)
        return;
    const int side = qMax(24, (scroll_->viewport()->width() - theme::kContentWidth) / 2);
    const QMargins current = host_layout->contentsMargins();
    if (current.left() != side || current.right() != side) {
        host_layout->setContentsMargins(side, current.top(), side, current.bottom());
    }
}

QList<QAction*> BlockEditor::ConfigureFormatActions(InlineEditor* editor, bool include_footnote) {
    auto make = [editor](const QString& name, const QString& text, const QString& label, bool checkable = false) {
        auto* action = new QAction(text, editor);
        action->setObjectName(name);
        action->setIconText(label);
        action->setCheckable(checkable);
        return action;
    };
    auto* bold = make("inlineBoldAction", "Bold\tCtrl+B", "B", true);
    auto* italic = make("inlineItalicAction", "Italic\tCtrl+I", "I", true);
    auto* math = make("inlineMathAction", "Inline Math", "Inline Math");
    auto* citation = make("inlineCitationAction", "Citation", "Citation");
    auto* reference = make("inlineReferenceAction", "Reference", "Reference");
    auto* footnote = make("inlineFootnoteAction", "Footnote", "Footnote");
    connect(bold, &QAction::triggered, editor, &InlineEditor::ToggleBold);
    connect(italic, &QAction::triggered, editor, &InlineEditor::ToggleItalic);
    connect(math, &QAction::triggered, editor, &InlineEditor::BeginInlineMath);
    connect(citation, &QAction::triggered, this, [this, editor]() { ShowCitationPicker(editor); });
    connect(reference, &QAction::triggered, this, [this, editor]() { ShowReferencePicker(editor); });
    if (include_footnote) {
        connect(footnote, &QAction::triggered, this,
                [this, editor]() { ShowFootnoteEditor(editor, -1, InlineContent{}); });
        footnote->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
        footnote->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        editor->addAction(footnote);
    }
    // 浮动工具栏只在有选区时出现；快捷键让空光标也能直达选择器（P-14）。
    citation->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    reference->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    citation->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    reference->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    editor->addAction(citation);
    editor->addAction(reference);
    auto update_marks = [editor, bold, italic]() {
        bold->setChecked(editor->IsBoldActive());
        italic->setChecked(editor->IsItalicActive());
    };
    connect(editor, &QTextEdit::currentCharFormatChanged, editor, update_marks);
    connect(editor, &QTextEdit::selectionChanged, editor, update_marks);
    update_marks();
    QList<QAction*> actions{bold, italic, math, citation, reference};
    if (include_footnote) {
        actions << footnote;
    }
    editor->SetFormattingActions(actions);

    // WP-00/WP-01：点击正文中已插入的 citation / cross-reference pill 时打开
    // 同一个 picker 并替换该 token。这两个信号以前只 emit、无人接收。
    connect(editor, &InlineEditor::CitationTokenActivated, this,
            [this, editor](const QStringList& keys, int position, int mode) {
                CitationPickerSeed seed;
                seed.keys = keys;
                seed.narrative = mode == static_cast<int>(CitationMode::Narrative);
                seed.position = position;
                ShowCitationPicker(editor, seed);
            });
    connect(editor, &InlineEditor::CrossReferenceTokenActivated, this,
            [this, editor](const QString& target, int position) {
                ReferencePickerSeed seed;
                seed.target = target;
                seed.position = position;
                ShowReferencePicker(editor, seed);
            });
    // README 承诺的 '@' 菜单：行首/空白后输入 '@' 打开交叉引用选择器。
    connect(editor, &InlineEditor::ReferenceMenuRequested, this,
            [this, editor](const QPoint&) { ShowReferencePicker(editor); });
    connect(editor, &InlineEditor::FootnoteTokenActivated, this, [this, editor](int position) {
        const auto info = editor->FootnoteAt(position);
        if (info)
            ShowFootnoteEditor(editor, position, info->body);
    });
    return actions;
}

void BlockEditor::SetCitationCatalog(std::vector<pf::CitationView> entries) {
    citation_catalog_ = std::move(entries);
}

void BlockEditor::ShowCitationPicker(InlineEditor* editor) {
    ShowCitationPicker(editor, CitationPickerSeed{});
}

void BlockEditor::ShowCitationPicker(InlineEditor* editor, CitationPickerSeed seed) {
    if (!editor)
        return;
    std::vector<PopupList::Item> items;
    const auto make_item = [&](const pf::CitationView& view) {
        PopupList::Item item;
        const QString key = ToQ(view.key);
        const QString author = ToQ(view.author_label);
        const QString year = ToQ(view.year);
        const QString title = ToQ(view.title);
        const QString venue = ToQ(view.venue);
        if (!author.isEmpty()) {
            item.label = author;
            if (!year.isEmpty())
                item.label += QStringLiteral(" · ") + year;
        } else if (!title.isEmpty()) {
            item.label = title;
        } else {
            item.label = key;
        }
        // 标题只在没有作为主行兜底时显示为次行。
        if (!title.isEmpty() && title != item.label)
            item.title = title;
        // venue 是可变长度文本，只放进可省略的 detail 区域；引用编号放进
        // 独立 badge 区域，永远完整显示（期刊名再长也不会把它挤掉）。
        if (!venue.isEmpty())
            item.detail = venue;
        else if (!view.entry_type.empty())
            item.detail = ToQ(view.entry_type);
        if (view.cited)
            item.badge = QStringLiteral("[%1]").arg(view.number);
        item.group = view.cited ? QStringLiteral("Cited in this paper") : QStringLiteral("Not cited");
        item.payload = key;
        item.search = (author + QLatin1Char(' ') + title + QLatin1Char(' ') + venue + QLatin1Char(' ') + year +
                       QLatin1Char(' ') + key + QLatin1Char(' ') + ToQ(view.entry_type))
                          .toLower();
        item.checkable = true;
        item.checked = seed.keys.contains(key);
        return item;
    };

    for (const auto& view : citation_catalog_) {
        if (view.cited)
            items.push_back(make_item(view));
    }
    for (const auto& view : citation_catalog_) {
        if (!view.cited)
            items.push_back(make_item(view));
    }

    // 兼容路径：某些 widget 级测试只注入旧 reference_items_（没有 catalog）。
    // 这里仍提供多选与标题/编号不可用时的最小信息。
    if (citation_catalog_.empty()) {
        for (const auto& item : reference_items_) {
            if (!item.payload.startsWith(QStringLiteral("cite:")))
                continue;
            PopupList::Item entry;
            entry.label = item.label;
            entry.detail = item.detail;
            entry.title = item.title;
            entry.group = QStringLiteral("References");
            entry.payload = item.payload.mid(5);
            entry.search = item.search.isEmpty() ? item.label.toLower() : item.search.toLower();
            entry.checkable = true;
            entry.checked = seed.keys.contains(entry.payload);
            items.push_back(std::move(entry));
        }
    }

    if (items.empty()) {
        PopupList::Item empty;
        empty.label = QStringLiteral("No references imported");
        empty.title = QStringLiteral("Import a .bib file or add a reference first");
        empty.enabled = false;
        items.push_back(empty);
    }

    PopupList::Options options;
    options.multi_select = true;
    options.show_toggle = true;
    options.toggle_label = QStringLiteral("Narrative — Author [n]");
    options.toggle_initial = seed.narrative;
    options.width_px = 620;
    options.max_height_px = 460;
    options.empty_title = QStringLiteral("No references imported");
    options.empty_hint = QStringLiteral("Import a .bib file or add a reference first");

    // 引用方案 §4：选择器作用于弹出层获得焦点*之前*用户所在的光标位置。弹出层
    // 的焦点往返绝不能被误认为正文编辑的结束（BeginProtectedInsert 会抑制
    // focusOut 提交），而确认会立即完成语义插入/替换 + 提交。
    const QString node_id = editor->property("row_node").toString();
    const int caret = seed.position >= 0 ? seed.position : editor->textCursor().position();
    editor->BeginProtectedInsert();
    QPointer<InlineEditor> guard(editor);

    auto* popup = new PopupList(qobject_cast<QDialog*>(editor->window()) ? editor->window() : this);
    connect(popup, &PopupList::chosenMany, this,
            [this, guard, node_id, caret, seed](QStringList payloads, bool narrative) {
                if (guard)
                    guard->EndProtectedInsert();
                if (!guard || payloads.isEmpty())
                    return;
                const CitationMode mode = narrative ? CitationMode::Narrative : CitationMode::Parenthetical;
                bool replaced = false;
                if (seed.position >= 0) {
                    replaced = guard->ReplaceCitationAt(seed.position, payloads, mode);
                } else {
                    QTextCursor cursor(guard->document());
                    cursor.setPosition(qMax(0, qMin(caret, guard->document()->characterCount() - 1)));
                    guard->setTextCursor(cursor);
                    guard->InsertCitationObject(payloads, mode);
                    replaced = true;
                }
                if (!replaced)
                    return;
                guard->setFocus(Qt::OtherFocusReason);
                CommitInlineRow(guard.data());
                // 提交会刷新引用编号；把光标放回 pill 之后，便于继续输入。
                const int after = caret + 1;
                QTimer::singleShot(0, this, [this, guard, node_id, after]() {
                    if (guard) {
                        QTextCursor cursor(guard->document());
                        cursor.setPosition(qBound(0, after, guard->document()->characterCount() - 1));
                        guard->setTextCursor(cursor);
                        guard->setFocus(Qt::OtherFocusReason);
                        return;
                    }
                    for (auto& block : blocks_) {
                        if (block.inline_editor && block.node_id == node_id) {
                            QTextCursor c(block.inline_editor->document());
                            c.setPosition(qMax(0, qMin(after, block.inline_editor->document()->characterCount() - 1)));
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
    popup->popup(PickerPositionBelowCaret(editor), items, options);
}

void BlockEditor::ShowReferencePicker(InlineEditor* editor) {
    ShowReferencePicker(editor, ReferencePickerSeed{});
}

void BlockEditor::ShowReferencePicker(InlineEditor* editor, ReferencePickerSeed seed) {
    if (!editor)
        return;
    std::vector<PopupList::Item> items;
    const auto group_for = [](NodeKind kind) {
        switch (kind) {
        case NodeKind::Section:
        case NodeKind::Subsection:
        case NodeKind::Subsubsection:
            return QStringLiteral("Sections");
        case NodeKind::Equation:
            return QStringLiteral("Math");
        case NodeKind::Figure:
            return QStringLiteral("Figures");
        case NodeKind::Table:
            return QStringLiteral("Tables");
        case NodeKind::Theorem:
            return QStringLiteral("Theorems");
        case NodeKind::Code:
            return QStringLiteral("Listings");
        default:
            return QStringLiteral("Other");
        }
    };
    const auto kind_label = [](NodeKind kind) {
        switch (kind) {
        case NodeKind::Section:
            return QStringLiteral("Section");
        case NodeKind::Subsection:
            return QStringLiteral("Subsection");
        case NodeKind::Subsubsection:
            return QStringLiteral("Subsubsection");
        case NodeKind::Equation:
            return QStringLiteral("Equation");
        case NodeKind::Figure:
            return QStringLiteral("Figure");
        case NodeKind::Table:
            return QStringLiteral("Table");
        case NodeKind::Theorem:
            return QStringLiteral("Theorem");
        case NodeKind::Code:
            return QStringLiteral("Listing");
        default:
            return QStringLiteral("Reference");
        }
    };
    if (!cross_reference_items_.empty()) {
        for (const auto& reference : cross_reference_items_) {
            PopupList::Item entry;
            entry.label = ToQ(reference.display);
            entry.title = ToQ(reference.title);
            entry.detail = kind_label(reference.kind);
            if (seed.position >= 0 && reference.node.value() == seed.target.toStdString())
                entry.detail += QStringLiteral(" · current");
            entry.group = group_for(reference.kind);
            entry.payload = ToQ(reference.node.value());
            entry.search = (entry.label + QLatin1Char(' ') + entry.title).toLower();
            entry.checkable = false;
            entry.checked = reference.node.value() == seed.target.toStdString();
            items.push_back(std::move(entry));
        }
    } else {
        // 兼容路径：只有旧 reference_items_ 的 widget 级测试。
        for (const auto& item : reference_items_) {
            if (!item.payload.startsWith(QStringLiteral("xref:")))
                continue;
            PopupList::Item entry;
            entry.label = item.label;
            entry.title = item.title;
            entry.detail = item.detail;
            entry.group = item.group.isEmpty() ? QStringLiteral("Cross references") : item.group;
            entry.payload = item.payload.mid(5);
            entry.search = item.search.isEmpty() ? item.label.toLower() : item.search.toLower();
            entry.checkable = false;
            entry.checked = entry.payload == seed.target;
            items.push_back(std::move(entry));
        }
    }
    if (items.empty()) {
        PopupList::Item empty;
        empty.label = QStringLiteral("Nothing to reference yet");
        empty.title = QStringLiteral("Add a section, figure, table, equation, theorem or code block first");
        empty.enabled = false;
        items.push_back(empty);
    }
    // 同一分组必须相邻，否则列表会重复出现 group 标题。顺序与用户查找
    // 目标的心智一致：Sections → Math → Figures → Tables。
    const auto group_rank = [](const QString& group) {
        if (group == QStringLiteral("Sections"))
            return 0;
        if (group == QStringLiteral("Math"))
            return 1;
        if (group == QStringLiteral("Figures"))
            return 2;
        if (group == QStringLiteral("Tables"))
            return 3;
        if (group == QStringLiteral("Theorems"))
            return 4;
        if (group == QStringLiteral("Listings"))
            return 5;
        return 4;
    };
    std::stable_sort(items.begin(), items.end(), [&](const PopupList::Item& a, const PopupList::Item& b) {
        return group_rank(a.group) < group_rank(b.group);
    });
    PopupList::Options options;
    options.width_px = 520;
    options.max_height_px = 440;
    options.empty_title = QStringLiteral("Nothing to reference yet");
    options.empty_hint = QStringLiteral("Add a section, figure, table or equation first");

    const QString node_id = editor->property("row_node").toString();
    const int caret = seed.position >= 0 ? seed.position : editor->textCursor().position();
    editor->BeginProtectedInsert();
    QPointer<InlineEditor> guard(editor);
    auto* popup = new PopupList(qobject_cast<QDialog*>(editor->window()) ? editor->window() : this);
    connect(popup, &PopupList::chosen, this, [this, guard, node_id, caret, seed](QString payload) {
        const QString target = payload.trimmed();
        if (guard)
            guard->EndProtectedInsert();
        if (target.isEmpty() || !guard)
            return;
        bool replaced = false;
        if (seed.position >= 0) {
            replaced = guard->ReplaceCrossReferenceAt(seed.position, target);
        } else {
            QTextCursor cursor(guard->document());
            cursor.setPosition(qMax(0, qMin(caret, guard->document()->characterCount() - 1)));
            guard->setTextCursor(cursor);
            guard->InsertCrossReferenceObject(target);
            replaced = true;
        }
        if (!replaced)
            return;
        guard->setFocus(Qt::OtherFocusReason);
        CommitInlineRow(guard.data());
        const int after = caret + 1;
        QTimer::singleShot(0, this, [this, guard, node_id, after]() {
            if (guard) {
                QTextCursor cursor(guard->document());
                cursor.setPosition(qBound(0, after, guard->document()->characterCount() - 1));
                guard->setTextCursor(cursor);
                guard->setFocus(Qt::OtherFocusReason);
                return;
            }
            for (auto& block : blocks_) {
                if (block.inline_editor && block.node_id == node_id) {
                    QTextCursor c(block.inline_editor->document());
                    c.setPosition(qMax(0, qMin(after, block.inline_editor->document()->characterCount() - 1)));
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
    popup->popup(PickerPositionBelowCaret(editor), items, options);
}

// 立即提交一个富文本行：选择器不得依赖稍后的 focusOut。文档通过
// 对应的富文本信号同步更新文档。提交语义与 CommitBlock 完全一致。
void BlockEditor::CommitInlineRow(InlineEditor* editor) {
    if (!editor)
        return;
    for (auto& block : blocks_) {
        if (block.inline_editor == editor) {
            CommitBlock(block);
            return;
        }
    }
}

bool BlockEditor::InsertCitationIntoParagraph(const QString& node_id, const QString& citation_key, int insert_offset) {
    for (auto& block : blocks_) {
        if (block.node_id != node_id || !block.inline_editor)
            continue;
        InlineEditor* editor = block.inline_editor;
        if (insert_offset >= 0) {
            QTextCursor cursor(editor->document());
            cursor.setPosition(qMin(insert_offset, editor->document()->characterCount() - 1));
            editor->setTextCursor(cursor);
        }
        editor->InsertCitationObject({citation_key});
        editor->setFocus(Qt::OtherFocusReason);
        CommitInlineRow(editor);
        return true;
    }
    return false;
}

void BlockEditor::SetCitationNumbers(std::shared_ptr<const pf::CitationNumberResolver> numbers) {
    if (citation_numbers_ == numbers)
        return;
    citation_numbers_ = std::move(numbers);
    if (cell_dialog_)
        cell_dialog_->SetCitationResolver(citation_numbers_);
    if (footnote_dialog_)
        footnote_dialog_->SetCitationResolver(citation_numbers_);
    for (auto& block : blocks_) {
        if (block.inline_editor) {
            block.inline_editor->SetCitationNumbers(citation_numbers_);
        }
    }
}

std::map<QString, QString> BlockEditor::CrossReferenceLabels() const {
    std::map<QString, QString> labels;
    if (!cross_reference_items_.empty()) {
        for (const auto& item : cross_reference_items_) {
            if (!item.node.empty() && !item.display.empty())
                labels[ToQ(item.node.value())] = ToQ(item.display);
        }
        return labels;
    }
    // 兼容路径：只有旧 reference_items_ 的 widget 级测试仍能显示标题。
    for (const auto& item : reference_items_) {
        if (!item.payload.startsWith(QStringLiteral("xref:")))
            continue;
        const QString node = item.payload.mid(5);
        if (!node.isEmpty() && !item.label.isEmpty())
            labels[node] = item.label;
    }
    return labels;
}

void BlockEditor::SetNumbering(std::shared_ptr<const pf::DocumentNumberer> numbering) {
    numbering_ = std::move(numbering);
    // 只更新编号标签，不重建行：用户可能正在编辑公式，不能让焦点丢失。
    for (auto& block : blocks_) {
        if (!block.card || block.kind != QLatin1String("Equation"))
            continue;
        auto* label = block.card->findChild<QLabel*>(QStringLiteral("equationNumber"));
        if (!label)
            continue;
        QString text;
        if (block.equation_numbered && numbering_) {
            if (const auto entry = numbering_->Find(NodeId(block.node_id.toStdString())))
                text = QStringLiteral("(%1)").arg(entry->number);
        }
        label->setText(text);
        label->setVisible(!text.isEmpty());
    }
}

void BlockEditor::SetMathPackages(std::vector<std::string> packages) {
    math_packages_ = std::move(packages);
    if (cell_dialog_)
        cell_dialog_->Editor()->SetMathPackages(math_packages_);
    if (footnote_dialog_)
        footnote_dialog_->Editor()->SetMathPackages(math_packages_);
    for (auto& block : blocks_) {
        if (block.inline_editor)
            block.inline_editor->SetMathPackages(math_packages_);
    }
}

void BlockEditor::SetCrossReferences(std::vector<pf::CrossReferenceItem> items) {
    cross_reference_items_ = std::move(items);
    const auto labels = CrossReferenceLabels();
    if (cell_dialog_)
        cell_dialog_->SetCrossReferenceLabels(labels);
    if (footnote_dialog_)
        footnote_dialog_->SetCrossReferenceLabels(labels);
    for (auto& block : blocks_) {
        if (block.inline_editor)
            block.inline_editor->SetCrossReferenceLabels(labels);
    }
}

InlineEditor* BlockEditor::NewInlineEditor(QWidget* card, const QString& node_id, const InlineContent& content,
                                          const QString& outline_key, theme::BlockVisualRole role,
                                          const QString& commit_role) {
    auto* editor = new InlineEditor(card);
    editor->SetAssetPasteHandler([this, node_id](const QMimeData* mime) { return HandleAssetMime(mime, node_id); });
    // 顺序很重要（UI 方案 §6）：加载内容之前先建立对应角色的字体环境，
    // 因为 InsertMathObject 在插入时依据 document()->defaultFont() 来确定行内
    // 数学公式的尺寸与对齐。
    editor->SetBodyTypography(theme::EditorFont(role), theme::LineHeightFor(role));
    editor->SetContent(content);
    editor->setProperty("row_node", node_id);
    editor->setProperty("row_focus_key", node_id);
    editor->setProperty("row_outline_key", outline_key);
    editor->setProperty("row_commit_role", commit_role);
    // Pill 使用文档范围的编号/标签映射进行渲染（引用方案 §3）：行内的 Citation
    // 对象保持语义化，仅其绘制依赖这些映射。
    editor->SetCitationNumbers(citation_numbers_);
    editor->SetCrossReferenceLabels(CrossReferenceLabels());
    editor->SetMathPackages(math_packages_);
    editor->installEventFilter(this); // 焦点状态 + 大纲同步

    connect(editor, &InlineEditor::Committed, this, [this, editor]() { CommitInlineRow(editor); });
    connect(editor, &InlineEditor::NewBlockAfter, this,
            [this, node_id]() { emit InsertBlockRequested(QStringLiteral("text"), node_id); });

    qobject_cast<QVBoxLayout*>(card->layout())->addWidget(editor);
    ConfigureFormatActions(editor);

    Block block;
    block.node_id = node_id;
    block.kind = card->property("row_kind").toString();
    block.card = card;
    block.inline_editor = editor;
    block.commit_role = commit_role;
    block.committed_text = ToQ(pf::InlineToPlainText(content));
    block.committed_content = content;
    block.outline_key = outline_key;
    blocks_.push_back(std::move(block));
    return editor;
}

QWidget* BlockEditor::MakeTextCard(const QString& node_id, const InlineContent& content, const QString& outline_key) {
    auto* card = MakeCard(node_id, QStringLiteral("Text"), QStringLiteral("paragraph"), true);
    auto* editor = NewInlineEditor(card, node_id, content, outline_key, theme::BlockVisualRole::Body,
                                    QStringLiteral("paragraph"));
    editor->setPlaceholderText(QStringLiteral("Write text…  Ctrl+Enter adds a new block"));
    return card;
}

QWidget* BlockEditor::MakeEquationCard(const QString& node_id, const pf::EquationBlock& equation,
                                       const QString& outline_key) {
    // 设计 §4：公式行拥有 LaTeX Source、Preview、Numbered 和 Label。
    // 用户始终只编辑数学正文。
    auto* card = MakeCard(node_id, QStringLiteral("Equation"), QStringLiteral("equation"), true);
    card->setProperty("equation_card", true);
    // 卡片空白处的点击不能继续冒泡到外层，再被全局过滤器误判为外部点击。
    card->setAttribute(Qt::WA_NoMousePropagation);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());

    auto* source = NewEditor(card, ToQ(equation.expression.latex), 2, theme::BlockVisualRole::EquationSource);
    // Palette 的 NoFocus 子控件会沿父链寻找鼠标焦点；在卡片处转交给源码，
    // 避免焦点落到外层 QScrollArea。先设策略，再设代理，保留源码的键盘焦点策略。
    card->setFocusPolicy(Qt::ClickFocus);
    card->setFocusProxy(source);
    source->setPlaceholderText(QStringLiteral("LaTeX body, e.g. \\frac{\\partial u}{\\partial t}"));
    source->setProperty("row_node", node_id);
    source->setProperty("row_focus_key", node_id);
    source->setProperty("row_outline_key", outline_key);
    source->setProperty("commands_enabled", true);
    if (auto* block_edit = qobject_cast<BlockEdit*>(source)) {
        block_edit->setReflowOnPaste(false);
        block_edit->MarkClean();
    }

    // WP-02：命令补全始终可用；面板默认折叠，避免每张公式卡片都被符号
    // 键盘占满。点击 "∑ Palette" 展开。
    auto* completer = new MathCommandCompleter(source);
    completer->Attach(source);
    completer->SetAvailablePackages(math_packages_);
    auto* palette_toggle = new QToolButton(card);
    palette_toggle->setText(QStringLiteral("∑ Palette"));
    palette_toggle->setCheckable(true);
    palette_toggle->setAutoRaise(true);
    palette_toggle->setFocusPolicy(Qt::NoFocus);
    palette_toggle->setProperty("row_node", node_id);
    card_layout->addWidget(palette_toggle);
    auto* palette = new MathPaletteWidget(card);
    palette->SetTarget(source);
    palette->SetAvailablePackages(math_packages_);
    palette->setVisible(false);
    card_layout->addWidget(palette);
    connect(palette_toggle, &QToolButton::toggled, palette, [palette](bool visible) { palette->setVisible(visible); });

    // 公式编号贴在预览右侧，模拟论文里的 “...        (3)”。编号来自
    // DocumentNumberer（全文自动排序）；Numbered 关闭时显示为空。
    QString number_text;
    if (equation.numbered && numbering_) {
        if (const auto numbered_node = numbering_->Find(NodeId(node_id.toStdString())))
            number_text = QStringLiteral("(%1)").arg(numbered_node->number);
    }
    auto* number_label = new QLabel(number_text, card);
    number_label->setObjectName(QStringLiteral("equationNumber"));
    number_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    number_label->setStyleSheet(QString("color: %1; font-style: italic; padding-left: 8px;")
                                    .arg(theme::kSecondaryText));
    number_label->setVisible(!number_text.isEmpty());

    auto* preview = new QLabel(card);
    preview->setObjectName(QStringLiteral("equationPreview"));
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(56);
    preview->setCursor(Qt::PointingHandCursor);
    preview->setToolTip(QStringLiteral("Click to edit this equation"));
    preview->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 6px;"
                                   " color: %3;")
                               .arg(theme::kEditorBackground, theme::kDivider, theme::kSecondaryText));
    auto* preview_row = new QWidget(card);
    auto* preview_row_layout = new QHBoxLayout(preview_row);
    preview_row_layout->setContentsMargins(0, 0, 0, 0);
    preview_row_layout->setSpacing(4);
    preview_row_layout->addWidget(preview, 1);
    preview_row_layout->addWidget(number_label, 0, Qt::AlignVCenter);
    card_layout->addWidget(preview_row);
    // 展示态点击预览/编号/空白区域都会进入编辑态。
    preview->installEventFilter(this);
    preview_row->installEventFilter(this);
    number_label->installEventFilter(this);

    auto* status = new QLabel(card);
    status->setWordWrap(true);
    card_layout->addWidget(status);

    auto* controls = new QWidget(card);
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(6);
    auto* numbered = new QCheckBox(QStringLiteral("Numbered"), controls);
    numbered->setChecked(equation.numbered);
    numbered->setProperty("row_node", node_id);
    numbered->setToolTip(QStringLiteral("Numbered equations get an equation number and a label"));
    auto* label_caption = new QLabel(QStringLiteral("Label"), controls);
    label_caption->setStyleSheet(QString("color: %1;").arg(theme::kSecondaryText));
    auto* label_edit = new QLineEdit(controls);
    label_edit->setPlaceholderText(QStringLiteral("eq:energy"));
    label_edit->setText(ToQ(equation.label));
    label_edit->setProperty("row_node", node_id);
    label_edit->setToolTip(QStringLiteral("LaTeX label used by cross references, e.g. eq:energy"));
    controls_layout->addWidget(numbered);
    controls_layout->addWidget(label_caption);
    controls_layout->addWidget(label_edit, 1);
    card_layout->addWidget(controls);

    // 展示态：只保留预览 + 编号；编辑态才显示源码、Palette 按钮与属性控件。
    auto apply_editing_mode = [source, palette_toggle, palette, status, controls](bool editing) {
        source->setVisible(editing);
        palette_toggle->setVisible(editing);
        status->setVisible(editing);
        controls->setVisible(editing);
        if (editing) {
            // 进入编辑态时直接展开 Palette：用户点击公式就是想要图形化输入，
            // 不应再点第二次。toggle 仍可手动收起。
            palette_toggle->setChecked(true);
            palette->setVisible(true);
        } else {
            palette_toggle->setChecked(false);
            palette->setVisible(false);
        }
    };
    apply_editing_mode(false);

    // P0-07：逐卡片的异步渲染状态。`card_id` 每张卡片唯一，并向服务注册；
    // generation 用于防范在同一卡片已有更新请求之后才到达的应答。
    const QString card_id = QStringLiteral("equation-card-") + node_id;
    // generation 存放在堆上：下面的服务连接会存活到本函数返回之后，
    // 因此捕获栈局部变量的引用会让 lambda 读取已释放的内存
    // （ASan: stack-use-after-return）。
    auto preview_generation = std::make_shared<std::uint64_t>(0);

    // 源码变化 -> 校验 -> 请求渲染 -> 预览（设计 §7）。
    auto refresh = [preview, status, card_id, preview_generation](const QString& latex) {
        const pf::MathValidation validation = pf::ValidateMath(latex.toStdString(), pf::MathFlavor::Display);
        if (validation.invalid()) {
            status->setText(QStringLiteral("Invalid — %1").arg(QString::fromStdString(validation.error)));
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
        *preview_generation = MathRenderService::Shared()->Request(card_id, QStringLiteral("display"), latex, style);
    };
    // P0-07：仅将异步结果应用到本卡片。服务已经丢弃了来自被取代 generation 的
    // 应答；此处的 id + generation 检查可防止被复用的卡片显示出别的公式。
    MathRenderService* math_service = MathRenderService::Shared();
    math_service->RegisterClient(card_id, preview);
    connect(math_service, &MathRenderService::mathRendered, preview,
            [preview, status, card_id, preview_generation](const MathRenderResponse& response) {
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
                preview->setToolTip(response.result.exact ? QString() : response.result.note);
                if (!response.result.exact && !response.result.note.isEmpty() && status->text().isEmpty())
                    status->setText(QStringLiteral("Approximate preview: %1").arg(response.result.note));
            });
    refresh(ToQ(equation.expression.latex));

    auto* timer = new QTimer(card);
    timer->setSingleShot(true);
    timer->setInterval(160);
    connect(source, &QPlainTextEdit::textChanged, timer, [timer]() { timer->start(); });
    connect(timer, &QTimer::timeout, card, [source, refresh]() { refresh(source->toPlainText()); });

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
    block.equation_editing = false;
    block.set_equation_editing = apply_editing_mode;
    blocks_.push_back(std::move(block));

    BlockEdit* block_edit = qobject_cast<BlockEdit*>(source);
    block_edit->SetSuppressFocusCommit(true);
    connect(block_edit, &BlockEdit::CommitRequested, this, [this, source]() {
        for (auto& candidate : blocks_) {
            if (candidate.editor == source) {
                CommitBlock(candidate);
                break;
            }
        }
    });

    // Numbered / label 的变更是独立属性；它们会立即提交。块通过其编辑器查找，
    // 因为文档其余部分仍在重建期间 `blocks_` 可能重新分配。
    connect(numbered, &QCheckBox::toggled, this, [this, source, label_edit](bool on) {
        for (auto& candidate : blocks_) {
            if (candidate.editor != source)
                continue;
            candidate.equation_numbered = on;
            emit EquationEdited(candidate.node_id, source->toPlainText(), on, label_edit->text());
            break;
        }
    });
    connect(label_edit, &QLineEdit::editingFinished, this, [this, source, numbered]() {
        const QLineEdit* edit = qobject_cast<QLineEdit*>(sender());
        const QString label = edit ? edit->text() : QString();
        for (auto& candidate : blocks_) {
            if (candidate.editor != source)
                continue;
            candidate.equation_label = label;
            emit EquationEdited(candidate.node_id, source->toPlainText(), numbered->isChecked(), label);
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

QLineEdit* BlockEditor::AddLabelEditor(QWidget* card, const QString& node_id, const QString& label,
                                         const QString& placeholder,
                                         const std::function<void(const QString&)>& commit) {
    auto* row = new QWidget(card);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(6);
    auto* caption = new QLabel(QStringLiteral("Label"), row);
    caption->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(theme::kSecondaryText));
    auto* edit = new QLineEdit(label, row);
    edit->setPlaceholderText(placeholder);
    edit->setProperty("row_node", node_id);
    edit->setProperty("row_focus_key", node_id);
    edit->setToolTip(QStringLiteral("LaTeX label used by \\ref; leave empty to use the node id"));
    row_layout->addWidget(caption);
    row_layout->addWidget(edit, 1);
    qobject_cast<QVBoxLayout*>(card->layout())->addWidget(row);
    // 失焦/回车时提交，但只在值确实变化时发命令，避免每次点击都重建卡片。
    connect(edit, &QLineEdit::editingFinished, this, [edit, committed = label, commit]() mutable {
        const QString text = edit->text().trimmed();
        // 提交后清除 modified：否则 HasUncommittedFocus 会一直认为这行还有
        // 未写入的输入，推迟后续结构性刷新。
        edit->setModified(false);
        if (text != committed) {
            committed = text;
            commit(text);
        }
    });
    return edit;
}

QWidget* BlockEditor::MakeFigureCard(const pf::Figure& figure, const QString& outline_key) {
    const QString node_id = ToQ(figure.id.value());
    QWidget* card = MakeCard(node_id, "Figure", "caption", true);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    auto* image = new FigureImageLabel(card);
    image->setObjectName(QStringLiteral("figurePreview"));
    image->setProperty("figure_asset", ToQ(figure.asset_id.value()));
    image->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 6px;"
                                 "color: %3;")
                             .arg(theme::kSidePanel, theme::kDivider, theme::kSecondaryText));
    const QString path = asset_path_resolver_ ? asset_path_resolver_(figure.asset_id) : QString();
    // 缩放到编辑器列宽，而非固定框：标签保持完整图像可见，
    // 并在窗格尺寸变化时重新适配。
    const bool pdf = QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0;
    image->setToolTip(pdf ? QStringLiteral("PDF first-page preview. If unavailable, install pdftoppm (Poppler) and restart the editor.\n") + path : path);
    image->SetSourcePixmap(figure_preview_resolver_ ? figure_preview_resolver_(figure.asset_id, QSize(1200, 1200))
                                                   : path.isEmpty() ? QPixmap() : QPixmap(path));
    image->SetFigureWidth(figure.width);
    card_layout->addWidget(image);
    auto* caption = NewInlineEditor(card, node_id, figure.caption, outline_key, theme::BlockVisualRole::Caption,
                                     QStringLiteral("caption"));
    caption->setObjectName(QStringLiteral("captionEditor"));
    caption->setPlaceholderText("Figure caption");
    AddLabelEditor(card, node_id, ToQ(figure.label), QStringLiteral("fig:architecture"),
                   [this, node_id](const QString& text) { emit FigureLabelEdited(node_id, text); });
    // 单栏 vs 双栏图片。无论模板是什么都记录在文档上；
    // 只有双栏模板才会导出差异（见 LatexRenderer：DoubleColumn 会输出带星号的
    // 浮动体）。
    auto* span_box = new QCheckBox(QStringLiteral("Span both columns (double-column figure)"), card);
    span_box->setChecked(figure.span == pf::FigureSpan::DoubleColumn);
    span_box->setToolTip(QStringLiteral("Double-column figure. Has no visible effect until the paper template "
                                        "uses two columns."));
    span_box->setProperty("row_node", node_id);
    span_box->setProperty("row_focus_key", node_id);
    span_box->setProperty("row_outline_key", outline_key);
    card_layout->addWidget(span_box);
    connect(span_box, &QCheckBox::toggled, this,
            [this, node = node_id](bool double_column) { emit FigureSpanChanged(node, double_column); });
    auto* width_row = new QWidget(card);
    auto* width_layout = new QHBoxLayout(width_row);
    width_layout->setContentsMargins(0, 0, 0, 0);
    width_layout->addWidget(new QLabel(QStringLiteral("Width"), width_row));
    auto* width = new QComboBox(width_row);
    width->setObjectName(QStringLiteral("figureWidth"));
    width->addItems({QStringLiteral("25%"), QStringLiteral("50%"), QStringLiteral("75%"), QStringLiteral("100%")});
    width->setCurrentIndex(static_cast<int>(figure.width));
    width->setToolTip(QStringLiteral("Width relative to the figure's column or full-width float"));
    width_layout->addWidget(width);
    width_layout->addStretch(1);
    auto* replace = new QPushButton(QStringLiteral("Replace image…"), width_row);
    replace->setObjectName(QStringLiteral("replaceFigureAsset"));
    width_layout->addWidget(replace);
    connect(replace, &QPushButton::clicked, this, [this, node_id]() { emit FigureAssetReplaceRequested(node_id); });
    card_layout->addWidget(width_row);
    connect(width, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, node_id, image](int index) {
                image->SetFigureWidth(static_cast<FigureWidth>(index));
                emit FigureWidthChanged(node_id, static_cast<FigureWidth>(index));
            });
    return card;
}

QWidget* BlockEditor::MakeTableCard(const pf::Table& table, const QString& outline_key) {
    const QString node_id = ToQ(table.id.value());
    QWidget* card = MakeCard(node_id, "Table", "caption", true);
    auto* grid = new QTableWidget(static_cast<int>(table.RowCount()), static_cast<int>(table.ColumnCount()), card);
    card->setFocusPolicy(Qt::ClickFocus);
    card->setFocusProxy(grid);
    grid->setObjectName(QStringLiteral("tableGrid"));
    grid->setProperty("row_node", node_id);
    grid->setProperty("row_outline_key", outline_key);
    grid->installEventFilter(this);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    grid->setSelectionMode(QAbstractItemView::SingleSelection);
    grid->setToolTip(QStringLiteral("Double-click a cell to edit text, math, citations and references"));
    grid->setMaximumHeight(qMin(260, 34 + 32 * grid->rowCount()));
    for (int row = 0; row < grid->rowCount(); ++row) {
        if (row == 0 && table.has_header_row)
            grid->setVerticalHeaderItem(row, new QTableWidgetItem(QStringLiteral("1 · Header")));
        for (int column = 0; column < grid->columnCount(); ++column) {
            auto* item = new QTableWidgetItem(ToQ(pf::InlineToPlainText(table.cells[row][column].content)));
            const auto alignment = table.columns[static_cast<size_t>(column)].alignment;
            item->setTextAlignment(Qt::AlignVCenter | (alignment == ColumnAlignment::Left ? Qt::AlignLeft
                                                      : alignment == ColumnAlignment::Center ? Qt::AlignHCenter
                                                                                             : Qt::AlignRight));
            if (row == 0 && table.has_header_row) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
            }
            grid->setItem(row, column, item);
        }
    }
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    card_layout->addWidget(grid);
    if (grid->rowCount() && grid->columnCount())
        grid->setCurrentCell(0, 0);
    connect(grid, &QTableWidget::cellDoubleClicked, this,
            [this, node_id, cells = table.cells](int row, int column) {
                if (row >= 0 && static_cast<size_t>(row) < cells.size() && column >= 0 &&
                    static_cast<size_t>(column) < cells[static_cast<size_t>(row)].size())
                    OpenTableCellEditor(node_id, row, column, cells[static_cast<size_t>(row)][static_cast<size_t>(column)].content);
            });

    auto* tools = new QWidget(card);
    auto* tools_layout = new QHBoxLayout(tools);
    tools_layout->setContentsMargins(0, 0, 0, 0);
    tools_layout->setSpacing(4);
    auto button = [tools, tools_layout](const QString& name, const QString& text) {
        auto* result = new QToolButton(tools);
        result->setObjectName(name);
        result->setText(text);
        result->setFocusPolicy(Qt::NoFocus);
        result->setAutoRaise(true);
        tools_layout->addWidget(result);
        return result;
    };
    auto* add_row = button(QStringLiteral("tableAddRow"), QStringLiteral("+ Row"));
    auto* delete_row = button(QStringLiteral("tableDeleteRow"), QStringLiteral("− Row"));
    auto* add_column = button(QStringLiteral("tableAddColumn"), QStringLiteral("+ Column"));
    auto* delete_column = button(QStringLiteral("tableDeleteColumn"), QStringLiteral("− Column"));
    add_row->setEnabled(table.RowCount() < Table::kMaxRows);
    add_column->setEnabled(table.ColumnCount() < Table::kMaxColumns);
    add_row->setToolTip(QStringLiteral("Insert a row below the selected cell"));
    delete_row->setToolTip(QStringLiteral("Delete the selected row"));
    add_column->setToolTip(QStringLiteral("Insert a column after the selected cell"));
    delete_column->setToolTip(QStringLiteral("Delete the selected column"));
    auto* alignment = new QComboBox(tools);
    alignment->setObjectName(QStringLiteral("tableColumnAlignment"));
    alignment->addItems({QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")});
    alignment->setToolTip(QStringLiteral("Alignment of the selected column"));
    tools_layout->addWidget(alignment);
    auto* header = new QCheckBox(QStringLiteral("Header row"), tools);
    header->setObjectName(QStringLiteral("tableHeaderRow"));
    header->setChecked(table.has_header_row);
    tools_layout->addWidget(header);
    tools_layout->addStretch(1);
    card_layout->addWidget(tools);
    auto update_selection = [grid, delete_row, delete_column, alignment, columns = table.columns]() {
        delete_row->setEnabled(grid->rowCount() > 1 && grid->currentRow() >= 0);
        delete_column->setEnabled(grid->columnCount() > 1 && grid->currentColumn() >= 0);
        const int column = grid->currentColumn();
        alignment->setEnabled(column >= 0 && static_cast<size_t>(column) < columns.size());
        const QSignalBlocker blocker(alignment);
        alignment->setCurrentIndex(alignment->isEnabled() ? static_cast<int>(columns[static_cast<size_t>(column)].alignment) : -1);
    };
    connect(grid, &QTableWidget::currentCellChanged, tools, update_selection);
    update_selection();
    connect(add_row, &QToolButton::clicked, this, [this, grid, node_id]() {
        emit TableRowInsertRequested(node_id, grid->currentRow() < 0 ? grid->rowCount() : grid->currentRow() + 1);
    });
    connect(delete_row, &QToolButton::clicked, this, [this, grid, node_id]() {
        if (grid->currentRow() >= 0)
            emit TableRowDeleteRequested(node_id, grid->currentRow());
    });
    connect(add_column, &QToolButton::clicked, this, [this, grid, node_id]() {
        emit TableColumnInsertRequested(node_id, grid->currentColumn() < 0 ? grid->columnCount() : grid->currentColumn() + 1);
    });
    connect(delete_column, &QToolButton::clicked, this, [this, grid, node_id]() {
        if (grid->currentColumn() >= 0)
            emit TableColumnDeleteRequested(node_id, grid->currentColumn());
    });
    connect(alignment, QOverload<int>::of(&QComboBox::activated), this, [this, grid, node_id](int index) {
        if (grid->currentColumn() >= 0)
            emit TableColumnAlignmentChanged(node_id, grid->currentColumn(), static_cast<ColumnAlignment>(index));
    });
    connect(header, &QCheckBox::toggled, this,
            [this, node_id](bool checked) { emit TableHeaderRowChanged(node_id, checked); });

    auto* caption = NewInlineEditor(card, node_id, table.caption, outline_key, theme::BlockVisualRole::Caption,
                                     QStringLiteral("caption"));
    caption->setObjectName(QStringLiteral("captionEditor"));
    caption->setPlaceholderText("Table caption");
    AddLabelEditor(card, node_id, ToQ(table.label), QStringLiteral("tab:results"),
                   [this, node_id](const QString& text) { emit TableLabelEdited(node_id, text); });
    return card;
}

void BlockEditor::ShowFootnoteEditor(InlineEditor* editor, int position, const InlineContent& initial) {
    if (!editor || footnote_dialog_)
        return;
    auto* dialog = new InlineContentEditorDialog(initial, this);
    footnote_dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(position < 0 ? QStringLiteral("Insert Footnote") : QStringLiteral("Edit Footnote"));
    dialog->SetPlaceholder(QStringLiteral("Footnote text…"));
    dialog->SetCitationResolver(citation_numbers_);
    dialog->SetCrossReferenceLabels(CrossReferenceLabels());
    auto* draft = dialog->Editor();
    draft->setProperty("row_node", editor->property("row_node"));
    draft->SetMathPackages(math_packages_);
    dialog->SetAllowedActions(ConfigureFormatActions(draft, false));
    QPointer<InlineEditor> target(editor);
    editor->BeginProtectedInsert();
    connect(dialog, &QDialog::finished, this, [this, dialog, target, position, initial](int result) {
        if (result == QDialog::Accepted && target) {
            const InlineContent content = dialog->content();
            if (position < 0)
                target->InsertFootnoteObject(content);
            else
                target->ReplaceFootnoteAt(position, content);
            target->setFocus(Qt::OtherFocusReason);
        }
        if (target)
            target->EndProtectedInsert();
        footnote_dialog_.clear();
        emit RowCommitted();
    });
    dialog->open();
}

void BlockEditor::OpenTableCellEditor(const QString& node_id, int row, int column, const InlineContent& initial) {
    if (cell_dialog_)
        return;
    auto* dialog = new InlineContentEditorDialog(initial, this);
    cell_dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Edit Cell — Row %1, Column %2").arg(row + 1).arg(column + 1));
    dialog->SetPlaceholder(QStringLiteral("Cell content…"));
    dialog->SetCitationResolver(citation_numbers_);
    dialog->SetCrossReferenceLabels(CrossReferenceLabels());
    auto* draft = dialog->Editor();
    draft->setProperty("row_node", node_id);
    draft->SetMathPackages(math_packages_);
    dialog->SetAllowedActions(ConfigureFormatActions(draft));
    QPointer<InlineEditor> caption;
    for (const auto& block : blocks_) {
        if (block.node_id == node_id && block.inline_editor) {
            caption = block.inline_editor;
            caption->BeginProtectedInsert();
            break;
        }
    }
    connect(dialog, &QDialog::finished, this, [this, dialog, caption, node_id, row, column, initial](int result) {
        // 在命令触发的同步 documentChanged 期间仍保护卡片；确认完成后再允许重建。
        if (result == QDialog::Accepted) {
            const InlineContent content = dialog->content();
            if (content != initial)
                emit TableCellEdited(node_id, row, column, content);
        }
        cell_dialog_.clear();
        if (caption)
            caption->EndProtectedInsert();
        emit RowCommitted();
    });
    dialog->open();
}

QWidget* BlockEditor::MakeListCard(const pf::ListBlock& list, const QString& outline_key) {
    const QString list_id = ToQ(list.id.value());
    auto* card = MakeCard(list_id, QStringLiteral("List"), QStringLiteral("list"), true);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());

    auto* tools = new QWidget(card);
    auto* tools_layout = new QHBoxLayout(tools);
    tools_layout->setContentsMargins(0, 0, 0, 0);
    tools_layout->setSpacing(4);
    auto button = [tools, tools_layout](const QString& name, const QString& text) {
        auto* result = new QToolButton(tools);
        result->setObjectName(name);
        result->setText(text);
        result->setFocusPolicy(Qt::NoFocus);
        result->setAutoRaise(true);
        tools_layout->addWidget(result);
        return result;
    };
    auto* style = button(QStringLiteral("listStyleToggle"),
                         list.style == ListStyle::Numbered ? QStringLiteral("1.") : QStringLiteral("•"));
    style->setToolTip(QStringLiteral("Toggle bullet / numbered list"));
    auto* add = button(QStringLiteral("listAddItem"), QStringLiteral("+ Item"));
    tools_layout->addStretch(1);
    card_layout->addWidget(tools);
    connect(style, &QToolButton::clicked, this, [this, list_id, list]() {
        emit ListStyleChangeRequested(list_id, static_cast<int>(list.style == ListStyle::Bullet ? ListStyle::Numbered
                                                                                                 : ListStyle::Bullet));
    });
    connect(add, &QToolButton::clicked, this,
            [this, list_id, count = static_cast<int>(list.items.size())]() { emit ListItemInsertRequested(list_id, count); });

    if (list.items.empty()) {
        auto* hint = new QLabel(QStringLiteral("Empty list — add an item."), card);
        hint->setStyleSheet(QString("color: %1; font-size: 9pt;").arg(theme::kSecondaryText));
        card_layout->addWidget(hint);
    }

    for (size_t index = 0; index < list.items.size(); ++index) {
        const auto& item = list.items[index];
        auto* row = new QWidget(card);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(5);
        auto* marker = new QLabel(list.style == ListStyle::Numbered
                                      ? QStringLiteral("%1.").arg(index + 1)
                                      : QStringLiteral("•"),
                                  row);
        marker->setFixedWidth(22);
        marker->setAlignment(Qt::AlignRight | Qt::AlignTop);
        marker->setStyleSheet(QString("color: %1; padding-top: 3px;").arg(theme::kSecondaryText));
        row_layout->addWidget(marker);

        auto* editor = NewInlineEditor(card, list_id, item.content, outline_key, theme::BlockVisualRole::Body,
                                       QStringLiteral("list_item"));
        card_layout->removeWidget(editor);
        editor->setParent(row);
        row_layout->addWidget(editor, 1);
        editor->SetListItemMode(true);
        editor->setProperty("row_focus_key", ToQ(item.id.value()));
        editor->setProperty("row_list_index", static_cast<int>(index));
        editor->setPlaceholderText(QStringLiteral("List item…"));
        // NewInlineEditor 的默认 Ctrl+Enter 会插入普通文本块；列表项改为插入
        // 下一个列表项。plain Enter 也由 InlineEditor::list_item_mode_ 走同一路径。
        disconnect(editor, &InlineEditor::NewBlockAfter, this, nullptr);
        connect(editor, &InlineEditor::NewBlockAfter, this,
                [this, list_id, index]() { emit ListItemInsertRequested(list_id, static_cast<int>(index + 1)); });

        for (auto& block : blocks_) {
            if (block.inline_editor != editor)
                continue;
            block.list_id = list_id;
            block.list_index = index;
            block.list_item_id = ToQ(item.id.value());
            block.commit_role = QStringLiteral("list_item");
            break;
        }

        auto* remove = new QToolButton(row);
        remove->setObjectName(QStringLiteral("listDeleteItem"));
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(QStringLiteral("Delete this item"));
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setAutoRaise(true);
        row_layout->addWidget(remove);
        connect(remove, &QToolButton::clicked, this,
                [this, list_id, index]() { emit ListItemDeleteRequested(list_id, static_cast<int>(index)); });

        if (index > 0) {
            auto* up = new QToolButton(row);
            up->setObjectName(QStringLiteral("listMoveItemUp"));
            up->setText(QStringLiteral("↑"));
            up->setFocusPolicy(Qt::NoFocus);
            up->setAutoRaise(true);
            row_layout->addWidget(up);
            connect(up, &QToolButton::clicked, this, [this, list_id, index]() {
                emit ListItemMoveRequested(list_id, static_cast<int>(index), static_cast<int>(index - 1));
            });
        }
        if (index + 1 < list.items.size()) {
            auto* down = new QToolButton(row);
            down->setObjectName(QStringLiteral("listMoveItemDown"));
            down->setText(QStringLiteral("↓"));
            down->setFocusPolicy(Qt::NoFocus);
            down->setAutoRaise(true);
            row_layout->addWidget(down);
            connect(down, &QToolButton::clicked, this, [this, list_id, index]() {
                emit ListItemMoveRequested(list_id, static_cast<int>(index), static_cast<int>(index + 1));
            });
        }
        card_layout->addWidget(row);
    }
    return card;
}

QWidget* BlockEditor::MakeTheoremCard(const pf::TheoremBlock& theorem, const QString& outline_key) {
    const QString node_id = ToQ(theorem.id.value());
    auto* card = MakeCard(node_id, QStringLiteral("Theorem"), QStringLiteral("theorem_body"), true);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());

    auto* tools = new QWidget(card);
    auto* tools_layout = new QHBoxLayout(tools);
    tools_layout->setContentsMargins(0, 0, 0, 0);
    tools_layout->setSpacing(6);
    auto* kind = new QComboBox(tools);
    kind->setObjectName(QStringLiteral("theoremKind"));
    const std::pair<const char*, TheoremKind> kinds[] = {
        {"Theorem", TheoremKind::Theorem},       {"Lemma", TheoremKind::Lemma},
        {"Proposition", TheoremKind::Proposition}, {"Corollary", TheoremKind::Corollary},
        {"Definition", TheoremKind::Definition}, {"Proof", TheoremKind::Proof},
        {"Remark", TheoremKind::Remark},         {"Example", TheoremKind::Example},
    };
    for (const auto& entry : kinds)
        kind->addItem(QString::fromUtf8(entry.first), static_cast<int>(entry.second));
    kind->setCurrentIndex(kind->findData(static_cast<int>(theorem.kind)));
    tools_layout->addWidget(kind);
    auto* title = new QLineEdit(ToQ(InlineToPlainText(theorem.title)), tools);
    title->setObjectName(QStringLiteral("theoremTitle"));
    title->setPlaceholderText(QStringLiteral("Optional title"));
    tools_layout->addWidget(title, 1);
    auto* numbered = new QCheckBox(QStringLiteral("Numbered"), tools);
    numbered->setObjectName(QStringLiteral("theoremNumbered"));
    numbered->setChecked(theorem.numbered && theorem.kind != TheoremKind::Proof);
    numbered->setEnabled(theorem.kind != TheoremKind::Proof);
    tools_layout->addWidget(numbered);
    auto* label = new QLineEdit(ToQ(theorem.label), tools);
    label->setObjectName(QStringLiteral("theoremLabel"));
    label->setPlaceholderText(QStringLiteral("thm:main"));
    tools_layout->addWidget(label);
    card_layout->addWidget(tools);

    auto* body = NewInlineEditor(card, node_id, theorem.body, outline_key, theme::BlockVisualRole::Body,
                                 QStringLiteral("theorem_body"));
    body->setPlaceholderText(QStringLiteral("Theorem body…"));
    body->SetMathPackages(math_packages_);

    Block* tracked = nullptr;
    for (auto& block : blocks_) {
        if (block.inline_editor == body) {
            tracked = &block;
            block.theorem_kind = static_cast<int>(theorem.kind);
            block.theorem_title = title->text();
            block.theorem_numbered = numbered->isChecked();
            block.theorem_label = label->text();
            break;
        }
    }
    auto current_kind = [kind]() { return static_cast<TheoremKind>(kind->currentData().toInt()); };
    auto emit_edit = [this, node_id, body, kind, title, numbered, label, tracked, current_kind]() {
        if (tracked) {
            tracked->theorem_kind = static_cast<int>(current_kind());
            tracked->theorem_title = title->text();
            tracked->theorem_numbered = numbered->isChecked();
            tracked->theorem_label = label->text();
        }
        emit TheoremEdited(node_id, static_cast<int>(current_kind()), title->text(), body->Content(),
                           numbered->isChecked(), label->text());
    };
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [numbered, emit_edit, current_kind](int) {
                numbered->setEnabled(current_kind() != TheoremKind::Proof);
                if (current_kind() == TheoremKind::Proof)
                    numbered->setChecked(false);
                emit_edit();
            });
    connect(title, &QLineEdit::editingFinished, this, emit_edit);
    connect(numbered, &QCheckBox::toggled, this, [emit_edit](bool) { emit_edit(); });
    connect(label, &QLineEdit::editingFinished, this, emit_edit);
    return card;
}

QWidget* BlockEditor::MakeCodeCard(const pf::CodeBlock& code_block, const QString& outline_key) {
    const QString node_id = ToQ(code_block.id.value());
    auto* card = MakeCard(node_id, QStringLiteral("Code"), QStringLiteral("code"), true);
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    auto* tools = new QWidget(card);
    auto* tools_layout = new QHBoxLayout(tools);
    tools_layout->setContentsMargins(0, 0, 0, 0);
    tools_layout->setSpacing(6);
    auto* language = new QComboBox(tools);
    language->setObjectName(QStringLiteral("codeLanguage"));
    language->setEditable(true);
    language->addItems({QStringLiteral("python"), QStringLiteral("c++"), QStringLiteral("java"),
                        QStringLiteral("javascript"), QStringLiteral("rust"), QStringLiteral("go"),
                        QStringLiteral("bash"), QStringLiteral("sql"), QStringLiteral("json"),
                        QStringLiteral("yaml"), QStringLiteral("latex")});
    language->setCurrentText(ToQ(code_block.language));
    tools_layout->addWidget(language);
    auto* line_numbers = new QCheckBox(QStringLiteral("Line numbers"), tools);
    line_numbers->setObjectName(QStringLiteral("codeLineNumbers"));
    line_numbers->setChecked(code_block.line_numbers);
    tools_layout->addWidget(line_numbers);
    auto* label = new QLineEdit(ToQ(code_block.label), tools);
    label->setObjectName(QStringLiteral("codeLabel"));
    label->setPlaceholderText(QStringLiteral("lst:main"));
    tools_layout->addWidget(label);
    card_layout->addWidget(tools);

    auto* source = NewEditor(card, ToQ(code_block.code), 8, theme::BlockVisualRole::EquationSource);
    source->setObjectName(QStringLiteral("codeSource"));
    source->setPlaceholderText(QStringLiteral("Code…"));
    source->setProperty("row_node", node_id);
    source->setProperty("row_focus_key", node_id);
    source->setProperty("row_outline_key", outline_key);
    source->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    if (auto* block_edit = qobject_cast<BlockEdit*>(source))
        block_edit->setReflowOnPaste(false);
    auto* caption = new QLineEdit(ToQ(InlineToPlainText(code_block.caption)), card);
    caption->setObjectName(QStringLiteral("codeCaption"));
    caption->setPlaceholderText(QStringLiteral("Optional listing caption"));
    card_layout->addWidget(caption);
    Block* tracked = nullptr;
    for (auto& block : blocks_) {
        if (block.editor == source) {
            tracked = &block;
            block.code_language = ToQ(code_block.language);
            block.code_caption = caption->text();
            block.code_line_numbers = line_numbers->isChecked();
            block.code_label = label->text();
            break;
        }
    }
    auto emit_edit = [this, node_id, source, language, caption, line_numbers, label, tracked]() {
        if (tracked) {
            tracked->code_language = language->currentText();
            tracked->code_caption = caption->text();
            tracked->code_line_numbers = line_numbers->isChecked();
            tracked->code_label = label->text();
        }
        emit CodeEdited(node_id, language->currentText(), source->toPlainText(), caption->text(),
                        line_numbers->isChecked(), label->text());
    };
    connect(language, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [emit_edit](int) { emit_edit(); });
    if (auto* language_edit = language->lineEdit())
        connect(language_edit, &QLineEdit::editingFinished, this, emit_edit);
    connect(line_numbers, &QCheckBox::toggled, this, [emit_edit](bool) { emit_edit(); });
    connect(label, &QLineEdit::editingFinished, this, emit_edit);
    connect(caption, &QLineEdit::editingFinished, this, emit_edit);
    return card;
}

QWidget* BlockEditor::CreateBlockCard(const pf::Block& block, const QString& outline_key) {
    if (const auto* para = std::get_if<pf::Paragraph>(&block)) {
        return MakeTextCard(ToQ(para->id.value()), para->content, outline_key);
    }
    if (const auto* eq = std::get_if<pf::EquationBlock>(&block)) {
        return MakeEquationCard(ToQ(eq->id.value()), *eq, outline_key);
    }
    if (const auto* figure = std::get_if<pf::Figure>(&block)) {
        return MakeFigureCard(*figure, outline_key);
    }
    if (const auto* table = std::get_if<pf::Table>(&block)) {
        return MakeTableCard(*table, outline_key);
    }
    if (const auto* list = std::get_if<pf::ListBlock>(&block)) {
        return MakeListCard(*list, outline_key);
    }
    if (const auto* theorem = std::get_if<pf::TheoremBlock>(&block)) {
        return MakeTheoremCard(*theorem, outline_key);
    }
    if (const auto* code = std::get_if<pf::CodeBlock>(&block)) {
        return MakeCodeCard(*code, outline_key);
    }
    return nullptr;
}

void BlockEditor::AppendBlocks(const std::vector<pf::Block>& blocks, const QString& outline_key) {
    auto* host_layout = qobject_cast<QVBoxLayout*>(host_->layout());
    if (host_layout == nullptr)
        return;
    for (const auto& block : blocks) {
        QWidget* card = CreateBlockCard(block, outline_key);
        if (card == nullptr)
            continue;
        host_layout->insertWidget(host_layout->count() - 1, card);
        // 每个正文块之后都跟一个间隙（包括最后一个），因此指针无需移动到工具栏
        // 就能添加下一个块。
        const QString anchor =
            std::visit([](const auto& typed) { return QString::fromStdString(typed.id.value()); }, block);
        host_layout->insertWidget(host_layout->count() - 1, MakeGap(anchor));
    }
}

QWidget* BlockEditor::MakeCard(const QString& node_id, const QString& kind, const QString& commit_role,
                               bool header_inline) {
    auto* card = new QFrame(host_);
    card->setObjectName("blockCard");
    card->setProperty("row_node", node_id);
    card->setProperty("row_kind", kind);

    auto* card_layout = new QVBoxLayout(card);
    // 纤细外观（UI 方案 §2）：卡片内边距来自主题，并按角色保留留白，
    // 使标题和前置信息保持各自的辨识度。
    card_layout->setContentsMargins(CardMarginsFor(RoleForKind(kind)));
    card_layout->setSpacing(2);

    // 悬停表头（始终预留空间以避免布局跳动，设计 #61）。
    auto* header = new QWidget(card);
    header->setFixedHeight(theme::spacing::kBlockHeaderHeight);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(6);
    auto* handle = new DragHandle(node_id, header);
    // 卡片类型标签展示与 PDF 一致的编号（WP-03）：Figure 3 / Eq. (4) /
    // Section 2.1。未编号的节点（Text、Paragraph）保持原类型名。
    QString kind_display = kind;
    if (numbering_) {
        if (const auto entry = numbering_->Find(NodeId(node_id.toStdString())))
            kind_display = ToQ(entry->display);
    }
    auto* type_label = new QLabel(DisplayLabelFor(kind_display), header);
    type_label->setStyleSheet(QString("color: %1; font-size: 8pt; font-weight: 600;"
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
    auto* more = new QToolButton(header);
    more->setText("⋯");
    more->setAutoRaise(true);
    more->setFixedSize(22, theme::spacing::kBlockHeaderHeight);
    more->setStyleSheet(QString("QToolButton { color: %1; border: none; }"
                                "QToolButton:hover { background: %2; border-radius: 4px; }")
                            .arg(theme::kSecondaryText, theme::kAccentSoft));
    const bool can_restructure = !node_id.isEmpty();
    const bool can_reflow = IsProseRole(commit_role);
    connect(more, &QToolButton::clicked, this, [this, more, card, node_id, can_restructure, can_reflow]() {
        if (!can_restructure && !can_reflow)
            return;
        QMenu menu(this);
        QMenu* insert_menu = nullptr;
        QAction* up = nullptr;
        QAction* down = nullptr;
        QAction* del = nullptr;
        QAction* reflow = nullptr;
        if (can_restructure) {
            insert_menu = menu.addMenu("Insert Block Below");
            for (const auto& entry : InsertEntries(false)) {
                QAction* action = insert_menu->addAction(QString::fromUtf8(entry.label));
                action->setData(QString::fromUtf8(entry.kind));
            }
        }
        if (can_reflow) {
            reflow = menu.addAction("Reflow Text");
            reflow->setToolTip("Undo the hard line breaks of a pasted paragraph so it "
                               "wraps to the block width");
        }
        if (can_restructure) {
            menu.addSeparator();
            up = menu.addAction("Move Up");
            down = menu.addAction("Move Down");
            menu.addSeparator();
            del = menu.addAction("Delete");
        }
        QAction* chosen_action = menu.exec(more->mapToGlobal(QPoint(0, more->height())));
        if (chosen_action == nullptr)
            return;
        if (insert_menu && chosen_action->parent() == insert_menu) {
            emit InsertBlockRequested(chosen_action->data().toString(), node_id);
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
    card->setProperty("header", QVariant::fromValue(static_cast<QWidget*>(header)));

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
void BlockEditor::UpdateCardState(QWidget* card) {
    if (!card)
        return;
    const bool missing = card->property("card_missing").toBool();
    const bool hover = card->property("card_hover").toBool();
    const bool focus = card->property("card_focus").toBool();
    const bool flash = card->property("card_flash").toBool();

    const char* line = "transparent";
    const char* background = "transparent";
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
    auto* header = card->property("header").value<QWidget*>();
    if (!header)
        return;
    const bool structural = !card->property("row_node").toString().isEmpty();
    for (QObject* child : header->children()) {
        if (auto* grip = qobject_cast<DragHandle*>(child)) {
            grip->setVisible(chrome && structural);
        } else if (auto* button = qobject_cast<QToolButton*>(child)) {
            button->setVisible(chrome && structural);
        } else if (auto* label = qobject_cast<QLabel*>(child)) {
            if (label->property("hover_only").toBool()) {
                label->setVisible(chrome);
            }
        }
    }
}

void BlockEditor::AddEditorToCard(QWidget* card, QPlainTextEdit* edit) {
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    card_layout->addWidget(edit);
}

QPlainTextEdit* BlockEditor::NewEditor(QWidget* card, const QString& text, int min_lines, theme::BlockVisualRole role,
                                       bool single_line) {
    auto* edit = new BlockEdit(single_line, card);
    edit->asset_paste_handler_ = [this, edit](const QMimeData* mime) {
        if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
            return false;
        return HandleAssetMime(mime, AssetAnchorAt(edit, edit->rect().center()));
    };
    // 先设置字体环境，再设置文本（UI 方案 §6）：度量、行高和占位符最小高度
    // 都源自角色的字体。
    edit->SetTypography(theme::EditorFont(role), theme::LineHeightFor(role));
    edit->SetInitialText(text);
    edit->setStyleSheet(
        QString("QPlainTextEdit { background: transparent; border: none; color: %1;"
                " padding: 0; selection-background-color: %2; }")
            .arg(theme::IsSecondaryRole(role) ? theme::kSecondaryText : theme::kPrimaryText, theme::kAccentSoft));
    // 占位符的最小高度（abstract 要求三行），按角色的行高缩放，
    // 使空状态与已加载状态一致。
    const int line_height = theme::LineHeightFor(role);
    const qreal line_scale = line_height > 0 ? line_height / 100.0 : 1.0;
    const int min_h = static_cast<int>(edit->fontMetrics().height() * min_lines * line_scale) + 12;
    if (auto* block_edit = qobject_cast<BlockEdit*>(edit)) {
        block_edit->SetMinimumContentHeight(min_h);
    }
    edit->installEventFilter(this);
    AddEditorToCard(card, edit);
    return edit;
}

void BlockEditor::ReflowRow(QWidget* card, const QString& node_id) {
    if (!card)
        return;
    if (auto* rich = card->findChild<InlineEditor*>()) {
        InlineContent content = rich->Content();
        for (auto& node : content) {
            if (auto* text = std::get_if<TextRun>(&node))
                text->text = pf::ReflowHardWrappedText(text->text);
        }
        if (content != rich->Content()) {
            const int caret = rich->textCursor().position();
            rich->SetContent(content);
            QTextCursor cursor(rich->document());
            cursor.setPosition(qMin(caret, rich->document()->characterCount() - 1));
            rich->setTextCursor(cursor);
            CommitInlineRow(rich);
        }
        RevealNode(node_id);
        return;
    }
    QPlainTextEdit* editor = card->findChild<QPlainTextEdit*>();
    if (!editor)
        return;
    Block* target = nullptr;
    for (auto& block : blocks_) {
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
    auto* block_edit = qobject_cast<BlockEdit*>(editor);
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

void BlockEditor::CommitBlock(Block& block) {
    if (rebuilding_)
        return;
    // 正文和题注共享富内容提交；commit_role 决定语义目标，绝不压平 mark/token。
    if (block.inline_editor) {
        const InlineContent content = block.inline_editor->Content();
        if (content == block.committed_content) {
            block.inline_editor->MarkClean();
            emit RowCommitted();
            return;
        }
        block.committed_content = content;
        block.committed_text = ToQ(pf::InlineToPlainText(content));
        block.inline_editor->MarkClean();
        if (block.commit_role == QLatin1String("caption")) {
            emit CaptionContentEdited(block.node_id, content);
        } else if (block.commit_role == QLatin1String("theorem_body")) {
            emit TheoremEdited(block.node_id, block.theorem_kind, block.theorem_title, content,
                               block.theorem_numbered, block.theorem_label);
        } else if (block.commit_role == QLatin1String("list_item")) {
            emit ListItemContentEdited(block.list_id, static_cast<int>(block.list_index), content);
        } else {
            emit ParagraphContentEdited(block.node_id, content);
        }
        emit RowCommitted();
        return;
    }
    if (!block.editor)
        return;
    auto* edit = qobject_cast<BlockEdit*>(block.editor);
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
    const QString& role = block.commit_role;
    if (role == "code") {
        emit CodeEdited(block.node_id, block.code_language, text, block.code_caption,
                        block.code_line_numbers, block.code_label);
        emit RowCommitted();
        return;
    }
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
        emit EquationEdited(block.node_id, text, block.equation_numbered, block.equation_label);
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

bool BlockEditor::eventFilter(QObject* watched, QEvent* event) {
    if (shutting_down_)
        return false;
    const QEvent::Type type = event->type();
    // Application-level filter：编辑中的公式卡片只在实际点击卡片外部时收起。
    if (type == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget)
            return false;
        QWidget* popup = QApplication::activePopupWidget();
        const bool in_popup = popup && (widget == popup || popup->isAncestorOf(widget));
        if (!in_popup) {
            for (auto& block : blocks_) {
                if (!block.set_equation_editing || !block.equation_editing || !block.card)
                    continue;
                if (widget == block.card || block.card->isAncestorOf(widget))
                    continue;
                SetEquationEditing(block.card, false);
            }
        }
    }
    // 窗格尺寸变化时（拖动分隔条、专注模式、窗口缩放）保持阅读列居中。
    if (scroll_ && watched == scroll_->viewport() && type == QEvent::Resize) {
        CenterContentColumn();
        return false;
    }
    // 块卡片的悬停外观（设计 #4）。样式本身在 UpdateCardState 中组合；
    // 过滤器只记录状态。
    if (type == QEvent::Enter || type == QEvent::Leave) {
        auto* card = qobject_cast<QFrame*>(watched);
        if (card && card->objectName() == "blockCard") {
            card->setProperty("card_hover", type == QEvent::Enter);
            UpdateCardState(card);
        }
        return false;
    }
    // 公式处于展示态时，点击预览/卡片进入编辑态并把焦点交给源码框。
    if (type == QEvent::MouseButtonPress) {
        QFrame* card = CardOf(watched);
        if (card && card->property("equation_card").toBool() && !IsEquationEditing(card)) {
            SetEquationEditing(card, true);
            for (auto& block : blocks_) {
                if (block.card == card && block.editor) {
                    block.editor->setFocus(Qt::MouseFocusReason);
                    break;
                }
            }
            return true;
        }
    }
    // 行聚焦：卡片上的强调色线，以及反向大纲同步（UI 方案 §9、§10）。
    // 每个行编辑器（BlockEdit 和 InlineEditor）都会被过滤。
    if (type == QEvent::FocusIn || type == QEvent::FocusOut) {
        if (auto* card = CardOf(watched)) {
            card->setProperty("card_focus", type == QEvent::FocusIn);
            if (type == QEvent::FocusIn && card->property("equation_card").toBool())
                SetEquationEditing(card, true);
            UpdateCardState(card);
            if (type == QEvent::FocusIn) {
                const auto* widget = qobject_cast<const QWidget*>(watched);
                emit FocusOutlineChanged(widget ? widget->property("row_outline_key").toString() : QString());
            }
        }
        return false;
    }

    // 来自各编辑器的按键接线。
    auto* edit = qobject_cast<BlockEdit*>(watched);
    if (edit && type == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_F2) {
            OpenSlashMenu(edit);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BlockEditor::BuildAuthorBindingPanel(QWidget* card, const FrontMatter& front) {
    if (card == nullptr)
        return;
    auto* card_layout = qobject_cast<QVBoxLayout*>(card->layout());
    if (card_layout == nullptr)
        return;

    auto* panel = new QWidget(card);
    panel->setObjectName("authorBindingPanel");
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(2, 0, 2, 2);
    panel_layout->setSpacing(1);

    auto* caption = new QLabel("Institution links", panel);
    caption->setStyleSheet(QString("color: %1; font-size: 8pt;"
                                   " font-weight: 700; letter-spacing: 0.5px;")
                               .arg(theme::kDisabledText));
    panel_layout->addWidget(caption);

    if (front.authors.empty()) {
        auto* hint = new QLabel("Add an author above to link institutions.", panel);
        hint->setStyleSheet(QString("color: %1; font-size: 9pt;").arg(theme::kDisabledText));
        panel_layout->addWidget(hint);
    } else if (front.affiliations.empty()) {
        auto* hint = new QLabel("Add an institution above, then link each author to it here.", panel);
        hint->setStyleSheet(QString("color: %1; font-size: 9pt;").arg(theme::kDisabledText));
        panel_layout->addWidget(hint);
    } else {
        for (size_t i = 0; i < front.authors.size(); ++i) {
            const auto& author = front.authors[i];
            auto* row = new QWidget(panel);
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            row_layout->setSpacing(6);

            auto* name = new QLabel(ToQ(author.name), row);
            name->setMinimumWidth(120);
            name->setStyleSheet(QString("color: %1; font-size: 9pt;").arg(theme::kPrimaryText));
            row_layout->addWidget(name);

            // 按钮显示当前的编号，因此无需打开任何东西就能看到绑定关系。
            QStringList current;
            for (size_t a = 0; a < front.affiliations.size(); ++a) {
                for (const auto& link : author.affiliations) {
                    if (link == front.affiliations[a].id) {
                        static const char* kSupers[] = {"\u00b9", "\u00b2", "\u00b3", "\u2074", "\u2075",
                                                        "\u2076", "\u2077", "\u2078", "\u2079"};
                        current << (a < 9 ? QString::fromUtf8(kSupers[a]) : QString::number(a + 1));
                    }
                }
            }
            auto* pick = new QToolButton(row);
            pick->setObjectName("authorAffiliationPicker");
            pick->setText(current.isEmpty() ? QStringLiteral("link…") : current.join(QLatin1Char(' ')));
            pick->setToolTip("Which institutions does this author belong to?");
            pick->setCursor(Qt::PointingHandCursor);
            pick->setFocusPolicy(Qt::NoFocus);
            pick->setStyleSheet(QString("QToolButton { color: %1; background: %2;"
                                        " border: 1px solid %3; border-radius: 9px;"
                                        " padding: 1px 8px; font-size: 9pt; }"
                                        "QToolButton:hover { border-color: %4; }")
                                    .arg(theme::kSecondaryText, theme::kSidePanel, theme::kDivider, theme::kAccent));
            row_layout->addWidget(pick);
            row_layout->addStretch(1);
            panel_layout->addWidget(row);

            const int author_index = static_cast<int>(i);
            connect(pick, &QToolButton::clicked, this, [this, pick, front, author_index]() {
                QMenu menu(this);
                for (size_t a = 0; a < front.affiliations.size(); ++a) {
                    const auto& affiliation = front.affiliations[a];
                    const bool linked = std::any_of(front.authors[author_index].affiliations.begin(),
                                                    front.authors[author_index].affiliations.end(),
                                                    [&](const AffiliationId& id) { return id == affiliation.id; });
                    QAction* action = menu.addAction(QStringLiteral("%1  %2").arg(a + 1).arg(ToQ(affiliation.name)));
                    action->setCheckable(true);
                    action->setChecked(linked);
                    const QString id = ToQ(affiliation.id.value());
                    connect(action, &QAction::triggered, this, [this, author_index, id](bool checked) {
                        emit AuthorAffiliationToggled(author_index, id, checked);
                    });
                }
                menu.exec(pick->mapToGlobal(QPoint(0, pick->height())));
            });
        }
    }
    card_layout->addWidget(panel);
}

QWidget* BlockEditor::MakeGap(const QString& anchor) {
    auto* gap = new BlockGap([this, anchor](QWidget* source) { ShowInsertMenu(anchor, source); },
                             [this, anchor](const QString& node) {
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

void BlockEditor::ShowInsertMenu(const QString& anchor, QWidget* source) {
    const bool body_empty = anchor.isEmpty();
    QMenu menu(this);
    if (body_empty) {
        QAction* hint = menu.addAction("The body is empty — start with a section");
        hint->setEnabled(false);
        menu.addSeparator();
    }
    const NodeKind container = ContainerKindFor(*container_document_, anchor);
    const int max_depth = max_heading_depth_ > 0 ? max_heading_depth_ : 3;
    const char* current_group = nullptr;
    for (const auto& entry : InsertOptionsFor(body_empty, container, max_depth)) {
        const std::string kind = entry.kind;
        if ((kind == "theorem" || kind == "proof") && !theorems_enabled_)
            continue;
        if (kind == "code" && !code_blocks_enabled_)
            continue;
        if (current_group == nullptr || std::strcmp(current_group, entry.group) != 0) {
            if (current_group != nullptr)
                menu.addSeparator();
            QAction* group = menu.addAction(QString::fromUtf8(entry.group));
            group->setEnabled(false);
            current_group = entry.group;
        }
        QAction* action = menu.addAction(QString::fromUtf8(entry.label));
        action->setData(QString::fromUtf8(entry.kind));
    }
    QAction* chosen = menu.exec(source->mapToGlobal(QPoint(source->width() / 2, source->height())));
    if (chosen == nullptr || !chosen->data().isValid())
        return;
    emit InsertBlockRequested(chosen->data().toString(), anchor);
}

void BlockEditor::OpenSlashMenu(QPlainTextEdit* origin) {
    // 规则与间隙菜单相同：按该行所属的容器以及模板的标题深度进行过滤。
    QString row_node;
    for (const auto& block : blocks_) {
        if (block.editor == origin) {
            row_node = block.node_id;
            break;
        }
    }
    const NodeKind container = ContainerKindFor(*container_document_, row_node);
    const int max_depth = max_heading_depth_ > 0 ? max_heading_depth_ : 3;

    std::vector<PopupList::Item> items;
    for (const auto& entry : InsertOptionsFor(row_node.isEmpty(), container, max_depth)) {
        const std::string kind = entry.kind;
        if ((kind == "theorem" || kind == "proof") && !theorems_enabled_)
            continue;
        if (kind == "code" && !code_blocks_enabled_)
            continue;
        PopupList::Item item;
        item.label = QString::fromUtf8(entry.label);
        item.detail = "Insert";
        item.group = QString::fromUtf8(entry.group);
        item.payload = QString::fromUtf8(entry.kind);
        item.search = item.label.toLower();
        items.push_back(std::move(item));
    }

    auto* popup = new PopupList(this);
    connect(popup, &PopupList::chosen, this,
            [this, row_node](QString payload) { emit InsertBlockRequested(payload, row_node); });
    QPoint anchor = origin->mapToGlobal(QPoint(60, origin->height() + 4));
    popup->popup(anchor, items);
}

void BlockEditor::RebuildFromDocument(const Document& doc) {
    // 在下一次重建之前，插入菜单都以本文档解析锚点；MainWindow 会让 session
    // 存活至编辑器的整个生命周期，而重建始终传入当前文档。
    container_document_ = &doc;
    // 保存焦点，以及用户仍在编辑的行的当前文本。该文本优先于文档：从其他任何
    // 地方触发的重建都绝不能丢弃当前正在输入的内容。
    focus_node_.clear();
    focus_pos_ = 0;
    QString table_focus_node;
    int table_row = -1, table_column = -1;
    if (auto* card = CardOf(focusWidget()); card && card->property("row_kind").toString() == QLatin1String("Table")) {
        if (auto* grid = card->findChild<QTableWidget*>()) {
            table_focus_node = card->property("row_node").toString();
            table_row = grid->currentRow();
            table_column = grid->currentColumn();
        }
    }
    QString pending_key;
    QString pending_text;
    bool has_pending = false;
    // 富文本行在确实需要重建时同样保留其光标。
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        focus_node_ = rich->property("row_focus_key").toString();
        focus_pos_ = rich->textCursor().position();
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        focus_node_ = edit->property("row_focus_key").toString();
        focus_pos_ = edit->textCursor().position();
        if (edit->IsDirty()) {
            pending_key = focus_node_;
            pending_text = edit->toPlainText();
            has_pending = true;
        }
    }

    rebuilding_ = true;
    auto* host_layout = qobject_cast<QVBoxLayout*>(host_->layout());
    // 清空上一轮的所有控件，只保留末尾的 stretch。行和间隙都在每次重建时创建，
    // 若有残留控件存活，它会持续从覆盖其上的行那里抢走悬停和点击。
    for (int i = host_layout->count() - 1; i >= 0; --i) {
        QLayoutItem* item = host_layout->itemAt(i);
        if (item == nullptr || item->widget() == nullptr)
            continue;
        host_layout->takeAt(i);
        QWidget* widget = item->widget();
        widget->hide();
        widget->deleteLater();
        delete item;
    }
    blocks_.clear();
    UpdateEquationEventFilter();
    // 每个正文块之后都跟一个间隙（包括最后一个），因此指针无需移动到工具栏
    // 就能添加下一个块。
    auto append_gap = [&](const QString& anchor) {
        host_layout->insertWidget(host_layout->count() - 1, MakeGap(anchor));
    };
    auto add = [&](const QString& node_id, const QString& kind, const QString& commit_role, const QString& text,
                   const QString& outline_key = QString(), bool header_inline = true, int min_lines = 1,
                   bool single_line = false) -> QWidget* {
        QWidget* card = MakeCard(node_id, kind, commit_role, header_inline);
        const QString focus_key = node_id.isEmpty() ? QStringLiteral("front:") + commit_role : node_id;
        const bool restore_pending = has_pending && focus_key == pending_key;
        QString row_text = restore_pending ? pending_text : text;
        if (IsProseRole(commit_role) && !restore_pending) {
            // 用户正在该行输入时绝不重新排布。
            row_text = ToQ(pf::ReflowHardWrappedText(ToStd(row_text)));
        }
        // 角色决定字体 + 行高（UI 方案 §3、§5）：标题、作者、摘要、节/小节/小小节
        // 标题以及正文，都按统一 GUI 排版表的字号和字重呈现。
        QPlainTextEdit* edit = NewEditor(card, row_text, min_lines, RoleForKind(kind), single_line);
        if (auto* block_edit = qobject_cast<BlockEdit*>(edit)) {
            if (restore_pending) {
                // 模型文本作为基线，因此用户的文本仍会在失焦时提交。
                block_edit->BeginProgrammaticEdit();
                block_edit->EndProgrammaticEdit();
                block_edit->MarkClean();
            }
        }
        if (auto* block_edit = qobject_cast<BlockEdit*>(edit)) {
            block_edit->setReflowOnPaste(IsProseRole(commit_role));
            block_edit->MarkClean();
        }
        edit->setProperty("row_node", node_id);
        edit->setProperty("row_focus_key", focus_key);
        edit->setProperty("row_outline_key", outline_key);
        edit->setProperty("commands_enabled", !node_id.isEmpty());
        auto center_text = [edit]() {
            auto* block_edit = qobject_cast<BlockEdit*>(edit);
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
        qobject_cast<BlockEdit*>(edit)->Resize();
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
            if (auto* restored = qobject_cast<BlockEdit*>(edit)) {
                restored->MarkDirty();
            }
        }
        // 在 Enter 时提交（commit 信号）——BlockEdit 会在 Enter 和 focusOut 时
        // 都发出 CommitRequested；我们需要的是失焦提交，Enter 只是继续移动。
        // 接线如下：
        BlockEdit* block_edit = qobject_cast<BlockEdit*>(edit);
        connect(block_edit, &BlockEdit::CommitRequested, this, [this, edit]() {
            for (auto& b : blocks_) {
                if (b.editor == edit) {
                    CommitBlock(b);
                    break;
                }
            }
        });
        connect(block_edit, &BlockEdit::TriggerSlash, this, [this, edit]() { OpenSlashMenu(edit); });
        connect(block_edit, &BlockEdit::NewBlockAfter, this,
                [this, node_id]() { emit InsertBlockRequested("paragraph", node_id); });
        host_layout->insertWidget(host_layout->count() - 1, card);
        if (!node_id.isEmpty())
            append_gap(node_id);
        blocks_.push_back(std::move(block));
        return card;
    };

    // 前置信息。其行不是大纲节点，因此传入空的大纲键（Abstract 是例外：
    // 它可通过 "front:abstract" 从大纲访问）。
    const auto& fm = doc.front_matter();
    add("", "Title", "title", ToQ(pf::InlineToPlainText(fm.title)), QString(), true, 1, true);
    QString authors;
    for (size_t i = 0; i < fm.authors.size(); ++i) {
        if (i)
            authors += " · ";
        authors += ToQ(fm.authors[i].name);
        // 机构上标
        for (const auto& aff_id : fm.authors[i].affiliations) {
            for (size_t a = 0; a < fm.affiliations.size(); ++a) {
                if (fm.affiliations[a].id == aff_id) {
                    const char* supers[] = {"¹", "²", "³", "⁴", "⁵"};
                    authors += supers[a < 5 ? a : 4];
                    break;
                }
            }
        }
    }
    {
        QWidget* authors_card = add("", "Authors", "authors", authors, QString(), true, 1, true);
        BuildAuthorBindingPanel(authors_card, fm);
    }
    QString affiliations;
    for (size_t i = 0; i < fm.affiliations.size(); ++i) {
        if (i)
            affiliations += "; ";
        const char* supers[] = {"¹ ", "² ", "³ ", "⁴ ", "⁵ "};
        affiliations += supers[i < 5 ? i : 4] + ToQ(fm.affiliations[i].name);
    }
    add("", "Institution", "affiliations", affiliations, QString(), true, 1, true);
    add("", "Abstract", "abstract", fm.abstract_text ? ToQ(pf::InlineToPlainText(*fm.abstract_text)) : QString(),
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
    for (const auto& section : doc.body().sections) {
        const QString section_key = ToQ(section.id.value());
        add(section_key, "Section Title", "section", ToQ(pf::InlineToPlainText(section.title)), section_key, true, 1,
            true);
        // P0-05：每个标题层级的所有块类型都走这一条追加路径。
        AppendBlocks(section.blocks, section_key);
        for (const auto& sub : section.subsections) {
            const QString sub_key = ToQ(sub.id.value());
            add(sub_key, "Subsection Title", "subsection", ToQ(pf::InlineToPlainText(sub.title)), sub_key, true, 1,
                true);
            AppendBlocks(sub.blocks, sub_key);
            for (const auto& subsub : sub.subsubsections) {
                const QString subsub_key = ToQ(subsub.id.value());
                add(subsub_key, "Subsubsection Title", "subsubsection", ToQ(pf::InlineToPlainText(subsub.title)),
                    subsub_key, true, 1, true);
                AppendBlocks(subsub.blocks, subsub_key);
            }
        }
    }

    ApplyHints();

    // 行列操作重建网格后保留当前单元格；题注仍使用下面的光标恢复路径。
    if (!table_focus_node.isEmpty()) {
        for (const auto& block : blocks_) {
            if (block.node_id != table_focus_node || !block.card)
                continue;
            if (auto* grid = block.card->findChild<QTableWidget*>(); grid && grid->rowCount() && grid->columnCount()) {
                grid->setCurrentCell(qBound(0, table_row, grid->rowCount() - 1),
                                     qBound(0, table_column, grid->columnCount() - 1));
                if (focus_node_.isEmpty())
                    grid->setFocus(Qt::OtherFocusReason);
            }
            break;
        }
    }

    // 恢复焦点。
    if (!focus_node_.isEmpty()) {
        for (const auto& block : blocks_) {
            // Text 行：富文本编辑器，光标钳制在其文档内。
            if (block.inline_editor && block.inline_editor->property("row_focus_key").toString() == focus_node_) {
                auto cursor = block.inline_editor->textCursor();
                cursor.setPosition(qMin(focus_pos_, block.inline_editor->document()->characterCount() - 1));
                block.inline_editor->setTextCursor(cursor);
                block.inline_editor->setFocus();
                break;
            }
            if (block.editor && block.editor->property("row_focus_key").toString() == focus_node_) {
                auto cursor = block.editor->textCursor();
                cursor.setPosition(qMin(focus_pos_, block.editor->toPlainText().length()));
                block.editor->setTextCursor(cursor);
                block.editor->setFocus();
                break;
            }
        }
    }
    rebuilding_ = false;
}

void BlockEditor::SetRequiredHints(const RequiredHints& hints) {
    hints_ = hints;
    ApplyHints();
}

void BlockEditor::ApplyHints() {
    for (auto& block : blocks_) {
        bool missing = false;
        if (block.kind == "Title") {
            missing = hints_.title && block.editor && block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Authors") {
            missing = hints_.authors && block.editor && block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Institution") {
            missing = hints_.affiliations && block.editor && block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Abstract") {
            missing = hints_.abstract_text && block.editor && block.editor->toPlainText().trimmed().isEmpty();
        } else if (block.kind == "Keywords") {
            missing = hints_.keywords && block.editor && block.editor->toPlainText().trimmed().isEmpty();
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
    for (auto& block : blocks_) {
        if (block.inline_editor) {
            block.inline_editor->SetCrossReferenceLabels(labels);
        }
    }
}

void BlockEditor::SetAssetPathResolver(std::function<QString(const AssetId&)> resolver) {
    asset_path_resolver_ = std::move(resolver);
}

void BlockEditor::SetFigurePreviewResolver(FigurePreviewResolver resolver) {
    figure_preview_resolver_ = std::move(resolver);
}

void BlockEditor::RefreshFigurePreviews(const QString& path) {
    if (!figure_preview_resolver_)
        return;
    for (QLabel* label : findChildren<QLabel*>(QStringLiteral("figurePreview"))) {
        const AssetId id(label->property("figure_asset").toString().toStdString());
        if (!path.isEmpty() && asset_path_resolver_ && asset_path_resolver_(id) != path)
            continue;
        auto* image = static_cast<FigureImageLabel*>(label);
        image->SetSourcePixmap(figure_preview_resolver_(id, QSize(1200, 1200)));
    }
}

QString BlockEditor::AssetAnchorAt(QWidget* target, const QPoint& position) const {
    const QPoint point = host_->mapFromGlobal(target->mapToGlobal(position));
    QWidget* hit = host_->childAt(point);
    for (QWidget* widget = hit ? hit : target; widget && widget != this; widget = widget->parentWidget()) {
        if (widget->property("gap_anchor").isValid())
            return widget->property("gap_anchor").toString();
        const QString node = widget->property("row_node").toString();
        if (!node.isEmpty())
            return node;
    }
    QString previous;
    for (const auto& block : blocks_) {
        if (!block.card || block.node_id.isEmpty())
            continue;
        if (block.card->mapTo(host_, QPoint()).y() > point.y())
            break;
        previous = block.node_id;
    }
    if (previous.isEmpty() && container_document_ && !container_document_->body().sections.empty())
        previous = ToQ(container_document_->body().sections.front().id.value());
    return previous;
}

bool BlockEditor::HandleAssetMime(const QMimeData* mime, const QString& anchor) {
    if (!HasAssetMime(mime))
        return false;
    if (anchor.isEmpty()) {
        emit AssetInputFailed(QStringLiteral("Add a section before inserting a figure."));
        return true;
    }
    const QStringList files = AssetFiles(mime);
    if (!files.isEmpty()) {
        // Inserting repeatedly after the same anchor reverses the order.
        // Queue the imports so the widget receiving the event can finish first.
        QTimer::singleShot(0, this, [this, files, anchor]() {
            for (auto path = files.crbegin(); path != files.crend(); ++path)
                emit AssetFileDropped(*path, anchor);
        });
        return true;
    }
    QImage image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull())
        image = qvariant_cast<QPixmap>(mime->imageData()).toImage();
    auto file = std::make_shared<QTemporaryFile>(QDir::tempPath() + "/paperforge-paste-XXXXXX.png");
    if (image.isNull() || !file->open()) {
        emit AssetInputFailed(QStringLiteral("Could not read the clipboard image or create its temporary file."));
        return true;
    }
    file->close();
    if (!image.save(file->fileName(), "PNG")) {
        emit AssetInputFailed(QStringLiteral("Could not save the clipboard image as PNG."));
        return true;
    }
    QTimer::singleShot(0, this, [this, file, anchor]() { emit AssetPasted(file->fileName(), anchor); });
    return true;
}

bool BlockEditor::HasUncommittedFocus() const {
    if (cell_dialog_ || footnote_dialog_)
        return true;
    // 模态公式编辑器会暂时拥有焦点，但它所在的行仍包含待处理的编辑，
    // 必须能在文档通知中存活。打开了引用/交叉引用选择器的行也是如此：
    // 选择器会插入到*那个*控件中并自行提交它（引用方案 §4）。
    for (const auto& block : blocks_) {
        if (block.inline_editor &&
            (block.inline_editor->IsMathEditorOpen() || block.inline_editor->IsProtectedInsertOpen())) {
            return true;
        }
    }
    // 只要焦点还落在正在编辑的公式卡片内（源码/Palette/属性控件），就不重建：
    // 否则点击 Palette 或切换 Numbered 会立刻把卡片打回展示态。
    if (QWidget* focused = focusWidget()) {
        for (const auto& block : blocks_) {
            if (block.equation_editing && block.card && block.card->isAncestorOf(focused))
                return true;
        }
    }
    // 公式行处于编辑态时，源码可能因焦点移到 Palette/属性控件而仍然脏；
    // 此时重建会丢失输入，即使焦点已经不在源码框上。
    for (const auto& block : blocks_) {
        if (!block.equation_editing || !block.editor)
            continue;
        if (auto* edit = qobject_cast<BlockEdit*>(block.editor); edit && edit->IsDirty())
            return true;
    }
    // Text 行（InlineEditor）与普通行一样带有未提交的输入；在任一方为脏时重建
    // 都会销毁正在输入的内容——对富文本行而言，还会销毁格式状态。
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        return rich->IsDirty();
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        return edit->IsDirty();
    }
    // 图表 label 输入框（WP-03）。textChanged 只设置 modified，不写文档；
    // 在失焦/回车前重建行会丢失用户正在输入的 label。
    if (auto* line = qobject_cast<QLineEdit*>(focusWidget()))
        return line->isModified();
    return false;
}

void BlockEditor::RefreshHints() {
    ApplyHints();
}

void BlockEditor::CommitFocused() {
    // 属性输入框（公式 Label、图表 Label）先走自己的 editingFinished 提交，
    // 再继续提交同一张公式卡片里的源码。
    if (auto* line = qobject_cast<QLineEdit*>(focusWidget()))
        QMetaObject::invokeMethod(line, "editingFinished", Qt::DirectConnection);
    if (QWidget* focused = focusWidget()) {
        for (auto& block : blocks_) {
            if (block.equation_editing && block.card && block.card->isAncestorOf(focused) && block.editor) {
                CommitBlock(block);
                return;
            }
        }
    }
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        for (auto& block : blocks_) {
            if (block.inline_editor == rich) {
                CommitBlock(block);
                return;
            }
        }
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        for (auto& block : blocks_) {
            if (block.editor == edit) {
                CommitBlock(block);
                return;
            }
        }
    }
    // 卡片可能刚刚因外部点击收起，源码提交还在 queued 队列中；Build/Save
    // 前必须同步冲刷，否则 snapshot 会落后于用户最后输入的内容。
    for (auto& block : blocks_) {
        if (block.kind != QLatin1String("Equation") || !block.editor)
            continue;
        auto* edit = qobject_cast<BlockEdit*>(block.editor);
        if (edit && edit->IsDirty()) {
            CommitBlock(block);
            return;
        }
    }
}

std::optional<QString> BlockEditor::FocusedNodeId() const {
    if (auto* rich = qobject_cast<InlineEditor*>(focusWidget())) {
        QString node = rich->property("row_node").toString();
        if (!node.isEmpty())
            return node;
    }
    if (auto* edit = qobject_cast<BlockEdit*>(focusWidget())) {
        QString node = edit->property("row_node").toString();
        if (!node.isEmpty())
            return node;
    }
    if (auto* line = qobject_cast<QLineEdit*>(focusWidget())) {
        QString node = line->property("row_node").toString();
        if (!node.isEmpty())
            return node;
    }
    if (auto* card = CardOf(focusWidget())) {
        const QString node = card->property("row_node").toString();
        if (!node.isEmpty())
            return node;
    }
    return std::nullopt;
}

void BlockEditor::RevealNode(const QString& node_id) {
    for (const auto& block : blocks_) {
        // 前置信息行（Abstract、Title）通过其行键寻址，
        // 真实块则通过其 node id 寻址。
        const bool matches =
            block.node_id == node_id || (block.editor && block.editor->property("row_focus_key").toString() == node_id);
        if (matches && block.card) {
            // 问题导航会落到这里（Build Diagnostics 方案 §26，UI 方案 §9）：
            // 滚动、短暂强调色闪烁，并将焦点移入该行。闪烁是一个属性，而非手写的
            // 样式表，因此它会与聚焦/悬停状态组合，而不是覆盖它们。
            scroll_->ensureWidgetVisible(block.card, 0, 80);
            QWidget* revealed = block.card;
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
