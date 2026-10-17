#pragma once
// QString <-> std::string 转换的唯一实现（GUI 侧）。
//
// 这些辅助函数此前在 MainWindow / BlockEditor / OutlinePanel / ProblemsPanel /
// ProjectController 五个 TU 中各自复制了一份，语义必须完全一致，否则同一段
// 文本在不同面板里会出现不同的编码处理。

#include <QString>

#include <string>

namespace pf::gui {

inline QString ToQ(const std::string& s) {
    return QString::fromStdString(s);
}

inline std::string ToStd(const QString& s) {
    return s.toStdString();
}

} // namespace pf::gui
