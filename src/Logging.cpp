#include "Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(amberApp, "amber.app")
Q_LOGGING_CATEGORY(amberSsh, "amber.ssh")
Q_LOGGING_CATEGORY(amberTerminal, "amber.terminal")

static QString g_logPath;

static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QString level;
    switch (type) {
    case QtDebugMsg:
        level = "DEBUG";
        break;
    case QtInfoMsg:
        level = "INFO";
        break;
    case QtWarningMsg:
        level = "WARN";
        break;
    case QtCriticalMsg:
        level = "ERROR";
        break;
    case QtFatalMsg:
        level = "FATAL";
        break;
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString location = QString::fromLatin1("%1:%2").arg(QString::fromUtf8(context.file ? context.file : ""))
                                 .arg(context.line);
    const QString entry = QString::fromLatin1("%1 [%2] %3 (%4)\n")
                              .arg(timestamp, level, message, location);

    const QString resolvedPath = g_logPath.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("amberssh.log")
        : g_logPath;

    const QFileInfo logInfo(resolvedPath);
    QDir().mkpath(logInfo.absolutePath());

    QFile logFile(resolvedPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        logFile.write(entry.toUtf8());
        logFile.close();
    }
}

void configureLogging(const QString &logPath)
{
    g_logPath = logPath;
    qSetMessagePattern("%{time process} [%{type}] %{category} %{message}");
    qInstallMessageHandler(messageHandler);
}
