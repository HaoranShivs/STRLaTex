// MathPreviewRenderer: a self-contained, pure-Qt LaTeX math preview engine.
//
// There is deliberately no TeX here: the caller types a math *body* (no
// `$`/`\(` delimiters) and gets a QPixmap back. The implementation is a small
// recursive-descent parser that turns the source into a box tree (width /
// ascent / descent + a paint closure) and then paints that tree with
// QPainter. Anything the parser does not understand degrades to literal text
// and flips MathRenderResult::exact to false; nothing here throws, and every
// parse loop is guaranteed to advance, so no input can hang or crash.
#include "MathPreviewRenderer.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace pf::gui {
namespace {

// ---------------------------------------------------------------------------
// Limits and constants
// ---------------------------------------------------------------------------

constexpr int kMaxDepth = 48;        // recursion guard for groups/environments
constexpr qreal kScriptScale = 0.7;  // ^ / _ shrink factor
constexpr qreal kFracScale = 0.85;   // numerator / denominator shrink factor
constexpr qreal kMinScale = 0.45;    // never shrink below this (keeps glyphs legible)

void NoopDraw(QPainter&, qreal, qreal) {}

QString U(char16_t cp) { return QString(QChar(cp)); }

bool IsAsciiLetter(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

bool IsSpecialChar(QChar c) {
    return c == u'\\' || c == u'{' || c == u'}' || c == u'^' || c == u'_' ||
           c.isSpace();
}

QString CollapseSpaces(const QString& in) {
    QString out;
    out.reserve(in.size());
    for (QChar c : in) out.append(c.isSpace() ? QChar(u' ') : c);
    return out;
}

QString CollapseWsTrim(const QString& in) {
    QString out;
    out.reserve(in.size());
    bool pendingSpace = false;
    for (QChar c : in) {
        if (c.isSpace()) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) out.append(QChar(u' '));
        pendingSpace = false;
        out.append(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Box tree
// ---------------------------------------------------------------------------

enum class Kind { HBox, Text, Fraction, Radical, Accent, Script, Limits, Delim, Grid, Space, Rule };

struct Box;
using BoxPtr = std::shared_ptr<Box>;

struct Box {
    qreal w = 0;  // logical width
    qreal h = 0;  // logical ascent above the baseline
    qreal d = 0;  // logical descent below the baseline
    Kind kind = Kind::HBox;
    // Large operators (\sum ...) draw their limits above/below.
    bool limits_op = false;
    // Set on Script/Limits boxes so a following script can be stacked on the
    // same base instead of nesting (x_i^2 must put both on x).
    BoxPtr op_base, op_sup, op_sub;
    std::function<void(QPainter&, qreal, qreal)> draw = NoopDraw;
};

BoxPtr MakeEmpty() { return std::make_shared<Box>(); }

void DrawBox(const BoxPtr& b, QPainter& p, qreal x, qreal baseline) {
    if (b && b->draw) b->draw(p, x, baseline);
}

// Collapse a list of boxes into a single horizontal box sharing one baseline.
BoxPtr MakeHBox(const std::vector<BoxPtr>& items) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::HBox;
    std::vector<std::pair<BoxPtr, qreal>> placed;
    placed.reserve(items.size());
    qreal x = 0;
    for (const BoxPtr& it : items) {
        if (!it) continue;
        if (it->w == 0 && it->h == 0 && it->d == 0) continue;
        placed.emplace_back(it, x);
        x += it->w;
        box->h = std::max(box->h, it->h);
        box->d = std::max(box->d, it->d);
    }
    box->w = x;
    box->draw = [placed](QPainter& p, qreal ox, qreal baseline) {
        for (const auto& pr : placed)
            if (pr.first && pr.first->draw) pr.first->draw(p, ox + pr.second, baseline);
    };
    return box;
}

BoxPtr MakeSpace(qreal width) {
    auto box = MakeEmpty();
    box->kind = Kind::Space;
    box->w = std::max(0.0, width);
    return box;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

struct FontSpec {
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool sans = false;
    bool script = false;
    bool force_upright = false;   // \mathrm, \mathbf, ...
    bool auto_italic = true;      // plain math letters become italic
    qreal size = 1.0;             // extra size multiplier (large operators)
};

struct ParseState {
    const MathRenderStyle* style = nullptr;
    QFont base_font;
    QColor color;
    bool exact = true;
    QString note;

    void Fail(const QString& why) {
        if (exact) {
            exact = false;
            note = why;
        }
    }
};

QFont FontFor(const ParseState& st, const FontSpec& spec, qreal scale) {
    QFont f = st.base_font;
    qreal eff = scale * spec.size;
    if (!(eff > 0.01)) eff = kMinScale;
    eff = std::clamp(eff, kMinScale, 3.0);
    const int px = static_cast<int>(std::lround(st.style->font_px * eff));
    f.setPixelSize(std::max(4, px));
    if (spec.bold) f.setBold(true);
    if (spec.italic && !spec.force_upright) f.setItalic(true);
    if (spec.mono) {
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::Monospace);
    } else if (spec.sans) {
        f.setFamily(QStringLiteral("sans-serif"));
        f.setStyleHint(QFont::SansSerif);
    } else if (spec.script) {
        f.setFamily(QStringLiteral("serif"));
        f.setStyleHint(QFont::Serif);
        f.setItalic(true);
    }
    return f;
}

BoxPtr MakeText(const QString& text, const QFont& font, const QColor& color) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Text;
    if (text.isEmpty()) return box;
    const QFontMetricsF fm(font);
    box->w = std::max(0.0, fm.horizontalAdvance(text));
    box->h = std::max(0.0, fm.ascent());
    box->d = std::max(0.0, fm.descent());
    box->draw = [text, font, color](QPainter& p, qreal x, qreal baseline) {
        p.setFont(font);
        p.setPen(color);
        p.drawText(QPointF(x, baseline), text);
    };
    return box;
}

// ---------------------------------------------------------------------------
// Composite boxes
// ---------------------------------------------------------------------------

BoxPtr MakeFraction(const BoxPtr& num, const BoxPtr& den, qreal fontPx, const QColor& color) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Fraction;
    const BoxPtr n = num ? num : MakeEmpty();
    const BoxPtr d = den ? den : MakeEmpty();
    const qreal rule = std::max(1.0, std::round(fontPx / 16.0));
    const qreal gap = std::max(1.0, std::round(fontPx * 0.18));
    const qreal half = rule / 2.0;
    box->w = std::max(n->w, d->w) + std::max(2.0, std::round(fontPx * 0.20));
    box->h = n->h + n->d + gap + half;
    box->d = d->h + d->d + gap + half;
    const qreal numBase = -(gap + half + n->d);
    const qreal denBase = gap + half + d->h;
    const qreal w = box->w;
    box->draw = [n, d, w, rule, numBase, denBase, color](QPainter& p, qreal x, qreal baseline) {
        p.fillRect(QRectF(x, baseline - rule / 2.0, w, rule), color);
        DrawBox(n, p, x + (w - n->w) / 2.0, baseline + numBase);
        DrawBox(d, p, x + (w - d->w) / 2.0, baseline + denBase);
    };
    return box;
}

BoxPtr MakeScriptSide(const BoxPtr& base, const BoxPtr& sup, const BoxPtr& sub, qreal fontPx) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Script;
    const BoxPtr b = base ? base : MakeEmpty();
    box->op_base = b;
    box->op_sup = sup;
    box->op_sub = sub;
    const qreal gapx = std::max(1.0, std::round(fontPx * 0.08));
    const qreal supShift = std::max(fontPx * 0.42, b->h - fontPx * 0.25);
    const qreal subShift = std::max(fontPx * 0.20, b->d + fontPx * 0.10);
    qreal scriptW = 0;
    if (sup) scriptW = std::max(scriptW, sup->w);
    if (sub) scriptW = std::max(scriptW, sub->w);
    box->w = b->w + gapx + scriptW;
    box->h = b->h;
    box->d = b->d;
    if (sup) box->h = std::max(box->h, supShift + sup->h);
    if (sub) box->d = std::max(box->d, subShift + sub->d);
    const qreal scriptX = b->w + gapx;
    box->draw = [b, sup, sub, scriptX, supShift, subShift](QPainter& p, qreal x, qreal baseline) {
        DrawBox(b, p, x, baseline);
        if (sup) DrawBox(sup, p, x + scriptX, baseline - supShift);
        if (sub) DrawBox(sub, p, x + scriptX, baseline + subShift);
    };
    return box;
}

