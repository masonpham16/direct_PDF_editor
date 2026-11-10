#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileOpenEvent>

namespace {

// macOS delivers "open this file" requests (double-clicking a PDF with
// DirectPDF set as its handler, dragging one onto the Dock icon, or
// Finder's Open With) as a QFileOpenEvent at the application level, not as
// a command-line argument — argv only covers launches from a shell. Events
// can arrive before MainWindow exists yet (the very launch that's opening
// a file), so anything received before mainWindow is set is queued and
// drained once main() has a window to hand it to.
class Application : public QApplication {
public:
    using QApplication::QApplication;

    MainWindow *mainWindow = nullptr;
    QStringList pendingOpenPaths;

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            const QString path = static_cast<QFileOpenEvent *>(event)->file();
            if (mainWindow) {
                mainWindow->openPathInNewTab(path);
            } else {
                pendingOpenPaths.append(path);
            }
            return true;
        }
        return QApplication::event(event);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    Application app(argc, argv);
    QApplication::setApplicationName("DirectPDF");
    QApplication::setOrganizationName("DirectPDF");
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));

    QCommandLineParser parser;
    parser.addPositionalArgument("file", "PDF file to open on startup.");
    parser.process(app);

    MainWindow window;
    app.mainWindow = &window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        window.openPathInNewTab(args.first());
    }
    for (const QString &path : app.pendingOpenPaths) {
        window.openPathInNewTab(path);
    }
    app.pendingOpenPaths.clear();

    return app.exec();
}
