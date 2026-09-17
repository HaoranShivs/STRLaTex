#pragma once
// PaperForge GUI theme (design doc sections 2, 64, 66): light theme only,
// editor is the visual center, side panels slightly grey.
//
// UI typography plan (2024 UI adjustment §12): every spacing / font number
// the editor uses lives in the theme namespaces below. BlockEditor.cpp must
// not grow ad-hoc UiFont(11) / setFixedHeight(18) literals; change a token
// here and the whole GUI follows.
//
// The GUI layout is decoupled from the PDF layout: these sizes serve reading
// and editing only; the final PDF stays under Template + LaTeX control.

#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

namespace pf::gui::theme {

// Palette (design #2)
constexpr const char* kMainBackground = "#F3F4F6";
constexpr const char* kEditorBackground = "#FFFFFF";
constexpr const char* kSidePanel = "#F8F9FB";
constexpr const char* kDivider = "#E4E7EC";
constexpr const char* kPrimaryText = "#202124";
constexpr const char* kSecondaryText = "#667085";
constexpr const char* kDisabledText = "#98A2B3";
constexpr const char* kAccent = "#2A5DB0";         // focus line / links
constexpr const char* kAccentSoft = "#EAF1FB";     // hover background
constexpr const char* kError = "#D64545";
constexpr const char* kErrorSoft = "#FDF0F0";
constexpr const char* kWarning = "#B7791F";
constexpr const char* kWarningSoft = "#FBF5E9";
constexpr const char* kOk = "#2F855A";
constexpr const char* kBlockHover = "#FAFBFC";     // block hover background

// Editor content width (design #62): centered column, comfortable reading.
constexpr int kContentWidth = 820;

// ---------------- Spacing tokens (UI plan §2, §12) ----------------
// Block chrome is deliberately thin so prose owns the page: the visual focus
// of a paper editor is the text, not the controls.
namespace spacing {
constexpr int kBlockGap = 12;          // insert/drop strip between blocks
constexpr int kBlockPaddingH = 8;      // card left/right padding
constexpr int kBlockPaddingV = 2;      // card top/bottom padding
constexpr int kBlockHeaderHeight = 15; // hover header strip
constexpr int kFocusLine = 2;          // left state line (focus/missing)
constexpr int kCardRadius = 6;         // card corner radius
constexpr int kEditorDocMargin = 4;    // QTextDocument margin inside editors
}  // namespace spacing

// ---------------- Typography tokens (UI plan §3, §5, §12) ----------------
// A single GUI hierarchy for the manuscript surface. Hierarchy comes from
// size + weight + spacing, not from making every heading maximally bold:
// the paper title stays Bold, headings below it are DemiBold (600).
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
constexpr double kUiPt = 10.0;         // application chrome baseline

constexpr int kBodyLineHeight = 150;     // % proportional, body text
constexpr int kAbstractLineHeight = 155; // % proportional, abstract
}  // namespace typography

// Visual role of a block's editor; maps 1:1 onto the typography table.
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

// GUI font (UI plan §7): system UI font first, Qt fallbacks after. Do not
// hard-code a single family - on Windows this lands on the native UI face,
// on Linux fontconfig resolves CJK through Noto Sans CJK when present. The
// GUI font has no relationship to the PDF's fonts.
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

// The application-wide chrome font; set on QApplication so no stylesheet
// needs to pin font-size (a stylesheet font would override every setFont()).
inline QFont AppFont() { return UiFont(typography::kUiPt); }

// The font for one visual role of the manuscript surface (UI plan §3).
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

// Line height (%) a role reads with; 0 means "leave the default".
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

// Whether a role reads in the secondary text color.
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

// Apply font + proportional line height to a text document. Callers wrap
// this in their own "programmatic edit" guard so the change is never
// mistaken for user input. The font environment must be in place *before*
// inline content is loaded (UI plan §6): inline math objects size and align
// themselves from document()->defaultFont() at insertion time.
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

// Global application stylesheet. Deliberately no font-family/font-size on
// the generic QWidget selector: QApplication::setFont(AppFont()) is the
// single source of the chrome font, and stylesheet fonts would silently
// override every per-widget setFont() the editor typography relies on.
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
