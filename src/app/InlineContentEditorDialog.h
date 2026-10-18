#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include <map>
#include <memory>

#include "document/Document.h"
#include "numbering/CitationNumberResolver.h"

class QAction;
class QToolBar;

namespace pf::gui {

class InlineEditor;

// 草稿在确认前只保存在 InlineEditor；取消不会发出任何文档命令。
class InlineContentEditorDialog : public QDialog {
    Q_OBJECT

  public:
    explicit InlineContentEditorDialog(const InlineContent& initial,
                                       QWidget* parent = nullptr);

    void SetCitationResolver(
        std::shared_ptr<const pf::CitationNumberResolver> resolver);
    void SetCrossReferenceLabels(std::map<QString, QString> labels);
    void SetAllowedActions(const QList<QAction*>& actions);
    void SetPlaceholder(const QString& text);
    InlineContent content() const;
    InlineEditor* Editor() const {
        return editor_;
    }

  private:
    InlineEditor* editor_ = nullptr;
    QToolBar* toolbar_ = nullptr;
};

} // namespace pf::gui
