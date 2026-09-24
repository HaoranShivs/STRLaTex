// MathPreviewRenderer 测试：属于 widget 层（需要 InlineEditorEditorTest.cpp
// 已经创建的 QApplication），但可在无头环境下安全运行。
//
// 这些压力测试显式选用有界的纯 Qt 兜底路径，因此它们守护的契约主要是
// 「始终返回可用的东西」：非空输入返回非空 pixmap、可供行内布局对齐的
// baseline，以及面对畸形或怪异源码时不崩溃、不挂起。
#include "TestMain.hpp"

#include <chrono>
#include <iostream>

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QStandardPaths>

#include "app/MathPreviewRenderer.h"

using namespace pf::gui;

namespace {

MathRenderStyle Style() {
    MathRenderStyle style;
    style.font_px = 18;
    style.color = QColor(20, 22, 26);
    style.device_pixel_ratio = 2.0;
    style.backend = MathRenderBackend::ApproximateOnly;
    return style;
}

bool HasTightTransparentMargins(const QImage& image) {
    if (image.isNull()) return false;
    int left = image.width(), top = image.height();
    int right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) == 0) continue;
            left = qMin(left, x);
            top = qMin(top, y);
            right = qMax(right, x);
            bottom = qMax(bottom, y);
        }
    }
    return right >= left && left <= 1 && top <= 1 &&
           image.width() - 1 - right <= 1 &&
           image.height() - 1 - bottom <= 1;
}

bool HasTransparentBorder(const QImage& image) {
    if (image.isNull() || image.width() < 3 || image.height() < 3)
        return false;
    for (int x = 0; x < image.width(); ++x) {
        if (qAlpha(image.pixel(x, 0)) != 0 ||
            qAlpha(image.pixel(x, image.height() - 1)) != 0)
            return false;
    }
    for (int y = 0; y < image.height(); ++y) {
        if (qAlpha(image.pixel(0, y)) != 0 ||
            qAlpha(image.pixel(image.width() - 1, y)) != 0)
            return false;
    }
    return true;
}

}  // namespace

PF_TEST(MathPreviewRendersFractionWithDescent) {
    const MathRenderStyle style = Style();
    const MathRenderResult res = RenderMathPreview(QStringLiteral("\\frac{a}{b}"), style);

    std::cout << "  frac: w=" << res.width << " h=" << res.height
              << " baseline=" << res.baseline << " exact=" << res.exact << "\n";

    PF_CHECK(!res.pixmap.isNull());
    PF_CHECK(res.width > 0);
    PF_CHECK(res.height > 0);
    PF_CHECK(res.baseline > 0);
    PF_CHECK(res.baseline < res.height);
    PF_CHECK_EQ(res.pixmap.devicePixelRatio(), style.device_pixel_ratio);
    PF_CHECK(res.exact);
    PF_CHECK(HasTightTransparentMargins(res.image));
}

PF_TEST(RealTexImageFitsInsideItsPdfPageWhenAvailable) {
    bool has_tex = !QStandardPaths::findExecutable("pdflatex").isEmpty();
    const QDir bundled(QStringLiteral(PF_INSTALL_ROOT) +
                       QStringLiteral("/runtime/texlive/bin"));
    for (const QFileInfo& platform : bundled.entryInfoList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFileInfo::exists(platform.absoluteFilePath() +
                              QStringLiteral("/pdflatex")))
            has_tex = true;
    }
    if (!has_tex || QStandardPaths::findExecutable("pdftocairo").isEmpty())
        return;
    MathRenderStyle style = Style();
    style.backend = MathRenderBackend::RealTexPreferred;
    const QStringList formulas = {
        QStringLiteral("x"),
        QStringLiteral("\\frac{\\partial u}{\\partial t}"),
        QStringLiteral("\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}"),
        QStringLiteral("\\sqrt{x^2+y^2}")};
    for (const QString& formula : formulas) {
        const MathRenderResult result = RenderMathPreviewImage(formula, style);
        PF_CHECK(result.HasPixels());
        PF_CHECK(result.used_tex);
        PF_CHECK(result.width > 0 && result.height > 0);
        if (result.used_tex) {
            // TrimTransparentMargins leaves one physical pixel of transparent
            // space. An opaque outer edge means the PDF page clipped ink.
            PF_CHECK(HasTransparentBorder(result.image));
            PF_CHECK(HasTightTransparentMargins(result.image));
        }
    }
}

