#include "Logging.h"
#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QStandardPaths>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString logPath = QDir(logDir).filePath("amberssh.log");
    configureLogging(logPath);

    MainWindow window;
    window.show();

    return app.exec();
}
