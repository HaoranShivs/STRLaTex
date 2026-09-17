#pragma once
// LatexRenderer: semantic Document -> LaTeX source (architecture section 20).
// Produces BuildPackage + SourceMap; knows nothing about the compiler.

#include <map>
#include <string>
#include <vector>

#include "core/Diagnostic.h"
#include "document/Document.h"
#include "render/SourceMap.h"
#include "template/TemplateRegistry.h"

namespace pf {

struct BuildPackageFile {
  std::string path; // relative, e.g. "main.tex"
  std::string content;
  bool executable = false;
};

struct BuildPackage {
  std::string entry_file = "main.tex";
  std::vector<BuildPackageFile> files;      // main.tex + any extras
  std::vector<std::string> required_assets; // asset relative paths referenced
  std::vector<std::string> bibliography_files; // e.g. "references.bib"
  std::vector<std::string> template_files;
};

struct RenderRequest {
  std::string build_id;
  std::string snapshot_id;
  ProjectRevision revision;
  const Document *document = nullptr;
  std::string template_id;
  // Asset resolution: asset id -> file name in build workspace.
  std::map<std::string, std::string> asset_files;
  // Bibliography: raw .bib content (empty = none).
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

  // Effective LaTeX label of a node: the user label when set, the node id
  // otherwise. FillLabelMap() populates it from the equation blocks at the
  // start of Render() so a \ref always points at the emitted \label.
  std::string LabelFor(const NodeId &node) const;
  void FillLabelMap(const Document &doc) const;
  mutable std::map<std::string, std::string> label_map_;

  // Asset resolution (architecture §17): asset id -> file name inside the
  // package's assets/ directory. Populated from the RenderRequest at the
  // start of Render() so \includegraphics points at the imported file
  // (with its real extension) instead of a synthetic placeholder name.
  std::string AssetPathFor(const AssetId &id) const;
  mutable std::map<std::string, std::string> asset_files_;
};

} // namespace pf
