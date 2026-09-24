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

// ---------------- 有界LRU缓存 ----------------

MathRenderCache::MathRenderCache(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

bool MathRenderCache::Find(const QString& key, MathRenderResult* out) {
    auto it = entries_.find(key);
    if (it == entries_.end())
        return false;
    // 命中会刷新时间戳，使该条目成为最近使用的条目。
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
        // 精确淘汰最近最少使用的那一个条目。此前的实现在溢出时会清空全部
        // 256个条目，仅因一次未命中就丢弃所有已预热的预览。
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
    // 仅限GUI线程：QPixmap的构造不是线程安全的。
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

// ---------------- 服务 ----------------

namespace {

QString CacheKeyFor(const QString& latex, const MathRenderStyle& style) {
    return latex + QChar(0x1f) + QString::number(style.font_px) + QChar(0x1f) +
           style.color.name(QColor::HexArgb) + QChar(0x1f) +
           QString::number(style.device_pixel_ratio, 'f', 2) + QChar(0x1f) +
           style.font_family + QChar(0x1f) + style.template_id + QChar(0x1f) +
           QString::number(static_cast<int>(style.backend)) + QChar(0x1f) +
           QStringLiteral("v1");  // 渲染器后端版本
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
    // 仅限GUI线程：每个editor+formula已签发的最新generation。
    std::map<QString, std::uint64_t> issued_generation;
    // 仅限GUI线程：存活的client，受保护以确保已销毁的widget安全。
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
    // GUI线程：先递增generation，这样一旦出现更新的请求，已在途的回复
    // 立即失效。
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
    // worker线程：纯计算。不涉及widget，不涉及QPixmap。
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
    response.render_font_px = job.style.font_px;
    response.result = result;

    // 回复通过对象自身的事件循环进入GUI线程。worker绝不直接调用widget
    // 方法。
    QMetaObject::invokeMethod(
        this, [this, response]() { ApplyReply(response); },
        Qt::QueuedConnection);
}

void MathRenderService::ApplyReply(const MathRenderResponse& response) {
    // GUI线程。
    auto client = impl_->clients.find(response.editor_id);
    if (client == impl_->clients.end() || client->second.isNull()) {
        // editor已不存在：绝不触碰它（历史上的use-after-free）。
        stale_replies_dropped_.fetch_add(1);
        return;
    }
    const QString key =
        FormulaKey(response.editor_id, response.formula_id);
    auto issued = impl_->issued_generation.find(key);
    if (issued == impl_->issued_generation.end() ||
        response.generation != issued->second) {
        // 针对同一formula的更新请求已经替换了该结果。
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
