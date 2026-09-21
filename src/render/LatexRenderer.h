#pragma once
// LatexRenderer：把语义化的 Document 转换为 LaTeX 源码（架构 20）。
// 产出 BuildPackage + SourceMap；不涉及编译器。

#include <map>
#include <string>
#include <vector>

#include "core/Diagnostic.h"
#include "document/Document.h"
#include "render/SourceMap.h"
#include "template/TemplateRegistry.h"

namespace pf {

struct BuildPackageFile {
  std::string path; // 相对路径，例如 "main.tex"
  std::string content;
  bool executable = false;
};

struct BuildPackage {
  std::string entry_file = "main.tex";
  std::vector<BuildPackageFile> files;      // main.tex 及附加文件
  std::vector<std::string> required_assets; // 被引用的 asset 相对路径
  std::vector<std::string> bibliography_files; // 例如 "references.bib"
  std::vector<std::string> template_files;
};

struct RenderRequest {
  std::string build_id;
  std::string snapshot_id;
  ProjectRevision revision;
  const Document *document = nullptr;
  std::string template_id;
  // Asset 解析：asset id -> build 工作区中的文件名。
  std::map<std::string, std::string> asset_files;
  // 参考文献：原始 .bib 内容（为空表示没有）。
  std::string bibliography_bibtex;
};

struct RenderResult {
  enum class Status { Ok, Failed };
  Status status = Status::Ok;
  std::string build_id;
  ProjectRevision revision;
  BuildPackage package;
  SourceMap source_map;
  std::vector<Diagnostic> diagnostics;
};

class LatexRenderer {
public:
  RenderResult Render(const RenderRequest &request) const;

private:
  static std::string EscapeLatex(const std::string &text);
  void RenderInline(const InlineContent &content, std::string *out) const;
  void RenderBlock(const Block &block, std::string *out, SourceMap *smap) const;

  // 节点的有效 LaTeX 标签：用户设置了标签时用用户标签，否则用节点 id。
  // FillLabelMap() 在 Render() 开头从公式块填充该映射，
  // 以保证 \ref 始终指向实际生成的 \label。
  std::string LabelFor(const NodeId &node) const;
  void FillLabelMap(const Document &doc) const;
  mutable std::map<std::string, std::string> label_map_;

  // Asset 解析（架构 17）：asset id -> package 内 assets/ 目录中的文件名。
  // 在 Render() 开头从 RenderRequest 填充，使 \includegraphics 指向导入的文件
  // （使用其真实扩展名），而不是合成的占位名。
  std::string AssetPathFor(const AssetId &id) const;
  mutable std::map<std::string, std::string> asset_files_;
};

} // namespace pf
