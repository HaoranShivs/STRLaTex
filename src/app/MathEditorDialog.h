#pragma once
// MathEditorDialog: the math source editor shared by the Inline Math toolbar
// action and the double-click edit on an inline math object.
//
// It shows exactly the boundary the design asks for:
//   * the user edits the math BODY (never the delimiters/environment),
//   * the preview updates as the source changes,
//   * validation never rewrites the source; an invalid body keeps its text
//     and shows its error (design §7/§8).

#include <QDialog>
#include <QImage>

#include <cstdint>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QTimer;

namespace pf::gui {

class MathEditorDialog : public QDialog {
    Q_OBJECT

public:
    explicit MathEditorDialog(const QString& latex, QWidget* parent = nullptr);

    // The edited math body.
    QString latex() const;

    // Test/embedding hooks.
    QPlainTextEdit* SourceEdit() const { return source_; }
    void SetSourceForTest(const QString& latex);
    void RefreshPreviewNow();
    QString StateText() const;

private:
    void RefreshPreview();
    // P0-07: apply an asynchronously rendered preview (GUI thread).
    void ApplyRenderedPreview(const QString& latex, const QImage& image,
                              int width, int height, int baseline,
                              qreal device_pixel_ratio, const QString& note,
                              bool exact);

    QPlainTextEdit* source_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* debounce_ = nullptr;
    // P0-07: the dialog renders through the shared worker; only the debounce
    // stays on the GUI thread, and it merely reduces request volume.
    std::uint64_t preview_generation_ = 0;
};

}  // namespace pf::gui
