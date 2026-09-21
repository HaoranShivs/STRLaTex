#pragma once
// Toolchain 值类型（方案 §6、§7、§14）。
//
// 刻意不 include render/template/build 中的头文件，使 TemplateRegistry 与
// Compiler 都能持有这些类型而不形成循环包含：renderer 依赖 template，
// template 声明 toolchain，compiler 消费它。

#include <cstdint>
#include <string>
#include <vector>

namespace pf {

// 由哪个 LaTeX 引擎编译文档。这由模板作者决定
// （例如 IEEEtran 专为 pdfLaTeX 设计，在 XeTeX 引擎下会丢失字体设置）；
// 下游不会从文档中重新推导该信息。
enum class LatexEngine : std::uint8_t {
    PdfLatex,
    XeLatex,
    LuaLatex,
};

const char* ToString(LatexEngine engine);

enum class BibliographyEngine : std::uint8_t {
    None,
    BibTex,
    Biber,
};

const char* ToString(BibliographyEngine engine);

// build 所需的内容，由 session 根据模板解析得出，并随每个请求一同传递
// （方案 §14）。
struct BuildToolchain {
    LatexEngine engine = LatexEngine::PdfLatex;
    BibliographyEngine bibliography_engine = BibliographyEngine::None;

    bool operator==(const BuildToolchain&) const = default;
};

// 模板声明该需求的方式（方案 §7）。
struct TemplateToolchainRequirement {
    LatexEngine engine = LatexEngine::PdfLatex;
    BibliographyEngine bibliography_engine = BibliographyEngine::None;
    std::vector<std::string> required_packages;
};

}  // namespace pf