BoxPtr MakeLimits(const BoxPtr& base, const BoxPtr& sup, const BoxPtr& sub, qreal fontPx) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Limits;
    box->limits_op = true;
    const BoxPtr b = base ? base : MakeEmpty();
    box->op_base = b;
    box->op_sup = sup;
    box->op_sub = sub;
    const qreal gap = std::max(1.0, std::round(fontPx * 0.14));
    box->w = b->w;
    if (sup) box->w = std::max(box->w, sup->w);
    if (sub) box->w = std::max(box->w, sub->w);
    qreal supBase = 0;
    qreal subBase = 0;
    box->h = b->h;
    box->d = b->d;
    if (sup) {
        supBase = -(b->h + gap) - sup->d;
        box->h = std::max(box->h, b->h + gap + sup->d + sup->h);
    }
    if (sub) {
        subBase = b->d + gap + sub->h;
        box->d = std::max(box->d, b->d + gap + sub->h + sub->d);
    }
    const qreal w = box->w;
    box->draw = [b, sup, sub, w, supBase, subBase](QPainter& p, qreal x, qreal baseline) {
        DrawBox(b, p, x + (w - b->w) / 2.0, baseline);
        if (sup) DrawBox(sup, p, x + (w - sup->w) / 2.0, baseline + supBase);
        if (sub) DrawBox(sub, p, x + (w - sub->w) / 2.0, baseline + subBase);
    };
    return box;
}

BoxPtr AttachScript(const BoxPtr& base, bool isSup, const BoxPtr& script, qreal fontPx) {
    BoxPtr b = base ? base : MakeEmpty();
    // Scripts stack on one base: x_i^2, \sum_{i=1}^{n}.
    if (b->kind == Kind::Script || b->kind == Kind::Limits) {
        BoxPtr core = b->op_base ? b->op_base : b;
        BoxPtr s = b->op_sup;
        BoxPtr u = b->op_sub;
        if (isSup) s = script;
        else u = script;
        if (b->limits_op) return MakeLimits(core, s, u, fontPx);
        return MakeScriptSide(core, s, u, fontPx);
    }
    if (b->limits_op) return MakeLimits(b, isSup ? script : nullptr, isSup ? nullptr : script, fontPx);
    return MakeScriptSide(b, isSup ? script : nullptr, isSup ? nullptr : script, fontPx);
}

BoxPtr MakeRadical(const BoxPtr& index, const BoxPtr& radicand, qreal fontPx, const QColor& color) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Radical;
    const BoxPtr r = radicand ? radicand : MakeEmpty();
    const qreal t = std::max(1.0, std::round(fontPx / 16.0));
    const qreal gap = std::max(1.0, std::round(fontPx * 0.14));
    const qreal rw = std::max(6.0, std::round(fontPx * 0.62));
    const qreal padRight = std::max(1.0, std::round(fontPx * 0.12));
    const qreal idxW = index ? index->w : 0.0;
    const qreal idxGap = index ? std::max(1.0, fontPx * 0.08) : 0.0;
    const qreal contentX = idxW + idxGap + rw;
    const qreal top = r->h + gap + t;
    box->w = contentX + r->w + padRight;
    box->h = top;
    box->d = r->d;
    qreal idxBase = 0;
    if (index) {
        idxBase = -top - index->d;
        box->h = std::max(box->h, -idxBase + index->h);
    }
    const qreal totalW = box->w;
    const qreal radX = contentX;
    const qreal rH = r->h;
    const qreal rD = r->d;
    box->draw = [index, r, t, rw, contentX, top, idxBase, totalW, radX, rH, rD, color](
                    QPainter& p, qreal x, qreal baseline) {
        const qreal yTop = baseline - top + t / 2.0;
        const qreal yBot = baseline + rD;
        const qreal glyphX = x + contentX - rw;
        QPainterPath path;
        path.moveTo(glyphX + rw * 0.05, baseline - rH * 0.40);
        path.lineTo(glyphX + rw * 0.38, yBot);
        path.lineTo(glyphX + rw * 0.80, yTop);
        path.lineTo(x + totalW, yTop);
        QPen pen(color);
        pen.setWidthF(t);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        if (index) DrawBox(index, p, x, baseline + idxBase);
        DrawBox(r, p, x + radX, baseline);
    };
    return box;
}

// A delimiter glyph sized to the enclosed box, or a stretched line for `|`.
BoxPtr MakeDelimiterFor(const QString& glyph, qreal contentH, qreal contentD, qreal fontPx,
                        const QColor& color, const ParseState& st) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Delim;
    if (glyph.isEmpty()) return box;
    const qreal target = std::max(fontPx, contentH + contentD) * 1.02;
    if (glyph == QStringLiteral("|") || glyph == U(0x2016)) {
        const qreal w = std::max(1.0, std::round(fontPx * (glyph == QStringLiteral("|") ? 0.09 : 0.16)));
        const qreal half = target / 2.0;
        box->w = w;
        box->h = half;
        box->d = half;
        box->draw = [w, target, color, half](QPainter& p, qreal x, qreal baseline) {
            p.fillRect(QRectF(x, baseline - half, w, target), color);
        };
        return box;
    }
    QFont f = st.base_font;
    f.setPixelSize(std::max(4, static_cast<int>(std::lround(fontPx))));
    for (int i = 0; i < 3; ++i) {
        const QFontMetricsF fm(f);
        const QRectF r = fm.boundingRect(glyph);
        const qreal gh = r.height();
        if (gh <= 0.5) break;
        const qreal k = target / gh;
        const int px = std::clamp(static_cast<int>(std::lround(f.pixelSize() * k)), 4, 4000);
        if (px == f.pixelSize()) break;
        f.setPixelSize(px);
    }
    const QFontMetricsF fm(f);
    const QRectF r = fm.boundingRect(glyph);
    const qreal centerRel = (contentD - contentH) / 2.0;
    const qreal baseOff = centerRel - (r.top() + r.bottom()) / 2.0;
    const qreal top = baseOff + r.top();
    const qreal bottom = baseOff + r.bottom();
    box->w = std::max(1.0, r.width());
    box->h = std::max(0.0, -top);
    box->d = std::max(0.0, bottom);
    box->draw = [glyph, f, color, baseOff](QPainter& p, qreal x, qreal baseline) {
        p.setFont(f);
        p.setPen(color);
        p.drawText(QPointF(x, baseline + baseOff), glyph);
    };
    return box;
}

