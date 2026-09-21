#include <QApplication>

#include <cstdio>
#include <exception>
#include <cstdlib>

#include "app/MainWindow.h"

// P0-02：最外层的异常屏障。内部的 Result/IoError
// 机制已经兜住了反序列化与 I/O 失败；本屏障是
// 最后一道防线，因此任何位置出现意外异常仍会产生
// 诊断信息，而不是静默中止。
int main(int argc, char* argv[]) {
    try {
        QApplication app(argc, argv);
        app.setApplicationName("PaperForge");
        app.setOrganizationName("PaperForge");

        pf::gui::MainWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& e) {
        // 向 stderr 输出致命日志：窗口可能已不存在，这是
        // 唯一保证可用的通道。
        std::fprintf(stderr,
                     "fatal: unhandled exception: %s\n", e.what());
        std::fflush(stderr);
        return EXIT_FAILURE;
    } catch (...) {
        std::fprintf(stderr, "fatal: unhandled unknown exception\n");
        std::fflush(stderr);
        return EXIT_FAILURE;
    }
}
