#include "app/math/MathRenderService.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace pf::gui {

// ---------------- Bounded LRU cache ----------------

MathRenderCache::MathRenderCache(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

bool MathRenderCache::Find(const QString& key, MathRenderResult* out) {
    auto it = entries_.find(key);
    if (it == entries_.end())
        return false;
    // A hit refreshes the stamp, making this entry the most recently used.
    it->second.stamp = ++clock_;
    if (out)
        *out = it->second.value;
    return true;
}

void MathRenderCache::Insert(const QString& key, MathRenderResult value) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.value = std::move(value);
        it->second.stamp = ++clock_;
        return;
    }
    if (entries_.size() >= capacity_) {
        // Evict exactly the least-recently-used entry. The previous
        // implementation cleared all 256 entries on overflow, discarding every
        // warm preview because of a single miss.
        auto victim = entries_.begin();
        for (auto candidate = entries_.begin(); candidate != entries_.end();
             ++candidate) {
            if (candidate->second.stamp < victim->second.stamp)
                victim = candidate;
        }
        entries_.erase(victim);
    }
    Entry entry;
    entry.value = std::move(value);
    entry.stamp = ++clock_;
    entries_.emplace(key, entry);
}

void MathRenderCache::ClearForTest() { entries_.clear(); }

QPixmap PixmapFromMathResult(const MathRenderResult& result) {
    // GUI thread only: QPixmap construction is not thread-safe.
    if (!result.pixmap.isNull())
        return result.pixmap;
    if (result.image.isNull())
        return {};
    QPixmap pixmap = QPixmap::fromImage(result.image);
    const qreal dpr =
        result.device_pixel_ratio > 0 ? result.device_pixel_ratio : 1.0;
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

// ---------------- Service ----------------

namespace {

QString CacheKeyFor(const QString& latex, const MathRenderStyle& style) {
    return latex + QChar(0x1f) + QString::number(style.font_px) + QChar(0x1f) +
           style.color.name(QColor::HexArgb) + QChar(0x1f) +
           QString::number(style.device_pixel_ratio, 'f', 2) + QChar(0x1f) +
           style.font_family + QChar(0x1f) + style.template_id + QChar(0x1f) +
           QString::number(static_cast<int>(style.backend)) + QChar(0x1f) +
           QStringLiteral("v1");  // renderer backend version
}

QString FormulaKey(const QString& editor, const QString& formula) {
    return editor + QChar(0x1f) + formula;
}

}  // namespace

struct MathRenderService::Impl {
    struct Job {
        MathRenderRequest request;
    };

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> queue;
    bool stopping = false;
    std::thread worker;
    std::thread::id worker_id;

    MathRenderCache cache;
    // GUI-thread-only: newest generation issued per editor+formula.
    std::map<QString, std::uint64_t> issued_generation;
    // GUI-thread-only: live clients, guarded so a destroyed widget is safe.
    std::map<QString, QPointer<QObject>> clients;
};

MathRenderService::MathRenderService(QObject* parent) : QObject(parent) {
    qRegisterMetaType<pf::gui::MathRenderResponse>(
        "pf::gui::MathRenderResponse");
    impl_ = std::make_unique<Impl>();
    impl_->worker = std::thread([this]() {
        impl_->worker_id = std::this_thread::get_id();
        for (;;) {
            Impl::Job job;
            {
                std::unique_lock<std::mutex> lock(impl_->mutex);
                impl_->condition.wait(lock, [this] {
                    return impl_->stopping || !impl_->queue.empty();
                });
                if (impl_->stopping && impl_->queue.empty())
                    return;
                job = std::move(impl_->queue.front());
                impl_->queue.pop_front();
            }
            RenderJob(job.request);
        }
    });
}

MathRenderService::~MathRenderService() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->condition.notify_all();
    if (impl_->worker.joinable())
        impl_->worker.join();
}

bool MathRenderService::IsWorkerThread() const {
    return std::this_thread::get_id() == impl_->worker_id;
}

void MathRenderService::RegisterClient(const QString& editor_id,
                                       QObject* client) {
    impl_->clients[editor_id] = client;
}

void MathRenderService::UnregisterClient(const QString& editor_id) {
    impl_->clients.erase(editor_id);
}

std::uint64_t MathRenderService::Request(const QString& editor_id,
                                         const QString& formula_id,
                                         const QString& latex,
                                         const MathRenderStyle& style) {
    // GUI thread: bump the generation first, so a reply that is already in
    // flight becomes stale the moment a newer request exists.
    const QString key = FormulaKey(editor_id, formula_id);
    const std::uint64_t generation = ++impl_->issued_generation[key];

    Impl::Job job;
    job.request.editor_id = editor_id;
    job.request.formula_id = formula_id;
    job.request.generation = generation;
    job.request.latex = latex;
    job.request.style = style;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.push_back(std::move(job));
    }
    impl_->condition.notify_one();
    return generation;
}

std::uint64_t MathRenderService::Generation(const QString& editor_id,
                                            const QString& formula_id) const {
    auto it = impl_->issued_generation.find(FormulaKey(editor_id, formula_id));
    return it == impl_->issued_generation.end() ? 0 : it->second;
}

void MathRenderService::RenderJob(const MathRenderRequest& job) {
    // Worker thread: pure computation. No widgets, no QPixmap.
    const QString key = CacheKeyFor(job.latex, job.style);
    MathRenderResult result;
    bool cached = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        cached = impl_->cache.Find(key, &result);
    }
    if (!cached) {
        result = RenderMathPreviewImage(job.latex, job.style);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cache.Insert(key, result);
    }
    renders_completed_.fetch_add(1);

    MathRenderResponse response;
    response.editor_id = job.editor_id;
    response.formula_id = job.formula_id;
    response.generation = job.generation;
    response.latex = job.latex;
    response.result = result;

    // The reply enters the GUI thread through the object's own event loop. The
    // worker never calls a widget method directly.
    QMetaObject::invokeMethod(
        this, [this, response]() { ApplyReply(response); },
        Qt::QueuedConnection);
}

void MathRenderService::ApplyReply(const MathRenderResponse& response) {
    // GUI thread.
    auto client = impl_->clients.find(response.editor_id);
    if (client == impl_->clients.end() || client->second.isNull()) {
        // The editor is gone: never touch it (the historical use-after-free).
        stale_replies_dropped_.fetch_add(1);
        return;
    }
    const QString key =
        FormulaKey(response.editor_id, response.formula_id);
    auto issued = impl_->issued_generation.find(key);
    if (issued == impl_->issued_generation.end() ||
        response.generation != issued->second) {
        // A newer request for the same formula already replaced this result.
        stale_replies_dropped_.fetch_add(1);
        return;
    }
    emit mathRendered(response);
}

MathRenderService* MathRenderService::Shared() {
    static MathRenderService* service = new MathRenderService;
    return service;
}

}  // namespace pf::gui