BoxPtr MakeGlyphAccent(const BoxPtr& base, const QString& glyph, qreal fontPx, const QColor& color,
                       const ParseState& st) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Accent;
    const BoxPtr b = base ? base : MakeEmpty();
    QFont af = st.base_font;
    af.setPixelSize(std::max(4, static_cast<int>(std::lround(fontPx * 0.66))));
    const QFontMetricsF afm(af);
    const QRectF r = afm.boundingRect(glyph);
    const qreal gap = std::max(1.0, std::round(fontPx * 0.10));
    const qreal accentBottom = -(b->h + gap);
    const qreal baseOff = accentBottom - r.bottom();
    box->w = b->w;
    box->h = std::max(b->h, -(baseOff + r.top()));
    box->d = b->d;
    const qreal offX = (b->w - r.width()) / 2.0;
    box->draw = [b, glyph, af, color, baseOff, offX](QPainter& p, qreal x, qreal baseline) {
        DrawBox(b, p, x, baseline);
        p.setFont(af);
        p.setPen(color);
        p.drawText(QPointF(x + offX, baseline + baseOff), glyph);
    };
    return box;
}

BoxPtr MakeLineAccent(const BoxPtr& base, bool above, qreal fontPx, const QColor& color) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Accent;
    const BoxPtr b = base ? base : MakeEmpty();
    const qreal t = std::max(1.0, std::round(fontPx / 16.0));
    const qreal gap = std::max(1.0, std::round(fontPx * 0.14));
    box->w = b->w;
    if (above) {
        box->h = b->h + gap + t;
        box->d = b->d;
        const qreal lineTop = -box->h;
        box->draw = [b, t, lineTop, color](QPainter& p, qreal x, qreal baseline) {
            DrawBox(b, p, x, baseline);
            p.fillRect(QRectF(x, baseline + lineTop, b->w, t), color);
        };
    } else {
        box->h = b->h;
        box->d = b->d + gap + t;
        const qreal lineTop = b->d + gap;
        box->draw = [b, t, lineTop, color](QPainter& p, qreal x, qreal baseline) {
            DrawBox(b, p, x, baseline);
            p.fillRect(QRectF(x, baseline + lineTop, b->w, t), color);
        };
    }
    return box;
}

enum class ColAlign { Left, Center, Right };

BoxPtr MakeGrid(const std::vector<std::vector<BoxPtr>>& rows, ColAlign align, bool rlAlternate,
                qreal colGap, qreal rowGap) {
    auto box = std::make_shared<Box>();
    box->kind = Kind::Grid;
    if (rows.empty()) return box;
    size_t ncols = 0;
    for (const auto& r : rows) ncols = std::max(ncols, r.size());
    if (ncols == 0) return box;
    std::vector<qreal> colW(ncols, 0.0);
    for (const auto& r : rows)
        for (size_t j = 0; j < r.size(); ++j)
            if (r[j]) colW[j] = std::max(colW[j], r[j]->w);
    std::vector<qreal> colX(ncols, 0.0);
    for (size_t j = 1; j < ncols; ++j) colX[j] = colX[j - 1] + colW[j - 1] + colGap;
    std::vector<qreal> rowBase(rows.size(), 0.0);
    qreal cursor = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        qreal a = 0, d = 0;
        for (const auto& c : rows[i])
            if (c) {
                a = std::max(a, c->h);
                d = std::max(d, c->d);
            }
        rowBase[i] = cursor + a;
        cursor = rowBase[i] + d + rowGap;
    }
    const qreal totalH = std::max(0.0, cursor - rowGap);
    box->w = colX[ncols - 1] + colW[ncols - 1];
    box->h = rowBase[0];
    box->d = std::max(0.0, totalH - box->h);
    std::vector<std::tuple<BoxPtr, qreal, qreal>> placed;
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < rows[i].size(); ++j) {
            const BoxPtr& c = rows[i][j];
            if (!c || (c->w == 0 && c->h == 0 && c->d == 0)) continue;
            ColAlign a = align;
            if (rlAlternate) a = (j % 2 == 0) ? ColAlign::Right : ColAlign::Left;
            qreal x = colX[j];
            if (a == ColAlign::Right) x += colW[j] - c->w;
            else if (a == ColAlign::Center) x += (colW[j] - c->w) / 2.0;
            placed.emplace_back(c, x, rowBase[i] - rowBase[0]);
        }
    }
    box->draw = [placed](QPainter& p, qreal ox, qreal baseline) {
        for (const auto& t : placed)
            DrawBox(std::get<0>(t), p, ox + std::get<1>(t), baseline + std::get<2>(t));
    };
    return box;
}

// ---------------------------------------------------------------------------
// Symbol tables
// ---------------------------------------------------------------------------

const QHash<QString, QString>& SymbolTable() {
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> m;
        auto add = [&m](const char* name, int cp) { m.insert(QString::fromLatin1(name), U(static_cast<char16_t>(cp))); };
        // Greek, lower case.
        add("alpha", 0x03B1);
        add("beta", 0x03B2);
        add("gamma", 0x03B3);
        add("delta", 0x03B4);
        add("epsilon", 0x03B5);
        add("varepsilon", 0x03F5);
        add("zeta", 0x03B6);
        add("eta", 0x03B7);
        add("theta", 0x03B8);
        add("vartheta", 0x03D1);
        add("iota", 0x03B9);
        add("kappa", 0x03BA);
        add("lambda", 0x03BB);
        add("mu", 0x03BC);
        add("nu", 0x03BD);
        add("xi", 0x03BE);
        add("pi", 0x03C0);
        add("rho", 0x03C1);
        add("sigma", 0x03C3);
        add("tau", 0x03C4);
        add("upsilon", 0x03C5);
        add("phi", 0x03C6);
        add("varphi", 0x03D5);
        add("chi", 0x03C7);
        add("psi", 0x03C8);
        add("omega", 0x03C9);
        // Greek, upper case.
        add("Gamma", 0x0393);
        add("Delta", 0x0394);
        add("Theta", 0x0398);
        add("Lambda", 0x039B);
        add("Xi", 0x039E);
        add("Pi", 0x03A0);
        add("Sigma", 0x03A3);
        add("Upsilon", 0x03A5);
        add("Phi", 0x03A6);
        add("Psi", 0x03A8);
        add("Omega", 0x03A9);
        // Relations / operators / arrows / misc.
        add("infty", 0x221E);
        add("partial", 0x2202);
        add("nabla", 0x2207);
        add("pm", 0x00B1);
        add("mp", 0x2213);
        add("times", 0x00D7);
        add("div", 0x00F7);
        add("cdot", 0x22C5);
        add("leq", 0x2264);
        add("le", 0x2264);
        add("geq", 0x2265);
        add("ge", 0x2265);
        add("neq", 0x2260);
        add("ne", 0x2260);
        add("approx", 0x2248);
        add("equiv", 0x2261);
        add("propto", 0x221D);
        add("in", 0x2208);
        add("notin", 0x2209);
        add("subset", 0x2282);
        add("supset", 0x2283);
        add("subseteq", 0x2286);
        add("supseteq", 0x2287);
        add("cup", 0x222A);
        add("cap", 0x2229);
        add("forall", 0x2200);
        add("exists", 0x2203);
        add("neg", 0x00AC);
        add("lnot", 0x00AC);
        add("to", 0x2192);
        add("rightarrow", 0x2192);
        add("leftarrow", 0x2190);
        add("Rightarrow", 0x21D2);
        add("Leftarrow", 0x21D0);
        add("Leftrightarrow", 0x21D4);
        add("leftrightarrow", 0x2194);
        add("mapsto", 0x21A6);
        add("ldots", 0x2026);
        add("cdots", 0x22EF);
        add("dots", 0x2026);
        add("vdots", 0x22EE);
        add("ddots", 0x22F1);
        add("prime", 0x2032);
        add("circ", 0x2218);
        add("ast", 0x2217);
        add("star", 0x22C6);
        add("oplus", 0x2295);
        add("otimes", 0x2297);
        add("perp", 0x22A5);
        add("parallel", 0x2225);
        add("angle", 0x2220);
        add("triangle", 0x25B3);
        add("square", 0x25A1);
        add("emptyset", 0x2205);
        add("varnothing", 0x2205);
        add("ell", 0x2113);
        add("hbar", 0x210F);
        add("Re", 0x211C);
        add("Im", 0x2111);
        add("degree", 0x00B0);
        add("therefore", 0x2234);
        add("because", 0x2235);
        add("setminus", 0x2216);
        add("mid", 0x2223);
        add("langle", 0x27E8);
        add("rangle", 0x27E9);
        add("lfloor", 0x230A);
        add("rfloor", 0x230B);
        add("lceil", 0x2308);
        add("rceil", 0x2309);
        add("backslash", 0x005C);
        add("qquad", 0x2003);  // never looked up as a glyph; handled as spacing
        return m;
    }();
    return table;
}

