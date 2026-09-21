#pragma once
// PaperForge GUI 主题（设计文档 2、64、66 节）：仅浅色主题，
// 编辑器是视觉中心，侧边面板略带灰色。
//
// UI 排版方案（2024 UI 调整 §12）：编辑器使用的每个间距／字体数值
// 都位于下面的 theme 命名空间中。BlockEditor.cpp 不得再新增
// 临时的 UiFont(11) / setFixedHeight(18) 字面量；在这里改一个 token，
// 整个 GUI 随之改变。
//
// GUI 布局与 PDF 布局解耦：这些尺寸只服务于阅读与编辑；
// 最终 PDF 仍由 Template + LaTeX 控制。

#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

namespace pf::gui::theme {

// 调色板（设计 #2）
constexpr const char* kMainBackground = "#F3F4F6";
constexpr const char* kEditorBackground = "#FFFFFF";
constexpr const char* kSidePanel = "#F8F9FB";
constexpr const char* kDivider = "#E4E7EC";
constexpr const char* kPrimaryText = "#202124";
constexpr const char* kSecondaryText = "#667085";
constexpr const char* kDisabledText = "#98A2B3";
constexpr const char* kAccent = "#2A5DB0";         // 焦点线／链接
constexpr const char* kAccentSoft = "#EAF1FB";     // 悬停背景
constexpr const char* kError = "#D64545";
constexpr const char* kErrorSoft = "#FDF0F0";
constexpr const char* kWarning = "#B7791F";
constexpr const char* kWarningSoft = "#FBF5E9";
constexpr const char* kOk = "#2F855A";
constexpr const char* kBlockHover = "#FAFBFC";     // 块悬停背景

// 编辑器内容宽度（设计 #62）：居中栏，阅读舒适。
constexpr int kContentWidth = 820;

// ---------------- 间距 token（UI 方案 §2、§12） ----------------
// 块的装饰刻意做得很轻，让正文占据页面：论文编辑器的视觉焦点
// 是文字，而不是控件。
namespace spacing {
constexpr int kBlockGap = 12;          // 块之间的插入／放置条
constexpr int kBlockPaddingH = 8;      // 卡片左右内边距
constexpr int kBlockPaddingV = 2;      // 卡片上下内边距
constexpr int kBlockHeaderHeight = 15; // 悬停时的标题条
constexpr int kFocusLine = 2;          // 左侧状态线（聚焦／缺失）
constexpr int kCardRadius = 6;         // 卡片圆角半径
constexpr int kEditorDocMargin = 4;    // 编辑器内部 QTextDocument 的外边距
}  // namespace spacing

// ---------------- 排版 token（UI 方案 §3、§5、§12） ----------------
// 稿件界面统一的 GUI 层级。层级来自字号 + 字重 + 间距，
// 而不是把每个标题都加粗到极致：论文标题保持 Bold，
// 其下的标题为 DemiBold (600)。
namespace typography {
constexpr double kTitlePt = 22.0;
constexpr double kAuthorsPt = 12.0;
constexpr double kAffiliationsPt = 10.5;
constexpr double kAbstractPt = 11.5;
constexpr double kKeywordsPt = 10.5;
constexpr double kSectionPt = 16.5;
constexpr double kSubsectionPt = 14.0;
constexpr double kSubsubsectionPt = 12.5;
constexpr double kBodyPt = 12.0;
constexpr double kCaptionPt = 10.5;
constexpr double kEquationSourcePt = 10.5;
constexpr double kUiPt = 10.0;         // 应用外框基准字号

constexpr int kBodyLineHeight = 150;     // % 比例行距，正文
constexpr int kAbstractLineHeight = 155; // % 比例行距，摘要
}  // namespace typography

// 块编辑器的视觉角色；与排版表一一对应。
enum class BlockVisualRole {
    Body,
    Abstract,
    Title,
    Authors,
    Affiliations,
    Keywords,
    SectionTitle,
    SubsectionTitle,
    SubsubsectionTitle,
    Caption,
    EquationSource,
};

// GUI 字体（UI 方案 §7）：系统 UI 字体优先，其后是 Qt 回退字体。
// 不要硬编码单一字族——在 Windows 上会落到原生 UI 字体，在 Linux
// 上当存在 Noto Sans CJK 时由 fontconfig 解析中日韩文字。GUI 字体
// 与 PDF 的字体毫无关系。
inline QFont UiFont(qreal point_size = typography::kUiPt,
                    int weight = QFont::Normal) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    QStringList families = font.families();
    for (const QString& extra :
         {QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans"),
          QStringLiteral("DejaVu Sans")}) {
        if (!families.contains(extra)) families << extra;
    }
    font.setFamilies(families);
    font.setPointSizeF(point_size);
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

inline QFont MonoFont(qreal point_size = typography::kUiPt) {
    QFont font(QStringLiteral("Monospace"));
    font.setStyleHint(QFont::TypeWriter);
    font.setFamilies({QStringLiteral("Ubuntu Mono"),
                      QStringLiteral("DejaVu Sans Mono"),
                      QStringLiteral("Monospace")});
    font.setPointSizeF(point_size);
    return font;
}

// 应用级外框字体；设置在 QApplication 上，因此样式表无需固定
// font-size（样式表字体会覆盖每一次 setFont()）。
inline QFont AppFont() { return UiFont(typography::kUiPt); }

// 稿件界面某个视觉角色所用的字体（UI 方案 §3）。
inline QFont EditorFont(BlockVisualRole role) {
    using R = BlockVisualRole;
    switch (role) {
        case R::Title:
            return UiFont(typography::kTitlePt, QFont::Bold);
        case R::Authors:
            return UiFont(typography::kAuthorsPt, QFont::Medium);
        case R::Affiliations:
            return UiFont(typography::kAffiliationsPt, QFont::Normal);
        case R::Abstract:
            return UiFont(typography::kAbstractPt, QFont::Normal);
        case R::Keywords:
            return UiFont(typography::kKeywordsPt, QFont::Medium);
        case R::SectionTitle:
            return UiFont(typography::kSectionPt, QFont::DemiBold);
        case R::SubsectionTitle:
            return UiFont(typography::kSubsectionPt, QFont::DemiBold);
        case R::SubsubsectionTitle:
            return UiFont(typography::kSubsubsectionPt, QFont::DemiBold);
        case R::Caption:
            return UiFont(typography::kCaptionPt, QFont::Normal);
        case R::Body:
            return UiFont(typography::kBodyPt, QFont::Normal);
        case R::EquationSource:
            return MonoFont(typography::kEquationSourcePt);
    }
    return UiFont(typography::kBodyPt);
}

// 某个角色阅读时使用的行高（%）；0 表示「保持默认」。
inline int LineHeightFor(BlockVisualRole role) {
    switch (role) {
        case BlockVisualRole::Body:
            return typography::kBodyLineHeight;
        case BlockVisualRole::Abstract:
            return typography::kAbstractLineHeight;
        default:
            return 0;
    }
}

// 某个角色是否以次要文字颜色渲染。
inline bool IsSecondaryRole(BlockVisualRole role) {
    switch (role) {
        case BlockVisualRole::Affiliations:
        case BlockVisualRole::Keywords:
        case BlockVisualRole::Caption:
            return true;
        default:
            return false;
    }
}

// 把字体 + 比例行距应用到文本文档。调用方需自行用「程序化编辑」
// 保护包裹此调用，使该变更绝不会被误认为用户输入。字体环境必须在
// 行内内容加载*之前*就位（UI 方案 §6）：行内数学对象在插入时
// 依据 document()->defaultFont() 确定自身大小与对齐。
inline void ApplyDocumentTypography(QTextDocument* doc, const QFont& font,
                                    int line_height_percent) {
    if (!doc) return;
    doc->setDefaultFont(font);
    if (line_height_percent <= 0) return;
    QTextCursor cursor(doc);
    cursor.select(QTextCursor::Document);
    QTextBlockFormat format;
    format.setLineHeight(line_height_percent,
                         QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(format);
    cursor.clearSelection();
}

// 全局应用样式表。通用 QWidget 选择器上刻意不设置 font-family/font-size：
// QApplication::setFont(AppFont()) 是外框字体的唯一来源，
// 而样式表字体会静默覆盖编辑器排版所依赖的每一次逐 widget setFont()。
inline QString AppStyleSheet() {
    return QString(R"(
QMainWindow, QWidget { background: %1; color: %2; }
QSplitter::handle { background: %3; width: 1px; height: 1px; }
QMenuBar { background: %1; border-bottom: 1px solid %3; }
QMenuBar::item:selected { background: %4; border-radius: 4px; }
QMenu { background: white; border: 1px solid %3; border-radius: 6px; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: %4; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #C9CDD4; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #AEB4BD; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #C9CDD4; border-radius: 5px; min-width: 30px; }
QPushButton { background: white; border: 1px solid %3; border-radius: 6px;
              padding: 5px 14px; color: %2; }
QPushButton:hover { background: %4; border-color: %5; }
QPushButton#primary { background: %5; color: white; border: none; font-weight: 600; }
QPushButton#primary:hover { background: #235098; }
QLineEdit, QPlainTextEdit, QTreeWidget, QListWidget {
    background: white; border: 1px solid %3; border-radius: 6px; color: %2; }
QTreeWidget::item:selected, QListWidget::item:selected { background: %4; color: %2; }
QTabWidget::pane { border: none; }
QTabBar::tab { background: transparent; color: %6; padding: 6px 12px;
               border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %2; border-bottom: 2px solid %5; }
QLabel#panelTitle { color: %6; font-size: 8pt; font-weight: 700; letter-spacing: 1px; }
QComboBox { background: white; border: 1px solid %3; border-radius: 6px; padding: 3px 8px; }
)")
        .arg(kMainBackground, kPrimaryText, kDivider, kAccentSoft, kAccent,
             kSecondaryText);
}

}  // namespace pf::gui::theme
