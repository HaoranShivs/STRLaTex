#pragma once
// P0-07：异步数学渲染。
//
// 以前使用真实TeX渲染formula是在GUI线程上同步执行的
// （在RenderMathPreview内部调用QProcess::waitForStarted/waitForFinished），
// 因此一次缓慢的编译会让窗口冻结长达约28秒，而被报告的inline math生命周期
// 崩溃也能在这种卡顿下编辑时稳定复现。
//
// MathRenderService拥有一个worker线程和一个有界LRU缓存：
//   * GUI线程只负责将请求入队并套用回复；
//   * QProcess在worker线程上存活并等待；
//   * 每个请求都携带generation，因此针对旧formula的迟到回复
//     绝不会覆盖更新的结果；
//   * 回复会与通过QPointer持有的已注册client进行匹配，
//     因此已删除的editor绝不会被触碰。
//
// QPixmap的构造仅限GUI线程，所以worker产出QImage，
// 回复在GUI线程上完成转换（PixmapFromMathResult）。

#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include "app/MathPreviewRenderer.h"

namespace pf::gui {

struct MathRenderRequest {
    QString editor_id;   // 标识发起请求的widget
    QString formula_id;  // 标识该widget内部的formula
    std::uint64_t generation = 0;
    QString latex;
    MathRenderStyle style;
};

struct MathRenderResponse {
    QString editor_id;
    QString formula_id;
    std::uint64_t generation = 0;
    QString latex;
    MathRenderResult result;
};

class MathRenderService : public QObject {
    Q_OBJECT

public:
    explicit MathRenderService(QObject* parent = nullptr);
    ~MathRenderService() override;

    // GUI线程：把id关联到一个存活的widget。指针以QPointer方式持有，
    // 因此已销毁的widget会静默丢弃其回复。
    void RegisterClient(const QString& editor_id, QObject* client);
    void UnregisterClient(const QString& editor_id);

    // GUI线程：将一次渲染入队；返回为其分配的generation。
    std::uint64_t Request(const QString& editor_id, const QString& formula_id,
                          const QString& latex, const MathRenderStyle& style);

    // GUI线程：为该formula签发的最新generation（0表示无）。
    std::uint64_t Generation(const QString& editor_id,
                             const QString& formula_id) const;

signals:
    // 当回复对应的editor仍然存活且其generation仍然最新时，在GUI线程上发出。
    // `result`携带QImage；请用PixmapFromMathResult()转换。
    void mathRendered(const pf::gui::MathRenderResponse& response);

public:
    // 供回归测试使用的遥测数据。
    std::size_t renders_completed() const { return renders_completed_.load(); }
    std::size_t stale_replies_dropped() const {
        return stale_replies_dropped_.load();
    }
    // 当由worker执行渲染时返回true（绝不会是调用线程）。
    bool IsWorkerThread() const;

    // 进程级实例，供渲染数学公式的widget使用。
    static MathRenderService* Shared();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<std::size_t> renders_completed_{0};
    std::atomic<std::size_t> stale_replies_dropped_{0};

    void RenderJob(const MathRenderRequest& request);
    void ApplyReply(const MathRenderResponse& response);
    friend struct MathRenderServiceAccess;
};

// 把worker结果转换为QPixmap。必须在GUI线程上调用。
QPixmap PixmapFromMathResult(const MathRenderResult& result);

// 用于已渲染formula的有界LRU缓存：容量固定，按条目淘汰
// 最近最少使用的项，绝不整块清空缓存。
class MathRenderCache {
public:
    explicit MathRenderCache(std::size_t capacity = 256);
    bool Find(const QString& key, MathRenderResult* out);
    void Insert(const QString& key, MathRenderResult value);
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return entries_.size(); }
    void ClearForTest();

private:
    struct Entry {
        MathRenderResult value;
        std::uint64_t stamp = 0;
    };
    std::size_t capacity_;
    std::uint64_t clock_ = 0;
    std::map<QString, Entry> entries_;
};

}  // namespace pf::gui

Q_DECLARE_METATYPE(pf::gui::MathRenderResponse)