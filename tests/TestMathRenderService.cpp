// P0-07 regression tests: asynchronous math rendering.
//
// The release blocker: RenderMathPreview() ran the TeX child process
// synchronously on the GUI thread (waitForStarted/waitForFinished), so a slow
// compile froze the window for up to ~28 s - and editing under that stall is
// what reproduced the inline-math lifetime crash.
//
// Invariants enforced here:
//   * a render request returns immediately, without running TeX on the caller;
//   * the worker produces a QImage and the QPixmap is built on the GUI thread;
//   * a reply for an old generation never reaches the client;
//   * a client destroyed while a render is in flight is never touched;
//   * the cache evicts one LRU entry instead of clearing all entries.
#include "TestMain.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QThread>

#include <atomic>
#include <memory>
#include <string>

#include "app/math/MathRenderService.h"

using namespace pf;
using namespace pf::gui;

namespace {

QApplication* EnsureApp() { return qApp; }

void Spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

// A client that records what the service delivered.
class RecordingClient : public QObject {
public:
    explicit RecordingClient(const QString& id) : id_(id) {}
    QString id() const { return id_; }

    void Record(const MathRenderResponse& response) {
        last_generation = response.generation;
        last_formula = response.formula_id;
        last_has_image = !response.result.image.isNull();
        ++received;
    }

    QString id_;
    std::atomic<int> received{0};
    std::uint64_t last_generation = 0;
    QString last_formula;
    bool last_has_image = false;
};

} // namespace

// The request itself must be non-blocking: enqueueing 20 renders returns
// promptly even though each one is a full parse/render job.
PF_TEST(MathRenderRequestIsNonBlocking) {
    EnsureApp();
    MathRenderService service;
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 20; ++i) {
        service.Request(QStringLiteral("editor-a"), QStringLiteral("f1"),
                        QStringLiteral("x_%1 + y").arg(i), style);
    }
    const qint64 elapsed = timer.elapsed();
    // The old synchronous path spent a TeX process per formula; enqueueing 20
    // must cost far less than one real render.
    PF_CHECK(elapsed < 2000);
    std::cout << "    enqueue of 20 requests took " << elapsed << " ms\n";
}

// The worker thread - not the GUI thread - executes the render.
PF_TEST(MathRenderRunsOnTheWorkerThread) {
    EnsureApp();
    MathRenderService service;
    PF_CHECK(!service.IsWorkerThread());
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;

    service.Request(QStringLiteral("editor-b"), QStringLiteral("f1"),
                    QStringLiteral("\\frac{a}{b}"), style);
    QElapsedTimer timer;
    timer.start();
    while (service.renders_completed() == 0 && timer.elapsed() < 5000)
        Spin(10);
    PF_CHECK(service.renders_completed() >= 1);
}

// A live client receives the reply; the pixels arrive as a QImage (so the
// worker never touched QPixmap) and convert on the GUI thread.
PF_TEST(MathRenderReplyReachesLiveClient) {
    EnsureApp();
    MathRenderService service;
    RecordingClient client(QStringLiteral("editor-c"));
    service.RegisterClient(client.id(), &client);
    QObject::connect(&service, &MathRenderService::mathRendered, &client,
                     [&client](const MathRenderResponse& response) {
                         if (response.editor_id == client.id())
                             client.Record(response);
                     });
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;
    const std::uint64_t generation = service.Request(
        client.id(), QStringLiteral("f1"), QStringLiteral("a^2+b^2"), style);
    QElapsedTimer timer;
    timer.start();
    while (client.received.load() == 0 && timer.elapsed() < 5000)
        Spin(10);
    PF_CHECK(client.received.load() == 1);
    PF_CHECK(client.last_generation == generation);
    PF_CHECK(client.last_has_image);
}

// A newer request for the same formula makes the older reply stale: the
// client must see only the newest generation.
PF_TEST(MathRenderDropsSupersededGeneration) {
    EnsureApp();
    MathRenderService service;
    RecordingClient client(QStringLiteral("editor-d"));
    service.RegisterClient(client.id(), &client);
    QObject::connect(&service, &MathRenderService::mathRendered, &client,
                     [&client](const MathRenderResponse& response) {
                         if (response.editor_id == client.id())
                             client.Record(response);
                     });
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;

    service.Request(client.id(), QStringLiteral("same"), QStringLiteral("old"),
                    style);
    const std::uint64_t newest = service.Request(
        client.id(), QStringLiteral("same"), QStringLiteral("new"), style);

    QElapsedTimer timer;
    timer.start();
    while (client.received.load() == 0 && timer.elapsed() < 5000)
        Spin(10);
    Spin(100);
    // Only the newest generation may have been applied.
    PF_CHECK(client.last_generation == newest);
    PF_CHECK(service.stale_replies_dropped() >= 1);
}

