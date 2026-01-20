#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class TerminalEmulator : public QObject
{
    Q_OBJECT

public:
    explicit TerminalEmulator(QObject *parent = nullptr);

    void resizeGrid(int columns, int rows);
    void processData(const QByteArray &data);
    const QStringList &lines() const;

signals:
    void screenUpdated();

private:
    void applyCharacter(QChar character);
    void lineFeed();

    int m_columns = 80;
    int m_rows = 24;
    int m_cursorRow = 0;
    int m_cursorColumn = 0;
    QStringList m_buffer;
    bool m_inEscape = false;
    QString m_escapeSequence;
};
