// Driver for the rich inline editor tests: they build widgets, so they need a
// QApplication. The app is heap-allocated and destroyed before main returns -
// a static QApplication destroyed at exit order-conflicts with the test
// framework's statics and segfaults after everything has already passed.
#include <QApplication>

#include "TestMain.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const int result = testfw::RunAll();
    std::cout.flush();
    // Destroy the application explicitly so its teardown does not race the
    // test framework's statics at exit.
    return result;
}
