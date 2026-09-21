#pragma once
// MathEditorDialog：Inline Math 工具栏动作与内联数学对象双击编辑
// 共用的数学源码编辑器。
//
// 它严格呈现设计所要求的边界：
//   * 用户只编辑数学 BODY（绝不编辑定界符/环境），
//   * 预览随源码变化而更新，
//   * 校验绝不改写源码；无效 body 保留其文本并显示其错误
//     （设计 §7/§8）。

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

    // 编辑后的数学 body。
    QString latex() const;

    // 测试/嵌入钩子。
    QPlainTextEdit* SourceEdit() const { return source_; }
    void SetSourceForTest(const QString& latex);
    void RefreshPreviewNow();
    QString StateText() const;

private:
    void RefreshPreview();
    // P0-07：应用异步渲染出的预览（GUI 线程）。
    void ApplyRenderedPreview(const QString& latex, const QImage& image,
                              int width, int height, int baseline,
                              qreal device_pixel_ratio, const QString& note,
                              bool exact);

    QPlainTextEdit* source_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* debounce_ = nullptr;
    // P0-07：对话框通过共享 worker 渲染；只有防抖留在 GUI 线程，
    // 且它仅用于降低请求量。
    std::uint64_t preview_generation_ = 0;
};

}  // namespace pf::gui