// Function names rendered upright; the bool is "limits above/below".
bool FunctionName(const QString& name, bool& limits) {
    static const QHash<QString, bool> funcs = {
        {QStringLiteral("lim"), true},   {QStringLiteral("limsup"), true},
        {QStringLiteral("liminf"), true},{QStringLiteral("max"), true},
        {QStringLiteral("min"), true},   {QStringLiteral("sup"), true},
        {QStringLiteral("inf"), true},   {QStringLiteral("det"), true},
        {QStringLiteral("gcd"), true},   {QStringLiteral("log"), false},
        {QStringLiteral("ln"), false},   {QStringLiteral("exp"), false},
        {QStringLiteral("sin"), false},  {QStringLiteral("cos"), false},
        {QStringLiteral("tan"), false},  {QStringLiteral("cot"), false},
        {QStringLiteral("sec"), false},  {QStringLiteral("csc"), false},
        {QStringLiteral("arcsin"), false},{QStringLiteral("arccos"), false},
        {QStringLiteral("arctan"), false},{QStringLiteral("sinh"), false},
        {QStringLiteral("cosh"), false}, {QStringLiteral("tanh"), false},
        {QStringLiteral("deg"), false},  {QStringLiteral("dim"), false},
        {QStringLiteral("ker"), false},  {QStringLiteral("hom"), false},
        {QStringLiteral("arg"), false},  {QStringLiteral("mod"), false},
        {QStringLiteral("bmod"), false}, {QStringLiteral("Pr"), true},
    };
    const auto it = funcs.constFind(name);
    if (it == funcs.constEnd()) return false;
    limits = it.value();
    return true;
}

bool LargeOperator(const QString& name, QString& glyph, bool& limits, qreal& size) {
    if (name == QLatin1String("sum")) { glyph = U(0x2211); limits = true; size = 1.55; return true; }
    if (name == QLatin1String("prod")) { glyph = U(0x220F); limits = true; size = 1.55; return true; }
    if (name == QLatin1String("coprod")) { glyph = U(0x2210); limits = true; size = 1.55; return true; }
    if (name == QLatin1String("int")) { glyph = U(0x222B); limits = false; size = 1.7; return true; }
    if (name == QLatin1String("oint")) { glyph = U(0x222E); limits = false; size = 1.7; return true; }
    if (name == QLatin1String("iint")) { glyph = U(0x222C); limits = false; size = 1.7; return true; }
    if (name == QLatin1String("iiint")) { glyph = U(0x222D); limits = false; size = 1.7; return true; }
    if (name == QLatin1String("bigcup")) { glyph = U(0x22C3); limits = true; size = 1.55; return true; }
    if (name == QLatin1String("bigcap")) { glyph = U(0x22C2); limits = true; size = 1.55; return true; }
    return false;
}

bool SupportedEnvironment(const QString& env) {
    static const QSet<QString> envs = {
        QStringLiteral("aligned"), QStringLiteral("align"),  QStringLiteral("align*"),
        QStringLiteral("gathered"),QStringLiteral("gather"), QStringLiteral("gather*"),
        QStringLiteral("cases"),   QStringLiteral("dcases"), QStringLiteral("matrix"),
        QStringLiteral("smallmatrix"), QStringLiteral("pmatrix"), QStringLiteral("bmatrix"),
        QStringLiteral("Bmatrix"), QStringLiteral("vmatrix"), QStringLiteral("Vmatrix"),
    };
    return envs.contains(env);
}

// ---------------------------------------------------------------------------
// Source scanning helpers (operate on a whole string, nesting aware)
// ---------------------------------------------------------------------------

