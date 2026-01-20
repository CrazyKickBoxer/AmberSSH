#pragma once

#include <QLoggingCategory>
#include <QString>

Q_DECLARE_LOGGING_CATEGORY(amberApp)
Q_DECLARE_LOGGING_CATEGORY(amberSsh)
Q_DECLARE_LOGGING_CATEGORY(amberTerminal)

void configureLogging(const QString &logPath);
