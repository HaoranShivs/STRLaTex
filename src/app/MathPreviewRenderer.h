#pragma once
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QString>

namespace pf::gui {

enum class MathRenderBackend {
    // 用真实的 TeX 引擎编译表达式。只有在运行环境不可用或 TeX 拒绝
    // 该源文件时，才使用受限的 QPainter 渲染器。
    RealTexPreferred,
    // 用于压力测试的快速确定性渲染器，同时作为回退方案。
    ApproximateOnly,
};

struct MathRenderStyle {
    int font_px = 18;                  // 基础字体像素大小
    QColor color = QColor(20, 22, 26); // 字形颜色
    qreal device_pixel_ratio = 2.0;    // 以 2 倍渲染以获得清晰效果
    QString font_family;               // 缓存/样式标识
    QString template_id;               // 缓存/模板标识
    MathRenderBackend backend = MathRenderBackend::RealTexPreferred;
};

struct MathRenderResult {
    // P0-07：像素以 QImage 形式传递，这样渲染就能在 worker 线程上运行
    //（QPixmap 只能在 GUI 线程上创建）。GUI 侧的入口点会转换为 `pixmap`；
    // worker 侧的调用方使用 `image`。
    QImage image;
    QPixmap pixmap;    // 仅空输入时为 null；dpr 设为 style.device_pixel_ratio
    int width = 0;     // 逻辑像素
    int height = 0;    // 逻辑像素
    int baseline = 0;  // 从 pixmap 顶部到数学基线的逻辑像素数
    bool exact = true; // 真实 TeX 或精确的 ApproximateOnly 输出时为 true
    bool used_tex = false;
    QString note; // 人类可读的渲染器/回退状态
    // 图像栅格化时所用的设备像素比（worker 侧：QPixmap 转换会在 GUI 线程上应用它）。
    qreal device_pixel_ratio = 1.0;

    // 有可绘制内容时为 true。
    bool HasPixels() const {
        return !pixmap.isNull() || !image.isNull();
    }
};

// 渲染 LaTeX 数学正文。对非空输入始终返回可用的结果。
// 仅限 GUI 线程：结果携带 QPixmap。
MathRenderResult RenderMathPreview(const QString& latex, const MathRenderStyle& style);

// P0-07 worker 线程入口：渲染完全相同，但结果携带 QImage 而不带
// QPixmap，因此可以安全地在 GUI 线程之外调用。调用方在应用结果时
// 于 GUI 线程上将其转换为 QPixmap。
MathRenderResult RenderMathPreviewImage(const QString& latex, const MathRenderStyle& style);

} // namespace pf::gui
