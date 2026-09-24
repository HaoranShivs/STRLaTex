#pragma once
// EditorItemKind：GUI 如何对编辑器中的一行进行分类（方案 §2.2）。
//
// 领域模型始终保留 Structure/Block 的区分
// （Section/Subsection/Subsubsection 是结构节点；Paragraph/Figure/
// Table/EquationBlock 是块）。GUI 过去把二者压平为同一个 "kind" 字符串，
// 这正是标题行与正文行难以区分、插入菜单无法按当前所处位置过滤的原因。

#include <QString>

#include "document/Document.h"

namespace pf::gui {

enum class EditorItemKind {
    // 前置部分
    PaperTitle,
    Authors,
    Affiliations,
    Abstract,
    Keywords,

    // 结构
    SectionTitle,
    SubsectionTitle,
    SubsubsectionTitle,

    // 内容
    Text,
    Equation,
    Figure,
    Table,
};

// 面向用户的标签，例如 "Section Title"、"Text"。
QString EditorItemLabel(EditorItemKind kind);

// 插入菜单 / "/" 命令载荷使用的稳定机器名。
QString EditorItemKindName(EditorItemKind kind);

// 反向解析。名称未知时返回 false。
bool EditorItemKindFromName(const QString& name, EditorItemKind* out);

// 对文档节点进行分类。前置部分有自己的 kind，并非派生
// 自 NodeKind。
EditorItemKind EditorItemKindOfNode(NodeKind kind);

} // namespace pf::gui
