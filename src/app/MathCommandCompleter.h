#pragma once
// MathCommandCompleter：在数学源码框里输入 `\` 时提供命令补全（WP-02）。
//
// 复用 MathCommandCatalog（命令、说明、宏包），因此补全列表与数学面板
// 显示同一组命令；缺包命令会带上 ⚠ 提示，但仍可选择（用户可能随后
// 切换模板或手动加载宏包）。

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <string>
#include <vector>

class QCompleter;
class QPlainTextEdit;
class QStringListModel;

namespace pf::gui {

class MathCommandCompleter : public QObject {
    Q_OBJECT

  public:
    explicit MathCommandCompleter(QObject* parent = nullptr);
    ~MathCommandCompleter() override;

    // 监听 target 的文本/光标变化，输入 `\ma` 后弹出补全。
    void Attach(QPlainTextEdit* target);
    void SetAvailablePackages(std::vector<std::string> packages);

    // 测试接缝：widget 级测试无法可靠地模拟 QCompleter 的弹出窗口焦点。
    bool IsCompletionVisibleForTest() const;
    QWidget* PopupForTest() const;
    QString CurrentCompletionForTest() const;
    bool AcceptCurrentForTest();

  signals:
    void CompletionAccepted(const QString& command);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void RefreshCompletions();
    void AcceptCompletion(const QString& completion);
    void InsertCommandAtCursor(const std::string& command);
    QString CommandFromCompletion(const QString& completion) const;
    QPointer<QPlainTextEdit> target_;
    QCompleter* completer_ = nullptr;
    QStringListModel* model_ = nullptr;
    std::vector<std::string> packages_;
};

} // namespace pf::gui