// Split `s` into [start, end) ranges on top-level `&` (onAmp) or `\\`.
// Group braces and nested \begin/\end environments are respected.
QVector<QPair<int, int>> SplitTopLevel(const QString& s, bool onAmp) {
    QVector<QPair<int, int>> out;
    int start = 0;
    int brace = 0;
    int env = 0;
    int j = 0;
    const int n = s.size();
    while (j < n) {
        const QChar c = s[j];
        if (c == u'\\') {
            if (j + 1 < n && s[j + 1] == u'\\') {
                if (!onAmp && brace == 0 && env == 0) {
                    out.push_back({start, j});
                    start = j + 2;
                }
                j += 2;
                continue;
            }
            int k = j + 1;
            while (k < n && s[k].isLetter()) ++k;
            const QString word = s.mid(j + 1, k - j - 1);
            if (word == QLatin1String("begin")) ++env;
            else if (word == QLatin1String("end") && env > 0) --env;
            j = (k > j + 1) ? k : std::min(n, j + 2);
            continue;
        }
        if (c == u'{') ++brace;
        else if (c == u'}') { if (brace > 0) --brace; }
        else if (onAmp && c == u'&' && brace == 0 && env == 0) {
            out.push_back({start, j});
            start = j + 1;
        }
        ++j;
    }
    out.push_back({start, n});
    return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(QString src, ParseState& st, int depth, const FontSpec& spec)
        : s_(std::move(src)), st_(st), depth_(depth), spec_(spec) {}

    BoxPtr Parse(bool stopAtBrace, bool stopAtRight) {
        std::vector<BoxPtr> items;
        while (i_ < s_.size()) {
            const QChar c = s_[i_];
            if (c == u'}') {
                if (stopAtBrace) break;
                st_.Fail(QStringLiteral("unbalanced brace"));
                items.push_back(MakePlainText(QStringLiteral("}")));
                ++i_;
                continue;
            }
            if (c == u'\\' && NextIsRightCommand()) {
                if (stopAtRight) {
                    stopped_at_right_ = true;
                    break;
                }
            }
            if (c.isSpace()) {  // whitespace collapses to a single gap
                while (i_ < s_.size() && s_[i_].isSpace()) ++i_;
                const bool nextIsScript =
                    (i_ < s_.size() && (s_[i_] == u'^' || s_[i_] == u'_'));
                const bool nextIsRight = (i_ < s_.size() && s_[i_] == u'\\' && NextIsRightCommand());
                const bool alreadySpaced = (!items.empty() && items.back()->kind == Kind::Space);
                if (!items.empty() && i_ < s_.size() && s_[i_] != u'}' && !nextIsScript &&
                    !nextIsRight && !alreadySpaced) {
                    items.push_back(MakeSpace(CurrentFontPx() * 0.30));
                }
                continue;
            }
            if (c == u'^' || c == u'_') {
                const bool isSup = (c == u'^');
                ++i_;
                if (i_ >= s_.size()) {  // dangling marker renders literally
                    st_.Fail(QStringLiteral("missing script argument"));
                    items.push_back(MakePlainText(QString(c), false));
                    continue;
                }
                const BoxPtr script = ParseScriptArgument();
                if (items.empty()) {
                    items.push_back(MakePlainText(QString(c), false));
                }
                items.back() = AttachScript(items.back(), isSup, script, CurrentFontPx());
                continue;
            }
            const int before = i_;
            const BoxPtr atom = ParseAtom();
            if (atom) items.push_back(atom);
            if (i_ == before) ++i_;  // absolute progress guarantee
        }
        return MakeHBox(items);
    }

    void SetScale(qreal s) { scale_ = s; }
    qreal Scale() const { return scale_; }

private:
    struct ScaleGuard {
        Parser& p;
        qreal old;
        ScaleGuard(Parser& parser, qreal factor) : p(parser), old(parser.scale_) {
            p.scale_ = old * factor;
        }
        ~ScaleGuard() { p.scale_ = old; }
    };

    // Bounds recursion through command arguments: \frac\frac\frac..., \hat\hat...,
    // \left(\left(... have no braces, so the group guard alone cannot stop them.
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) { ++p.depth_; }
        ~DepthGuard() { --p.depth_; }
    };

    qreal CurrentFontPx() const {
        const qreal eff = std::clamp(scale_, kMinScale, 3.0);
        return std::max(4.0, st_.style->font_px * eff);
    }

    BoxPtr MakePlainText(const QString& text, bool autoItalic = true) {
        FontSpec sp = spec_;
        if (!autoItalic) sp.italic = false;
        return MakeText(text, FontFor(st_, sp, scale_), st_.color);
    }

    bool NextIsRightCommand() const {
        if (i_ >= s_.size() || s_[i_] != u'\\') return false;
        int k = i_ + 1;
        QString word;
        while (k < s_.size() && s_[k].isLetter()) {
            word.append(s_[k]);
            ++k;
        }
        return word == QLatin1String("right");
    }

    // -- atoms ------------------------------------------------------------

    BoxPtr ParseAtom() {
        if (i_ >= s_.size()) return MakeEmpty();
        const QChar c = s_[i_];
        if (c == u'{') return ParseBracedGroup();
        if (c == u'\\') return ParseCommand();
        if (c == u'^' || c == u'_') {
            ++i_;
            return MakePlainText(QString(c), false);
        }
        return ParsePlainRun();
    }

    BoxPtr ParsePlainRun() {
        const bool letter = IsAsciiLetter(s_[i_]);
        const int start = i_;
        while (i_ < s_.size() && !IsSpecialChar(s_[i_]) && IsAsciiLetter(s_[i_]) == letter) ++i_;
        const QString text = s_.mid(start, i_ - start);
        FontSpec sp = spec_;
        sp.italic = letter && sp.auto_italic;
        return MakeText(text, FontFor(st_, sp, scale_), st_.color);
    }

    BoxPtr ParseBracedGroup() {
        ++i_;  // past '{'
        if (depth_ >= kMaxDepth) {
            st_.Fail(QStringLiteral("nesting too deep"));
            return MakePlainText(CollapseWsTrim(ConsumeRawBalanced()), false);
        }
        ++depth_;
        BoxPtr inner = Parse(true, false);
        --depth_;
        if (i_ < s_.size() && s_[i_] == u'}') ++i_;
        else st_.Fail(QStringLiteral("unbalanced brace"));
        return inner;
    }

    // Reads a balanced {...} body (already past the '{') and consumes the '}'.
    QString ConsumeRawBalanced() {
        int nesting = 1;
        const int start = i_;
        while (i_ < s_.size()) {
            if (s_[i_] == u'{') ++nesting;
            else if (s_[i_] == u'}') {
                --nesting;
                if (nesting == 0) {
                    const QString raw = s_.mid(start, i_ - start);
                    ++i_;
                    return raw;
                }
            }
            ++i_;
        }
        st_.Fail(QStringLiteral("unbalanced brace"));
        return s_.mid(start);
    }

    BoxPtr ParseScriptArgument() {
        ScaleGuard guard(*this, kScriptScale);
        if (i_ >= s_.size()) {
            st_.Fail(QStringLiteral("missing script argument"));
            return MakeEmpty();
        }
        if (s_[i_] == u'{') return ParseBracedGroup();
        if (s_[i_] == u'\\') return ParseCommand();
        const QChar c = s_[i_];
        ++i_;
        FontSpec sp = spec_;
        sp.italic = IsAsciiLetter(c) && sp.auto_italic;
        return MakeText(QString(c), FontFor(st_, sp, scale_), st_.color);
    }

    BoxPtr ParseArgument() {
        if (i_ >= s_.size()) {
            st_.Fail(QStringLiteral("missing argument"));
            return MakeEmpty();
        }
        if (s_[i_] == u'{') return ParseBracedGroup();
        if (s_[i_] == u'\\') return ParseCommand();
        const QChar c = s_[i_];
        ++i_;
        FontSpec sp = spec_;
        sp.italic = IsAsciiLetter(c) && sp.auto_italic;
        return MakeText(QString(c), FontFor(st_, sp, scale_), st_.color);
    }

    // -- commands ---------------------------------------------------------

    BoxPtr ParseCommand() {
        const int backslash = i_;
        if (depth_ >= kMaxDepth) {
            // Too deep: consume the command name and draw it literally.
            st_.Fail(QStringLiteral("nesting too deep"));
            ++i_;
            if (i_ < s_.size() && s_[i_].isLetter()) {
                while (i_ < s_.size() && s_[i_].isLetter()) ++i_;
            } else if (i_ < s_.size()) {
                ++i_;
            }
            return MakePlainText(CollapseWsTrim(s_.mid(backslash, i_ - backslash)), false);
        }
        DepthGuard guard(*this);
        ++i_;  // past '\\'
        if (i_ >= s_.size()) return MakePlainText(QStringLiteral("\\"), false);
        if (s_[i_].isLetter()) {
            int k = i_;
            while (k < s_.size() && s_[k].isLetter()) ++k;
            const QString name = s_.mid(i_, k - i_);
            i_ = k;
            return DispatchWord(name, backslash);
        }
        const QChar c = s_[i_];
        ++i_;
        return DispatchSymbol(c, backslash);
    }

    BoxPtr DispatchSymbol(QChar c, int backslash) {
        switch (c.unicode()) {
            case u'{':
            case u'}':
            case u'_':
            case u'&':
            case u'%':
            case u'#':
            case u'$':
                return MakePlainText(QString(c), false);
            case u',':
                return MakeSpace(0.17 * CurrentFontPx());
            case u';':
                return MakeSpace(0.28 * CurrentFontPx());
            case u'!':
                return MakeSpace(-0.17 * CurrentFontPx());
            case u' ':
                return MakeSpace(0.33 * CurrentFontPx());
            case u'\\':
                return MakeSpace(0.30 * CurrentFontPx());  // line break inside a group
            case u'|':
                return MakePlainText(U(0x2016), false);
            default:
                break;
        }
        st_.Fail(QStringLiteral("unsupported command: \\") + QString(c));
        (void)backslash;
        return MakePlainText(QStringLiteral("\\") + QString(c), false);
    }

    BoxPtr DispatchWord(const QString& name, int backslash) {
        (void)backslash;
        // Spacing words.
        if (name == QLatin1String("quad")) return MakeSpace(CurrentFontPx());
        if (name == QLatin1String("qquad")) return MakeSpace(2.0 * CurrentFontPx());

        // Fractions.
        if (name == QLatin1String("frac") || name == QLatin1String("dfrac") ||
            name == QLatin1String("tfrac")) {
            BoxPtr num, den;
            {
                ScaleGuard guard(*this, kFracScale);
                num = ParseArgument();
            }
            {
                ScaleGuard guard(*this, kFracScale);
                den = ParseArgument();
            }
            return MakeFraction(num, den, CurrentFontPx(), st_.color);
        }

        // Roots.
        if (name == QLatin1String("sqrt")) return ParseSqrt();

        // Delimiters.
        if (name == QLatin1String("left")) return ParseLeft();
        if (name == QLatin1String("right")) {
            st_.Fail(QStringLiteral("unmatched \\right"));
            QChar ch;
            const bool present = ReadDelimiter(ch);
            if (!present) return MakeEmpty();
            return MakeDelimiterFor(QString(ch), 0, 0, CurrentFontPx(), st_.color, st_);
        }

        // Font commands.
        if (name == QLatin1String("mathbf") || name == QLatin1String("boldsymbol") ||
            name == QLatin1String("mathbfit")) {
            return ParseWithFont([&](FontSpec& s) {
                s.bold = true;
                if (name == QLatin1String("mathbf")) {
                    s.force_upright = true;
                    s.auto_italic = false;
                } else {
                    s.italic = true;
                    s.auto_italic = false;
                }
            });
        }
        if (name == QLatin1String("mathcal") || name == QLatin1String("mathscr") ||
            name == QLatin1String("mathfrak")) {
            return ParseWithFont([&](FontSpec& s) {
                s.script = true;
                s.italic = true;
                s.auto_italic = false;
            });
        }
        if (name == QLatin1String("mathrm") || name == QLatin1String("mathup")) {
            return ParseWithFont([&](FontSpec& s) {
                s.force_upright = true;
                s.auto_italic = false;
            });
        }
        if (name == QLatin1String("mathit")) {
            return ParseWithFont([&](FontSpec& s) {
                s.italic = true;
                s.auto_italic = false;
            });
        }
        if (name == QLatin1String("mathsf")) {
            return ParseWithFont([&](FontSpec& s) {
                s.sans = true;
                s.force_upright = true;
                s.auto_italic = false;
            });
        }
        if (name == QLatin1String("mathtt")) {
            return ParseWithFont([&](FontSpec& s) {
                s.mono = true;
                s.force_upright = true;
                s.auto_italic = false;
            });
        }
        if (name == QLatin1String("text") || name == QLatin1String("operatorname") ||
            name == QLatin1String("textrm") || name == QLatin1String("textnormal") ||
            name == QLatin1String("mbox")) {
            return ParseTextArgument(name);
        }

        // Accents.
        if (name == QLatin1String("hat") || name == QLatin1String("widehat"))
            return MakeGlyphAccent(ParseArgument(), U(0x02C6), CurrentFontPx(), st_.color, st_);
        if (name == QLatin1String("vec"))
            return MakeGlyphAccent(ParseArgument(), U(0x2192), CurrentFontPx(), st_.color, st_);
        if (name == QLatin1String("dot"))
            return MakeGlyphAccent(ParseArgument(), U(0x02D9), CurrentFontPx(), st_.color, st_);
        if (name == QLatin1String("ddot"))
            return MakeGlyphAccent(ParseArgument(), U(0x00A8), CurrentFontPx(), st_.color, st_);
        if (name == QLatin1String("tilde") || name == QLatin1String("widetilde"))
            return MakeGlyphAccent(ParseArgument(), U(0x02DC), CurrentFontPx(), st_.color, st_);
        if (name == QLatin1String("bar") || name == QLatin1String("overline"))
            return MakeLineAccent(ParseArgument(), true, CurrentFontPx(), st_.color);
        if (name == QLatin1String("underline"))
            return MakeLineAccent(ParseArgument(), false, CurrentFontPx(), st_.color);

        // Environments.
        if (name == QLatin1String("begin")) return ParseBegin();
        if (name == QLatin1String("end")) {
            st_.Fail(QStringLiteral("unmatched \\end"));
            return MakePlainText(QStringLiteral("\\end"), false);
        }

        // Large operators.
        {
            QString glyph;
            bool limits = false;
            qreal size = 1.0;
            if (LargeOperator(name, glyph, limits, size)) {
                FontSpec sp = spec_;
                sp.italic = false;
                sp.auto_italic = false;
                sp.force_upright = true;
                sp.size = size;
                BoxPtr box = MakeText(glyph, FontFor(st_, sp, scale_), st_.color);
                box->limits_op = limits;
                return box;
            }
        }

        // Upright function names.
        {
            bool limits = false;
            if (FunctionName(name, limits)) {
                FontSpec sp = spec_;
                sp.italic = false;
                sp.auto_italic = false;
                sp.force_upright = true;
                BoxPtr box = MakeText(name, FontFor(st_, sp, scale_), st_.color);
                box->limits_op = limits;
                return box;
            }
        }

        // Unicode symbol table.
        const auto it = SymbolTable().constFind(name);
        if (it != SymbolTable().constEnd()) {
            FontSpec sp = spec_;
            sp.italic = false;
            sp.auto_italic = false;
            return MakeText(it.value(), FontFor(st_, sp, scale_), st_.color);
        }

        // Unknown: literal fallback.
        st_.Fail(QStringLiteral("unsupported command: \\") + name);
        return MakePlainText(QStringLiteral("\\") + name, false);
    }

    template <typename Fn>
    BoxPtr ParseWithFont(Fn&& configure) {
        const FontSpec old = spec_;
        configure(spec_);
        BoxPtr box = ParseArgument();
        spec_ = old;
        return box;
    }

    BoxPtr ParseTextArgument(const QString& name) {
        FontSpec sp = spec_;
        sp.italic = false;
        sp.auto_italic = false;
        sp.force_upright = true;
        if (i_ >= s_.size() || s_[i_] != u'{') {
            if (i_ < s_.size()) {
                const QChar c = s_[i_];
                ++i_;
                return MakeText(QString(c), FontFor(st_, sp, scale_), st_.color);
            }
            st_.Fail(QStringLiteral("missing argument for \\") + name);
            return MakeEmpty();
        }
        ++i_;
        const QString raw = ConsumeRawBalanced();
        return MakeText(CollapseSpaces(raw), FontFor(st_, sp, scale_), st_.color);
    }

    BoxPtr ParseSqrt() {
        BoxPtr index;
        if (i_ < s_.size() && s_[i_] == u'[') {
            ++i_;
            int nesting = 0;
            const int start = i_;
            while (i_ < s_.size() && !(s_[i_] == u']' && nesting == 0)) {
                if (s_[i_] == u'{') ++nesting;
                else if (s_[i_] == u'}') { if (nesting > 0) --nesting; }
                ++i_;
            }
            const QString idxStr = s_.mid(start, i_ - start);
            if (i_ < s_.size() && s_[i_] == u']') ++i_;
            else st_.Fail(QStringLiteral("unbalanced bracket in \\sqrt"));
            Parser p(idxStr, st_, depth_ + 1, spec_);
            p.SetScale(scale_ * 0.62);
            index = p.Parse(false, false);
        }
        const BoxPtr radicand = ParseArgument();
        return MakeRadical(index, radicand, CurrentFontPx(), st_.color);
    }

    bool ReadDelimiter(QChar& out) {
        while (i_ < s_.size() && s_[i_].isSpace()) ++i_;
        if (i_ >= s_.size()) return false;
        if (s_[i_] == u'\\') {
            ++i_;
            if (i_ >= s_.size()) return false;
            if (s_[i_].isLetter()) {
                int k = i_;
                while (k < s_.size() && s_[k].isLetter()) ++k;
                const QString word = s_.mid(i_, k - i_);
                i_ = k;
                if (word == QLatin1String("lvert") || word == QLatin1String("rvert") ||
                    word == QLatin1String("vert")) { out = u'|'; return true; }
                if (word == QLatin1String("lVert") || word == QLatin1String("rVert") ||
                    word == QLatin1String("Vert")) { out = QChar(0x2016); return true; }
                if (word == QLatin1String("langle")) { out = QChar(0x27E8); return true; }
                if (word == QLatin1String("rangle")) { out = QChar(0x27E9); return true; }
                if (word == QLatin1String("lfloor")) { out = QChar(0x230A); return true; }
                if (word == QLatin1String("rfloor")) { out = QChar(0x230B); return true; }
                if (word == QLatin1String("lceil")) { out = QChar(0x2308); return true; }
                if (word == QLatin1String("rceil")) { out = QChar(0x2309); return true; }
                if (word == QLatin1String("lbrace")) { out = u'{'; return true; }
                if (word == QLatin1String("rbrace")) { out = u'}'; return true; }
                if (word == QLatin1String("backslash")) { out = u'\\'; return true; }
                st_.Fail(QStringLiteral("unsupported delimiter: \\") + word);
                return false;
            }
            const QChar c = s_[i_];
            ++i_;
            if (c == u'.') return false;
            out = (c == u'|') ? QChar(0x2016) : c;
            return true;
        }
        const QChar c = s_[i_];
        ++i_;
        if (c == u'.') return false;
        out = c;
        return true;
    }

    BoxPtr ParseLeft() {
        QChar openCh;
        const bool hasOpen = ReadDelimiter(openCh);
        const BoxPtr content = Parse(false, true);
        QChar closeCh;
        bool hasClose = false;
        if (stopped_at_right_) {
            stopped_at_right_ = false;
            i_ += 6;  // "\right"
            hasClose = ReadDelimiter(closeCh);
        } else {
            st_.Fail(QStringLiteral("unmatched \\left"));
        }
        const qreal fpx = CurrentFontPx();
        BoxPtr left = hasOpen ? MakeDelimiterFor(QString(openCh), content->h, content->d, fpx, st_.color, st_)
                              : MakeEmpty();
        BoxPtr right = hasClose ? MakeDelimiterFor(QString(closeCh), content->h, content->d, fpx, st_.color, st_)
                                : MakeEmpty();
        return MakeHBox({left, content, right});
    }

    BoxPtr ParseBegin() {
        if (i_ >= s_.size() || s_[i_] != u'{') {
            st_.Fail(QStringLiteral("malformed \\begin"));
            return MakePlainText(QStringLiteral("\\begin"), false);
        }
        const int beginStart = i_ - 6;  // points at the backslash of "\begin"
        ++i_;  // past '{'
        const int nameStart = i_;
        while (i_ < s_.size() && s_[i_] != u'}') ++i_;
        const QString env = s_.mid(nameStart, i_ - nameStart);
        if (i_ < s_.size()) ++i_;
        else st_.Fail(QStringLiteral("unbalanced brace after \\begin"));

        int bodyEnd = -1;
        int afterEnd = -1;
        FindEnvironmentEnd(env, bodyEnd, afterEnd);
        if (bodyEnd < 0) {
            bodyEnd = s_.size();
            afterEnd = s_.size();
            st_.Fail(QStringLiteral("missing \\end{") + env + QStringLiteral("}"));
        }
        const QString raw = s_.mid(std::max(0, beginStart), afterEnd - std::max(0, beginStart));
        i_ = afterEnd;

        if (!SupportedEnvironment(env)) {
            st_.Fail(QStringLiteral("unsupported environment: ") + env);
            return MakePlainText(CollapseWsTrim(raw), false);
        }
        if (depth_ >= kMaxDepth) {
            st_.Fail(QStringLiteral("nesting too deep"));
            return MakePlainText(CollapseWsTrim(raw), false);
        }
        const int bodyStart = nameStart + env.size() + 1;  // just past "}{"
        return BuildEnvironment(env, s_.mid(bodyStart, std::max(0, bodyEnd - bodyStart)));
    }

    void FindEnvironmentEnd(const QString& env, int& bodyEnd, int& afterEnd) {
        int nesting = 0;
        int j = i_;
        const int n = s_.size();
        while (j < n) {
            if (s_[j] == u'\\') {
                if (j + 1 < n && s_[j + 1] == u'\\') {
                    j += 2;
                    continue;
                }
                int k = j + 1;
                while (k < n && s_[k].isLetter()) ++k;
                const QString word = s_.mid(j + 1, k - j - 1);
                if (word == QLatin1String("begin")) {
                    ++nesting;
                } else if (word == QLatin1String("end")) {
                    if (nesting == 0) {
                        int b = k;
                        while (b < n && s_[b].isSpace()) ++b;
                        if (b < n && s_[b] == u'{') {
                            const int e = s_.indexOf(u'}', b);
                            const int nameEnd = (e < 0) ? n : e;
                            const QString endName = s_.mid(b + 1, nameEnd - (b + 1));
                            if (endName != env && !endName.isEmpty())
                                st_.Fail(QStringLiteral("mismatched \\end{") + endName + QStringLiteral("}"));
                            bodyEnd = j;
                            afterEnd = (e < 0) ? n : e + 1;
                            return;
                        }
                        bodyEnd = j;
                        afterEnd = k;
                        return;
                    }
                    --nesting;
                }
                j = (k > j + 1) ? k : std::min(n, j + 2);
                continue;
            }
            ++j;
        }
        bodyEnd = -1;
        afterEnd = -1;
    }

    BoxPtr BuildEnvironment(const QString& env, const QString& body) {
        const qreal fpx = CurrentFontPx();
        const bool isAligned = (env == QLatin1String("aligned") || env == QLatin1String("align") ||
                                env == QLatin1String("align*") || env == QLatin1String("gathered") ||
                                env == QLatin1String("gather") || env == QLatin1String("gather*"));
        const bool isCases = (env == QLatin1String("cases") || env == QLatin1String("dcases"));
        const bool isMatrix = (env == QLatin1String("matrix") || env == QLatin1String("smallmatrix"));
        const ColAlign align = isAligned ? ColAlign::Right : (isCases ? ColAlign::Left : ColAlign::Center);
        const qreal colGap = isCases ? fpx * 0.9 : fpx * 0.85;
        const qreal rowGap = isMatrix ? fpx * 0.35 : fpx * 0.45;

        const QVector<QPair<int, int>> rowRanges = SplitTopLevel(body, false);
        std::vector<std::vector<BoxPtr>> cells;
        for (const auto& rr : rowRanges) {
            const QString rowStr = body.mid(rr.first, rr.second - rr.first);
            const QVector<QPair<int, int>> colRanges = SplitTopLevel(rowStr, true);
            std::vector<BoxPtr> row;
            bool any = false;
            for (const auto& cc : colRanges) {
                const QString cellStr = rowStr.mid(cc.first, cc.second - cc.first);
                Parser p(cellStr, st_, depth_ + 1, spec_);
                p.SetScale(scale_);
                BoxPtr cell = p.Parse(false, false);
                if (cell && (cell->w > 0 || cell->h > 0 || cell->d > 0)) any = true;
                row.push_back(cell);
            }
            if (any || cells.empty()) cells.push_back(std::move(row));
        }

        const BoxPtr grid = MakeGrid(cells, align, isAligned, colGap, rowGap);
        if (env == QLatin1String("pmatrix"))
            return MakeHBox({MakeDelimiterFor(QStringLiteral("("), grid->h, grid->d, fpx, st_.color, st_), grid,
                             MakeDelimiterFor(QStringLiteral(")"), grid->h, grid->d, fpx, st_.color, st_)});
        if (env == QLatin1String("bmatrix"))
            return MakeHBox({MakeDelimiterFor(QStringLiteral("["), grid->h, grid->d, fpx, st_.color, st_), grid,
                             MakeDelimiterFor(QStringLiteral("]"), grid->h, grid->d, fpx, st_.color, st_)});
        if (env == QLatin1String("Bmatrix"))
            return MakeHBox({MakeDelimiterFor(QStringLiteral("{"), grid->h, grid->d, fpx, st_.color, st_), grid,
                             MakeDelimiterFor(QStringLiteral("}"), grid->h, grid->d, fpx, st_.color, st_)});
        if (env == QLatin1String("vmatrix"))
            return MakeHBox({MakeDelimiterFor(QStringLiteral("|"), grid->h, grid->d, fpx, st_.color, st_), grid,
                             MakeDelimiterFor(QStringLiteral("|"), grid->h, grid->d, fpx, st_.color, st_)});
        if (env == QLatin1String("Vmatrix")) {
            const QString bars = U(0x2016);
            return MakeHBox({MakeDelimiterFor(bars, grid->h, grid->d, fpx, st_.color, st_), grid,
                             MakeDelimiterFor(bars, grid->h, grid->d, fpx, st_.color, st_)});
        }
        if (isCases) {
            BoxPtr brace = MakeDelimiterFor(QStringLiteral("{"), grid->h, grid->d, fpx, st_.color, st_);
            return MakeHBox({brace, MakeSpace(fpx * 0.18), grid});
        }
        return grid;
    }

    QString s_;
    int i_ = 0;
    ParseState& st_;
    int depth_ = 0;
    qreal scale_ = 1.0;
    FontSpec spec_;
    bool stopped_at_right_ = false;
};

