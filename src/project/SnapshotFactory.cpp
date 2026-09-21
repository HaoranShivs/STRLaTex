#include "project/SnapshotFactory.h"

#include "core/IdGenerator.h"
#include "core/ProjectPath.h"
#include "persistence/ProjectPersistence.h"

namespace pf {

BuildSnapshot SnapshotFactory::CreateBuildSnapshot(
    const ProjectState &state, const std::string &bibliography_bibtex) const {
  BuildSnapshot snapshot;
  snapshot.project_id = state.id();
  snapshot.snapshot_id = IdGenerator::NewSnapshotId();
  snapshot.build_id = BuildId(IdGenerator::NewBuildId());
  snapshot.revision = state.revision();
  // document 的不可变深拷贝。
  snapshot.document = std::make_shared<const Document>(state.document());
  snapshot.template_id = state.template_selection();
  snapshot.bibliography_bibtex = bibliography_bibtex;
  // Asset manifest：asset id -> 相对路径（由 compiler workspace 暂存阶段解析）。
  // 暂存目标在包的 assets/ 目录下保留源文件名与扩展名，这正是渲染器在
  // \includegraphics 中输出的内容，也是 pdfLaTeX 识别图形格式所必需的。
  //
  // P0-04（第二道检查）：每个存储的相对路径都会在此处、在它被转换为文件系统源
  // 之前立即重新校验。路径逃逸出项目的 asset（project.paper 格式错误或被手工
  // 修改，或注册表项在内存中被篡改）会被跳过——渲染器随后报告缺失 asset 状态，
  // 而不是把任意文件复制进 build workspace。
  for (const auto &[id, meta] : assets_->registry().All()) {
    auto safe = ResolveUntrustedProjectPath(assets_->assets_dir(),
                                            meta.relative_path);
    if (!safe.ok())
      continue;
    snapshot.asset_files[id.value()] = meta.relative_path;
    snapshot.asset_sources["assets/" + meta.relative_path] = safe.value();
  }
  return snapshot;
}

SaveSnapshot
SnapshotFactory::CreateSaveSnapshot(const ProjectState &state) const {
  SaveSnapshot data;
  data.project_id = state.id();
  data.revision = state.revision();
  data.serialized.project_id = state.id().value();
  data.serialized.revision = state.revision();
  data.serialized.template_id = state.template_selection();
  data.serialized.document = state.document();
  data.serialized.bibliography_path = state.settings().bibliography_path;
  for (const auto &[id, meta] : assets_->registry().All()) {
    data.serialized.assets.push_back(meta);
  }
  return data;
}

} // namespace pf
