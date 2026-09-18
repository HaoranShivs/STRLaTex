#include <QApplication>

#include <cstdio>
#include <exception>
#include <cstdlib>

#include "app/MainWindow.h"

// P0-02: the outermost exception barrier. The internal Result/IoError
// plumbing already contains deserialization and I/O failures; this barrier is
// the last line of defence, so an unexpected exception anywhere still produces
// a diagnostic instead of a silent abort.
int main(int argc, char* argv[]) {
    try {
        QApplication app(argc, argv);
        app.setApplicationName("PaperForge");
        app.setOrganizationName("PaperForge");

        pf::gui::MainWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& e) {
        // Fatal log to stderr: the window may be gone, so this is the only
        // channel that is guaranteed to exist.
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
