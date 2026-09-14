#include <QApplication>

#include "app/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("PaperForge");
    app.setOrganizationName("PaperForge");

    pf::gui::MainWindow window;
    window.show();
    return app.exec();
}