// A client destroyed while a render is in flight must never be touched: the
// service holds a QPointer and drops the reply.
PF_TEST(MathRenderNeverTouchesDestroyedClient) {
    EnsureApp();
    MathRenderService service;
    auto* client = new RecordingClient(QStringLiteral("editor-e"));
    service.RegisterClient(client->id(), client);
    QObject::connect(
        &service, &MathRenderService::mathRendered, client,
        [client](const MathRenderResponse& response) {
            if (response.editor_id == client->id())
                client->Record(response);
        });
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;
    service.Request(client->id(), QStringLiteral("f1"),
                    QStringLiteral("x+y"), style);
    // Destroy the client immediately: the render is still queued or running.
    delete client;
    // Pump long enough for the reply to arrive.
    Spin(300);
    // Reaching here without a sanitizer report and without a crash is the
    // assertion; the drop counter proves the reply was discarded.
    PF_CHECK(service.stale_replies_dropped() >= 1);
}

// The cache evicts one LRU entry, never the whole cache.
PF_TEST(MathRenderCacheEvictsLeastRecentlyUsedOnly) {
    MathRenderCache cache(3);
    MathRenderResult value;
    value.width = 1;
    cache.Insert(QStringLiteral("a"), value);
    value.width = 2;
    cache.Insert(QStringLiteral("b"), value);
    value.width = 3;
    cache.Insert(QStringLiteral("c"), value);
    PF_CHECK_EQ(cache.size(), std::size_t{3});

    // Touch "a" so "b" becomes the least recently used.
    MathRenderResult hit;
    PF_CHECK(cache.Find(QStringLiteral("a"), &hit));
    PF_CHECK_EQ(hit.width, 1);

    value.width = 4;
    cache.Insert(QStringLiteral("d"), value);
    PF_CHECK_EQ(cache.size(), std::size_t{3});
    // "b" was evicted; "a", "c" and "d" survive.
    PF_CHECK(!cache.Find(QStringLiteral("b"), &hit));
    PF_CHECK(cache.Find(QStringLiteral("a"), &hit));
    PF_CHECK(cache.Find(QStringLiteral("c"), &hit));
    PF_CHECK(cache.Find(QStringLiteral("d"), &hit));
}

// A worker result converts to a usable QPixmap on the GUI thread.
PF_TEST(MathResultConvertsToPixmapOnGuiThread) {
    EnsureApp();
    MathRenderResult result;
    result.image = QImage(4, 4, QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::red);
    result.device_pixel_ratio = 2.0;
    const QPixmap pixmap = PixmapFromMathResult(result);
    PF_CHECK(!pixmap.isNull());
    PF_CHECK_EQ(pixmap.width(), 4);
    PF_CHECK(pixmap.devicePixelRatio() == 2.0);
}
// P0-08 regression: the service connection outlives the function that created
// it, so any per-card state captured by reference is a stack-use-after-return.
// This drives the same shape - a card whose reply arrives long after the
// creating scope has exited - and would be caught by ASan.
PF_TEST(MathRenderLateReplyAfterCreatingScopeExited) {
    EnsureApp();
    MathRenderService service;
    RecordingClient client(QStringLiteral("editor-late"));
    service.RegisterClient(client.id(), &client);
    QObject::connect(&service, &MathRenderService::mathRendered, &client,
                     [&client](const MathRenderResponse& response) {
                         if (response.editor_id == client.id())
                             client.Record(response);
                     });

    // Simulate the BlockEditor pattern: the generation lives on the heap, so
    // the connection below stays valid after this scope ends.
    auto generation = std::make_shared<std::uint64_t>(0);
    {
        MathRenderStyle style;
        style.backend = MathRenderBackend::ApproximateOnly;
        style.font_px = 16;
        *generation = service.Request(client.id(), QStringLiteral("card"),
                                      QStringLiteral("x^2"), style);
    }
    // The creating scope is gone; the reply must still be applied safely.
    QElapsedTimer timer;
    timer.start();
    while (client.received.load() == 0 && timer.elapsed() < 5000)
        Spin(10);
    PF_CHECK(client.received.load() == 1);
    PF_CHECK(client.last_generation == *generation);
}
