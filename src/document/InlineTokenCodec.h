#pragma once
// Citation token 编解码（citation picker v2）。
//
// `InlineEditor` 把一个 citation 存成单个 object-replacement 字符，语义载荷放在
// 该字符的 char format 里。载荷必须同时携带 **keys 与 mode**，否则重新编辑段落
// 时 ContentInRange 只能按默认 Parenthetical 重建，Narrative 会静默丢失。
//
// 线格式（向后兼容）：
//
//   "key1,key2"   旧格式：等价于 Parenthetical（p|key1,key2）
//   "p|key1,key2" 显式 Parenthetical
//   "n|key1,key2" 显式 Narrative
//
// 逗号是 key 分隔符；BibTeX key 不允许逗号（见 BibTeX ValidKey）。因此只在
// 第一个 '|' 前识别前缀，未知前缀整体仍按旧格式解析——旧 token 永远不会因为
// 编解码器升级而消失。
//
// 本文件是纯 C++（Qt-free），可在 domain 测试中直接单测。

#include <optional>
#include <string>

#include "document/Document.h"

namespace pf {

// 编码一个 citation token。key 会去除首尾空白、丢弃空 key 并去重（保持首次
// 出现顺序）。调用方应保证 keys 非空；空 keys 返回空字符串。
std::string EncodeCitationToken(const Citation& citation);

// 解码一个 token payload。空载荷或没有任何有效 key 时返回 nullopt。
// 旧格式（无前缀）按 Parenthetical 解析。
std::optional<Citation> DecodeCitationToken(const std::string& payload);

} // namespace pf
