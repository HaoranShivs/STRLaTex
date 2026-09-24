#pragma once
// CompilerFactory：唯一把模板的 toolchain 需求转换为编译器实例的地方
// （方案 §13）。
//
// 模板决定 build 需要什么，工厂决定由哪个后端来提供。下游任何地方都不会
// 从文档重新推导引擎。

#include <memory>
#include <string>

#include "build/Compiler.h"
#include "build/RuntimeManager.h"
#include "template/TemplateRegistry.h"

namespace pf {

class CompilerFactory {
  public:
    // texlive_root 是随附的便携版运行时；为空表示运行时不可用，生产后端会
    // 以运行时错误拒绝 build（绝不会是文档错误）。
    explicit CompilerFactory(std::filesystem::path texlive_root) : texlive_root_(std::move(texlive_root)) {}

    // 构建模板所请求的编译器（方案 §31）：通过随附的 TeX Live 运行时执行
    // pdfLaTeX。XeLaTeX/LuaLaTeX 属于协议的一部分，但尚无随附模板选用；
    // 在运行时具备这些引擎之前，它们会回退到同一个 pdfLaTeX 后端，并带上
    // 模板请求的 engine 标志。
    std::unique_ptr<ICompiler> Create(const TemplateDefinition& tpl) const;

    // build 请求应当携带的 toolchain（方案 §14）。
    BuildToolchain ToolchainFor(const TemplateDefinition& tpl) const;

  private:
    std::filesystem::path texlive_root_;
};

} // namespace pf
