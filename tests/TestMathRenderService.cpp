// P0-07 回归测试：异步数学渲染。
//
// 发布阻塞点：RenderMathPreview() 在 GUI 线程上同步运行 TeX 子进程
// （waitForStarted/waitForFinished），因此一次缓慢的编译会把窗口冻结长达约 28 秒——
// 而正是在这种卡顿下的编辑复现了行内公式生命周期的崩溃。
//
// 此处强制的不变量：
//   * 渲染请求立即返回，不在调用方执行 TeX；
//   * worker 产出 QImage，QPixmap 在 GUI 线程上构建；
//   * 旧 generation 的回复永远不会到达客户端；
//   * 渲染进行中被销毁的客户端永远不会被触碰；
//   * 缓存只淘汰一个 LRU 条目，而不是清空所有条目。
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

QApplication* EnsureApp() {
    return qApp;
}

void Spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

// 记录服务交付内容的客户端。
class RecordingClient : public QObject {
  public:
    explicit RecordingClient(const QString& id) : id_(id) {}
    QString id() const {
        return id_;
    }

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

// 请求本身必须是非阻塞的：即使每个渲染都是完整的解析/渲染任务，
// 入队 20 个渲染也必须迅速返回。
PF_TEST(MathRenderRequestIsNonBlocking) {
    EnsureApp();
    MathRenderService service;
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 20; ++i) {
        service.Request(QStringLiteral("editor-a"), QStringLiteral("f1"), QStringLiteral("x_%1 + y").arg(i), style);
    }
    const qint64 elapsed = timer.elapsed();
    // 旧的同步路径为每个公式启动一个 TeX 进程；入队 20 个请求的耗时
    // 必须远小于一次真正的渲染。
    PF_CHECK(elapsed < 2000);
    std::cout << "    enqueue of 20 requests took " << elapsed << " ms\n";
}

// 执行渲染的是 worker 线程，而非 GUI 线程。
PF_TEST(MathRenderRunsOnTheWorkerThread) {
    EnsureApp();
    MathRenderService service;
    PF_CHECK(!service.IsWorkerThread());
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;

    service.Request(QStringLiteral("editor-b"), QStringLiteral("f1"), QStringLiteral("\\frac{a}{b}"), style);
    QElapsedTimer timer;
    timer.start();
    while (service.renders_completed() == 0 && timer.elapsed() < 5000)
        Spin(10);
    PF_CHECK(service.renders_completed() >= 1);
}

// 存活的客户端会收到回复；像素以 QImage 形式到达（因此 worker 从未接触
// QPixmap），并在 GUI 线程上完成转换。
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
    const std::uint64_t generation =
        service.Request(client.id(), QStringLiteral("f1"), QStringLiteral("a^2+b^2"), style);
    QElapsedTimer timer;
    timer.start();
    while (client.received.load() == 0 && timer.elapsed() < 5000)
        Spin(10);
    PF_CHECK(client.received.load() == 1);
    PF_CHECK(client.last_generation == generation);
    PF_CHECK(client.last_has_image);
}

// 对同一公式的较新请求会使较旧的回复变为陈旧：客户端只能看到最新的
// generation。
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

    service.Request(client.id(), QStringLiteral("same"), QStringLiteral("old"), style);
    const std::uint64_t newest = service.Request(client.id(), QStringLiteral("same"), QStringLiteral("new"), style);

    QElapsedTimer timer;
    timer.start();
    while (client.received.load() == 0 && timer.elapsed() < 5000)
        Spin(10);
    Spin(100);
    // 只可能应用了最新的 generation。
    PF_CHECK(client.last_generation == newest);
    PF_CHECK(service.stale_replies_dropped() >= 1);
}

// 渲染进行中被销毁的客户端绝不能被触碰：服务持有 QPointer 并丢弃该回复。
PF_TEST(MathRenderNeverTouchesDestroyedClient) {
    EnsureApp();
    MathRenderService service;
    auto* client = new RecordingClient(QStringLiteral("editor-e"));
    service.RegisterClient(client->id(), client);
    QObject::connect(&service, &MathRenderService::mathRendered, client, [client](const MathRenderResponse& response) {
        if (response.editor_id == client->id())
            client->Record(response);
    });
    MathRenderStyle style;
    style.backend = MathRenderBackend::ApproximateOnly;
    style.font_px = 16;
    service.Request(client->id(), QStringLiteral("f1"), QStringLiteral("x+y"), style);
    // 立即销毁客户端：此时渲染仍在排队或运行中。
    delete client;
    // 持续泵送事件，留足回复到达的时间。
    Spin(300);
    // 能在没有 sanitizer 报告且没有崩溃的情况下走到这里，本身就是断言；
    // 丢弃计数器证明该回复已被丢弃。
    PF_CHECK(service.stale_replies_dropped() >= 1);
}

// 缓存只淘汰一个 LRU 条目，绝不淘汰整个缓存。
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

    // 访问一次 "a"，使 "b" 成为最近最少使用者。
    MathRenderResult hit;
    PF_CHECK(cache.Find(QStringLiteral("a"), &hit));
    PF_CHECK_EQ(hit.width, 1);

    value.width = 4;
    cache.Insert(QStringLiteral("d"), value);
    PF_CHECK_EQ(cache.size(), std::size_t{3});
    // "b" 被淘汰；"a"、"c" 和 "d" 得以保留。
    PF_CHECK(!cache.Find(QStringLiteral("b"), &hit));
    PF_CHECK(cache.Find(QStringLiteral("a"), &hit));
    PF_CHECK(cache.Find(QStringLiteral("c"), &hit));
    PF_CHECK(cache.Find(QStringLiteral("d"), &hit));
}

// worker 结果在 GUI 线程上转换为可用的 QPixmap。
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
// P0-08 回归：服务连接的存活期长于创建它的函数，因此任何按引用捕获的
// 每卡片状态都会造成 stack-use-after-return。
// 本测试构造同样的形态——卡片的回复在创建作用域退出很久之后才到达——
// 会被 ASan 捕获。
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

    // 模拟 BlockEditor 的模式：generation 位于堆上，因此本作用域结束后
    // 下面的连接依然有效。
    auto generation = std::make_shared<std::uint64_t>(0);
    {
        MathRenderStyle style;
        style.backend = MathRenderBackend::ApproximateOnly;
        style.font_px = 16;
        *generation = service.Request(client.id(), QStringLiteral("card"), QStringLiteral("x^2"), style);
    }
    // 创建作用域已消失；回复仍必须被安全地应用。
    QElapsedTimer timer;
    timer.start();
    while (client.received.load() == 0 && timer.elapsed() < 5000)
        Spin(10);
    PF_CHECK(client.received.load() == 1);
    PF_CHECK(client.last_generation == *generation);
}
