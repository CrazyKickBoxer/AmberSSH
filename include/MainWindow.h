#pragma once

#include <QMainWindow>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class SshClient;
class TerminalEmulator;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void connectToHost();
    void handleConnected();
    void handleDisconnected();
    void handleError(const QString &message);
    void handleScreenUpdate();

private:
    void appendStatus(const QString &message);

    QLineEdit *m_hostInput = nullptr;
    QLineEdit *m_portInput = nullptr;
    QLineEdit *m_userInput = nullptr;
    QLineEdit *m_passwordInput = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPlainTextEdit *m_terminalView = nullptr;
    QPlainTextEdit *m_statusView = nullptr;

    SshClient *m_sshClient = nullptr;
    TerminalEmulator *m_terminal = nullptr;
};
