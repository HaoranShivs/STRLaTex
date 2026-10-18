#pragma once
// InlineContent 工具函数：纯文本提取、拼接、拆分。
#include <string>
#include <vector>

#include "document/Document.h"

namespace pf {

// 从内联内容中提取纯文本（用于搜索/大纲）。
std::string InlineToPlainText(const InlineContent& content);

// 检查内联内容是否为空或仅含空白字符。
bool InlineIsBlank(const InlineContent& content);

// 构造只含一个 TextRun 的内联内容。
InlineContent InlineFromText(std::string text, std::uint8_t marks = 0);

// 解析由 InlineToPlainText 生成的稳定编辑器表示形式。
// 识别 [cite:key,key2] / [citet:key,key2] 与 [ref:node-id]，在用户编辑
// 周边段落文本时保留语义内联节点（citet = Narrative）。
InlineContent InlineFromEditorText(std::string text);

// ---- 富文本（阶段 B）----

// 同时携带字符标记的编辑器表示形式。token 保持可读（[cite:key] /
// [citet:key] / [ref:node-id]），因此字符串依然可做 diff，旧的纯文本
// 路径也继续可用；标记通过下列分隔符附加到文本上，这些分隔符不会出现在
// 正常行文中：
//   **粗体**、*斜体*、***粗斜体***
// 公式变为 $math$。往返：InlineFromRichText(InlineToRichText(x))
// 会保留每个 run 的标记以及每个语义节点。
std::string InlineToRichText(const InlineContent& content);

// InlineToRichText 的逆操作。无法识别的标记会按字面文本保留，因此
// document 绝不会因解析失败而丢失内容。
InlineContent InlineFromRichText(const std::string& text);

// 当内容带有任何字符标记、公式、引文、引用或脚注时为真——即任何会被
// 纯文本字段抹平的东西。
bool InlineIsRich(const InlineContent& content);

// 版本化的行内剪贴板编码（WP-06）：保留 Footnote 的 body，而
// InlineToRichText 只输出可读占位符。v2 使用 `PFINLINE2` 头部；
// InlineFromClipboard 对损坏/未知版本返回 nullopt。
std::string InlineToClipboard(const InlineContent& content);
std::optional<InlineContent> InlineFromClipboard(const std::string& text);

// 还原从 PDF 或其他文字处理软件复制出来的文本中的硬换行。这类粘贴内容
// 在到达时已按固定列宽预先折行，导致段落永远无法按编辑器宽度重新排版。
//
// 单个换行在 document 模型中不携带含义（LaTeX 将其视为空格），因此单个
// 换行变为空格，空行仍保留为段落分隔。看起来有结构的文本——列表项、
// 显式的 LaTeX 换行或预格式化块——原样返回。
std::string ReflowHardWrappedText(std::string_view text);

} // namespace pf
