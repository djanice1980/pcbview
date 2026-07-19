#include "app/viewer.h"

#include <QApplication>
#include <QIcon>

#include "app/main_window.h"
#include "app/theme.h"

namespace pcbview::app {

int runViewer(const std::string& path, const ViewerOptions& opts) {
    // Qt wants argc/argv by reference and outlives them, so keep them alive here.
    static int argc = 1;
    static char argv0[] = "pcbview";
    static char* argv[] = {argv0, nullptr};

    QApplication app(argc, argv);
    QApplication::setApplicationName("pcbview");
    QApplication::setOrganizationName("pcbview");
    QApplication::setWindowIcon(QIcon(":/pcbview.png"));

    theme::apply(app);

    // MainWindow imports and tessellates: it owns the board so File->Open can
    // replace it.
    MainWindow window(QString::fromStdString(path));
    if (opts.width > 0 && opts.height > 0) window.resize(opts.width, opts.height);
    window.show();

    return app.exec();
}

}  // namespace pcbview::app