PF_TEST(MathPreviewRendersScriptsAndGreek) {
    const MathRenderStyle style = Style();
    const MathRenderResult plain = RenderMathPreview(QStringLiteral("x"), style);
    const MathRenderResult scripted = RenderMathPreview(QStringLiteral("x_i^2 + \\alpha"), style);

    std::cout << "  x: w=" << plain.width << " h=" << plain.height
              << " baseline=" << plain.baseline << "\n";
    std::cout << "  x_i^2 + alpha: w=" << scripted.width << " h=" << scripted.height
              << " baseline=" << scripted.baseline << " exact=" << scripted.exact << "\n";

    PF_CHECK(!plain.pixmap.isNull());
    PF_CHECK(!scripted.pixmap.isNull());
    PF_CHECK(scripted.width > plain.width);
    PF_CHECK(scripted.baseline > 0);
    PF_CHECK(scripted.baseline < scripted.height);
    PF_CHECK(scripted.exact);
}

PF_TEST(MathPreviewRendersAlignedRows) {
    const MathRenderStyle style = Style();
    const MathRenderResult single = RenderMathPreview(QStringLiteral("a=b"), style);
    const MathRenderResult aligned =
        RenderMathPreview(QStringLiteral("\\begin{aligned}a&=b\\\\c&=d\\end{aligned}"), style);

    std::cout << "  a=b: w=" << single.width << " h=" << single.height << "\n";
    std::cout << "  aligned: w=" << aligned.width << " h=" << aligned.height
              << " baseline=" << aligned.baseline << " exact=" << aligned.exact << "\n";

    PF_CHECK(!single.pixmap.isNull());
    PF_CHECK(!aligned.pixmap.isNull());
    PF_CHECK(aligned.height > single.height);
    PF_CHECK(aligned.baseline > 0);
    PF_CHECK(aligned.baseline < aligned.height);
}

PF_TEST(MathPreviewRendersCasesAndMatrix) {
    const MathRenderStyle style = Style();
    const MathRenderResult cases =
        RenderMathPreview(QStringLiteral("\\begin{cases}x&y\\\\z&w\\end{cases}"), style);
    const MathRenderResult pmatrix =
        RenderMathPreview(QStringLiteral("\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}"), style);

    std::cout << "  cases: w=" << cases.width << " h=" << cases.height
              << " baseline=" << cases.baseline << "\n";
    std::cout << "  pmatrix: w=" << pmatrix.width << " h=" << pmatrix.height
              << " baseline=" << pmatrix.baseline << "\n";

    PF_CHECK(!cases.pixmap.isNull());
    PF_CHECK(cases.width > 0);
    PF_CHECK(cases.height > 0);
    PF_CHECK(!pmatrix.pixmap.isNull());
    PF_CHECK(pmatrix.width > 0);
    PF_CHECK(pmatrix.height > 0);
}

PF_TEST(MathPreviewFallsBackInsteadOfCrashing) {
    const MathRenderStyle style = Style();
    const MathRenderResult unknown =
        RenderMathPreview(QStringLiteral("\\thisIsNotACommand{"), style);
    const MathRenderResult truncated = RenderMathPreview(QStringLiteral("\\frac{a"), style);

    std::cout << "  unknown: w=" << unknown.width << " h=" << unknown.height
              << " exact=" << unknown.exact << " note=" << unknown.note.toStdString() << "\n";
    std::cout << "  frac-truncated: w=" << truncated.width << " h=" << truncated.height
              << " exact=" << truncated.exact << " note=" << truncated.note.toStdString() << "\n";

    PF_CHECK(!unknown.pixmap.isNull());
    PF_CHECK(!truncated.pixmap.isNull());
    // 兜底的关键就在于它会被上报，而不是悄无声息。
    PF_CHECK(!unknown.exact);
    PF_CHECK(!truncated.exact);
    PF_CHECK(!truncated.note.isEmpty());
}

PF_TEST(MathPreviewEmptyInputIsNull) {
    const MathRenderStyle style = Style();
    const MathRenderResult empty = RenderMathPreview(QString(), style);
    const MathRenderResult blank = RenderMathPreview(QStringLiteral("   \n\t "), style);

    PF_CHECK(empty.pixmap.isNull());
    PF_CHECK_EQ(empty.width, 0);
    PF_CHECK_EQ(empty.height, 0);
    PF_CHECK(empty.exact);
    PF_CHECK(blank.pixmap.isNull());
}

