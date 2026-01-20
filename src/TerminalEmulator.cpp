#include "TerminalEmulator.h"

#include "Logging.h"

TerminalEmulator::TerminalEmulator(QObject *parent)
    : QObject(parent)
{
    resizeGrid(m_columns, m_rows);
}

void TerminalEmulator::resizeGrid(int columns, int rows)
{
    m_columns = columns;
    m_rows = rows;
    m_buffer = QStringList(rows, QString(columns, QChar(' ')));
    m_cursorRow = 0;
    m_cursorColumn = 0;
    emit screenUpdated();
    qCInfo(amberTerminal) << "Terminal resized to" << columns << "x" << rows;
}

void TerminalEmulator::processData(const QByteArray &data)
{
    for (const char byte : data) {
        const QChar character = QChar::fromLatin1(byte);
        if (m_inEscape) {
            m_escapeSequence.append(character);
            if (character.isLetter()) {
                m_inEscape = false;
                m_escapeSequence.clear();
            }
            continue;
        }

        if (character == QChar('\x1b')) {
            m_inEscape = true;
            m_escapeSequence.clear();
            continue;
        }

        applyCharacter(character);
    }

    emit screenUpdated();
}

const QStringList &TerminalEmulator::lines() const
{
    return m_buffer;
}

void TerminalEmulator::applyCharacter(QChar character)
{
    if (character == QChar('\r')) {
        m_cursorColumn = 0;
        return;
    }

    if (character == QChar('\n')) {
        lineFeed();
        return;
    }

    if (character == QChar('\b')) {
        m_cursorColumn = qMax(0, m_cursorColumn - 1);
        return;
    }

    if (!character.isPrint()) {
        return;
    }

    if (m_cursorRow >= 0 && m_cursorRow < m_buffer.size()) {
        QString &line = m_buffer[m_cursorRow];
        if (m_cursorColumn >= 0 && m_cursorColumn < line.size()) {
            line[m_cursorColumn] = character;
        }
    }

    m_cursorColumn++;
    if (m_cursorColumn >= m_columns) {
        lineFeed();
    }
}

void TerminalEmulator::lineFeed()
{
    m_cursorColumn = 0;
    m_cursorRow++;
    if (m_cursorRow >= m_rows) {
        m_buffer.pop_front();
        m_buffer.push_back(QString(m_columns, QChar(' ')));
        m_cursorRow = m_rows - 1;
    }
}