// ---------------------------------------------------------------------------
// Rasterisation
// ---------------------------------------------------------------------------

MathRenderResult FinishImage(const BoxPtr& content, const MathRenderStyle& style, bool exact,
                             const QString& note, qreal dprWanted) {
    MathRenderResult res;
    res.exact = exact;
    res.note = note;

    qreal W = content ? content->w : 0.0;
    qreal H = content ? content->h : 0.0;
    qreal D = content ? content->d : 0.0;
    if (!std::isfinite(W) || W < 0) W = 0;
    if (!std::isfinite(H) || H < 0) H = 0;
    if (!std::isfinite(D) || D < 0) D = 0;

    const qreal pad = std::max(2.0, std::round(style.font_px * 0.16));
    int logicalW = static_cast<int>(std::ceil(W + 2.0 * pad));
    int logicalH = static_cast<int>(std::ceil(H + D + 2.0 * pad));
    logicalW = std::clamp(logicalW, 1, 20000);
    logicalH = std::clamp(logicalH, 1, 20000);

    qreal dpr = dprWanted;
    if (!std::isfinite(dpr) || dpr <= 0) dpr = 1.0;
    dpr = std::clamp(dpr, 0.5, 4.0);

    int pxW = std::max(1, static_cast<int>(std::ceil(logicalW * dpr)));
    int pxH = std::max(1, static_cast<int>(std::ceil(logicalH * dpr)));

    QImage img;
    for (int attempt = 0; attempt < 2; ++attempt) {
        img = QImage(pxW, pxH, QImage::Format_ARGB32_Premultiplied);
        if (!img.isNull()) break;
        dpr = 1.0;
        pxW = logicalW;
        pxH = logicalH;
    }
    if (img.isNull()) {
        res.exact = false;
        if (res.note.isEmpty()) res.note = QStringLiteral("image allocation failed");
        return res;
    }
    img.fill(Qt::transparent);

    int baseline = static_cast<int>(std::lround(pad + H));
    baseline = std::clamp(baseline, 1, std::max(1, logicalH - 1));

    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.scale(dpr, dpr);
        p.translate(pad, baseline);
        if (content && content->draw) content->draw(p, 0, 0);
        p.end();
    }

    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(dprWanted > 0 ? dprWanted : 1.0);
    res.pixmap = pm;
    res.width = logicalW;
    res.height = logicalH;
    res.baseline = baseline;
    return res;
}

