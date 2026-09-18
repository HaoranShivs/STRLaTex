#pragma once
// P0-07: asynchronous math rendering.
//
// Rendering a formula with real TeX used to run synchronously on the GUI
// thread (QProcess::waitForStarted/waitForFinished inside RenderMathPreview),
// so a slow compile froze the window for up to ~28 s, and the reported inline
// math lifetime crash was reproducible by editing under that stall.
//
// MathRenderService owns a worker thread and a bounded LRU cache:
//   * the GUI thread only enqueues a request and applies a reply;
//   * the QProcess lives and waits on the worker thread;
//   * every request carries a generation, so a late reply for an old formula
//     can never overwrite a newer one;
//   * replies are matched against a registered client held through QPointer,
//     so a deleted editor is never touched.
//
// QPixmap construction is GUI-thread-only, so the worker produces a QImage and
// the reply is converted on the GUI thread (PixmapFromMathResult).

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
    QString editor_id;   // identifies the requesting widget
    QString formula_id;  // identifies the formula inside that widget
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

    // GUI thread: associate an id with a live widget. The pointer is held as
    // a QPointer, so a destroyed widget silently discards its replies.
    void RegisterClient(const QString& editor_id, QObject* client);
    void UnregisterClient(const QString& editor_id);

    // GUI thread: queue a render; returns the generation assigned to it.
    std::uint64_t Request(const QString& editor_id, const QString& formula_id,
                          const QString& latex, const MathRenderStyle& style);

    // GUI thread: the newest generation issued for this formula (0 = none).
    std::uint64_t Generation(const QString& editor_id,
                             const QString& formula_id) const;

signals:
    // Emitted on the GUI thread for a reply whose editor is still alive and
    // whose generation is still current. `result` carries a QImage; convert
    // with PixmapFromMathResult().
    void mathRendered(const pf::gui::MathRenderResponse& response);

public:
    // Telemetry for the regression tests.
    std::size_t renders_completed() const { return renders_completed_.load(); }
    std::size_t stale_replies_dropped() const {
        return stale_replies_dropped_.load();
    }
    // True when the worker runs the render (never the calling thread).
    bool IsWorkerThread() const;

    // Process-wide instance used by the widgets that render math.
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

// Convert a worker result to a QPixmap. MUST be called on the GUI thread.
QPixmap PixmapFromMathResult(const MathRenderResult& result);

// Bounded LRU cache for rendered formulas: fixed capacity, per-entry eviction
// of the least-recently-used item, never a whole-cache clear.
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