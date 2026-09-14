#pragma once
// PaperForge GUI theme (design doc sections 2, 64, 66): light theme only,
// editor is the visual center, side panels slightly grey.

#include <QFont>
#include <QString>
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

// Editor content width (design #62): centered column, comfortable reading.
constexpr int kContentWidth = 820;

inline QFont UiFont(int point_size = 10, bool bold = false) {
    QFont font("Noto Sans");
    font.setPointSize(point_size);
    font.setBold(bold);
    return font;
}

inline QFont MonoFont(int point_size = 10) {
    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);
    font.setPointSize(point_size);
    return font;
}

// Global application stylesheet.
inline QString AppStyleSheet() {
    return QString(R"(
QMainWindow, QWidget { background: %1; color: %2; font-family: "Noto Sans"; font-size: 10pt; }
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