PF_TEST(MathPreviewNeverNullOnFuzz) {
    const MathRenderStyle style = Style();
    const QStringList fuzz = {
        QStringLiteral("\\frac{a}{b}"),
        QStringLiteral("\\frac{\\frac{a}{b}}{\\frac{c}{d}}"),
        QStringLiteral("\\left(\\frac{a}{b}\\right)^2"),
        QStringLiteral("{{{{{{{{{{x}}}}}}}}}}"),
        QStringLiteral("\\\\"),
        QStringLiteral("&"),
        QStringLiteral("\\sum_{i=1}^{n} i^2"),
        QStringLiteral("\\prod_{k=0}^{\\infty} \\frac{1}{k!}"),
        QStringLiteral("\\int_0^1 x^2 \\, dx"),
        QStringLiteral("\\oint_C \\vec{F} \\cdot d\\vec{r}"),
        QStringLiteral("\\lim_{n \\to \\infty} \\left(1 + \\frac{1}{n}\\right)^n"),
        QStringLiteral("\\sqrt{x} + \\sqrt[3]{y} + \\sqrt{\\frac{a}{b}}"),
        QStringLiteral("x_i^2"),
        QStringLiteral("x^{a^{b^{c}}}"),
        QStringLiteral("\\hat{x} + \\bar{y} + \\vec{z} + \\dot{a} + \\ddot{b} + \\tilde{c}"),
        QStringLiteral("\\overline{AB} + \\underline{CD}"),
        QStringLiteral("\\mathbf{A}\\mathcal{B}\\mathrm{C}\\mathit{D}\\mathsf{E}\\mathtt{F}"),
        QStringLiteral("\\text{hello world} + \\operatorname{rank}(A)"),
        QStringLiteral("\\begin{matrix}a&b\\\\c&d\\end{matrix}"),
        QStringLiteral("\\begin{bmatrix}1&0\\\\0&1\\end{bmatrix}"),
        QStringLiteral("\\begin{cases}x&y\\\\z&w\\end{cases}"),
        QStringLiteral("\\begin{aligned}a&=b\\\\c&=d\\end{aligned}"),
        QStringLiteral("\\begin{pmatrix}\\frac{a}{b}&c\\\\d&\\sqrt{e}\\end{pmatrix}"),
        QStringLiteral("\\begin{unknownenv}a&b\\end{unknownenv}"),
        QStringLiteral("\\frac{a"),
        QStringLiteral("\\thisIsNotACommand{"),
        QStringLiteral("{unbalanced"),
        QStringLiteral("}{"),
        QStringLiteral("\\left(\\frac{a}{b}"),
        QStringLiteral("\\right)"),
        QStringLiteral("\\alpha\\beta\\gamma\\delta\\epsilon\\varepsilon\\pi\\rho\\sigma\\omega"),
        QStringLiteral("\\Gamma\\Delta\\Theta\\Lambda\\Xi\\Pi\\Sigma\\Upsilon\\Phi\\Psi\\Omega"),
        QStringLiteral("\\infty\\partial\\nabla\\pm\\times\\div\\cdot\\leq\\geq\\neq\\approx"),
        QStringLiteral("\\forall x \\in A: x \\notin B \\Rightarrow x \\subset C"),
        QStringLiteral("a \\\\ b \\\\ c"),
        QStringLiteral("^2"),
        QStringLiteral("_i"),
        QStringLiteral("x^"),
        QStringLiteral("\\sqrt[\\frac{a}{b}]{x}"),
        QStringLiteral("\\frac{}{}"),
        QStringLiteral("\\text{}"),
        QStringLiteral("\\begin{aligned}\\end{aligned}"),
        QStringLiteral("!!! ??? ::: ;;; ''' \"\"\" @@@ ~~~"),
        QStringLiteral("\\ "),
        QStringLiteral("\\!\\,\\;\\quad\\qquad"),
    };

    const auto start = std::chrono::steady_clock::now();
    int rendered = 0;
    for (const QString& src : fuzz) {
        const MathRenderResult res = RenderMathPreview(src, style);
        if (src.trimmed().isEmpty()) continue;
        PF_CHECK(!res.pixmap.isNull());
        PF_CHECK(res.width > 0);
        PF_CHECK(res.height > 0);
        PF_CHECK(res.baseline > 0);
        PF_CHECK(res.baseline < res.height);
        PF_CHECK_EQ(res.pixmap.devicePixelRatio(), style.device_pixel_ratio);
        ++rendered;
    }
    // 确定性的压力输入：即使嵌套使用无花括号的命令参数或 \left/\right 链，
    // 递归也必须是有界的。
    QStringList stress = fuzz;
    stress << QStringLiteral("\\frac").repeated(300) + QStringLiteral(" a b");
    stress << QStringLiteral("\\hat").repeated(300) + QStringLiteral(" x");
    stress << QStringLiteral("\\sqrt").repeated(300) + QStringLiteral(" x");
    stress << QStringLiteral("\\left(").repeated(300) + QStringLiteral("x") +
                  QStringLiteral("\\right)").repeated(300);
    stress << QStringLiteral("{").repeated(1000) + QStringLiteral("x") +
                  QStringLiteral("}").repeated(1000);
    for (const QString& src : stress) {
        const MathRenderResult res = RenderMathPreview(src, style);
        PF_CHECK(!res.pixmap.isNull());
        PF_CHECK(res.width > 0);
        PF_CHECK(res.height > 0);
        PF_CHECK(res.baseline > 0);
        PF_CHECK(res.baseline < res.height);
        ++rendered;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::cout << "  fuzz: rendered " << rendered << " inputs in " << ms << " ms\n";
    PF_CHECK(rendered > 30);
    PF_CHECK(ms < 5000);
}