// A last-resort renderer: one line of literal text, never throws.
MathRenderResult RenderLiteralFallback(const QString& latex, const MathRenderStyle& style,
                                       const QString& note) {
    ParseState st;
    st.style = &style;
    st.color = style.color;
    st.base_font = QGuiApplication::font();
    st.base_font.setPixelSize(std::max(4, style.font_px));
    FontSpec sp;
    sp.auto_italic = false;
    sp.force_upright = true;
    const BoxPtr box = MakeText(CollapseWsTrim(latex), FontFor(st, sp, 1.0), style.color);
    return FinishImage(box, style, false, note, style.device_pixel_ratio);
}

}  // namespace

MathRenderResult RenderMathPreview(const QString& latex, const MathRenderStyle& style) {
    MathRenderResult empty;
    if (latex.trimmed().isEmpty()) return empty;
    if (!QGuiApplication::instance()) {
        MathRenderResult res;
        res.exact = false;
        res.note = QStringLiteral("no QGuiApplication: cannot render");
        return res;
    }
    try {
        ParseState st;
        st.style = &style;
        st.color = style.color;
        st.base_font = QGuiApplication::font();
        st.base_font.setPixelSize(std::max(4, style.font_px));

        const qreal fpx = std::max(4.0, static_cast<qreal>(style.font_px));
        const QVector<QPair<int, int>> lineRanges = SplitTopLevel(latex, false);
        std::vector<std::vector<BoxPtr>> lines;
        for (const auto& lr : lineRanges) {
            const QString lineStr = latex.mid(lr.first, lr.second - lr.first);
            Parser p(lineStr, st, 0, FontSpec{});
            std::vector<BoxPtr> row;
            row.push_back(p.Parse(false, false));
            lines.push_back(std::move(row));
        }
        const BoxPtr content = MakeGrid(lines, ColAlign::Left, false, 0.0, fpx * 0.45);
        return FinishImage(content, style, st.exact, st.note, style.device_pixel_ratio);
    } catch (...) {
        return RenderLiteralFallback(latex, style, QStringLiteral("internal error: literal fallback"));
    }
}

}  // namespace pf::gui
