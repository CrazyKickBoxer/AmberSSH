#include "MainWindow.h"

#include "Logging.h"
#include "SshClient.h"
#include "TerminalEmulator.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_sshClient(new SshClient(this))
    , m_terminal(new TerminalEmulator(this))
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    auto *formLayout = new QFormLayout();

    m_hostInput = new QLineEdit("localhost", this);
    m_portInput = new QLineEdit("22", this);
    m_userInput = new QLineEdit(this);
    m_passwordInput = new QLineEdit(this);
    m_passwordInput->setEchoMode(QLineEdit::Password);

    formLayout->addRow(tr("Host"), m_hostInput);
    formLayout->addRow(tr("Port"), m_portInput);
    formLayout->addRow(tr("User"), m_userInput);
    formLayout->addRow(tr("Password"), m_passwordInput);

    m_connectButton = new QPushButton(tr("Connect"), this);

    auto *topRow = new QHBoxLayout();
    topRow->addLayout(formLayout);
    topRow->addWidget(m_connectButton);
    mainLayout->addLayout(topRow);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_terminalView = new QPlainTextEdit(this);
    m_terminalView->setReadOnly(true);
    m_terminalView->setPlaceholderText(tr("Terminal output will appear here."));

    m_statusView = new QPlainTextEdit(this);
    m_statusView->setReadOnly(true);
    m_statusView->setMaximumHeight(120);

    splitter->addWidget(m_terminalView);
    splitter->addWidget(m_statusView);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter);

    setCentralWidget(centralWidget);
    setWindowTitle(tr("Amber SSH Terminal"));
    resize(1200, 800);

    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::connectToHost);
    connect(m_sshClient, &SshClient::connected, this, &MainWindow::handleConnected);
    connect(m_sshClient, &SshClient::disconnected, this, &MainWindow::handleDisconnected);
    connect(m_sshClient, &SshClient::errorOccurred, this, &MainWindow::handleError);
    connect(m_terminal, &TerminalEmulator::screenUpdated, this, &MainWindow::handleScreenUpdate);
}

void MainWindow::connectToHost()
{
    if (m_sshClient->isConnected()) {
        m_sshClient->disconnectFromHost();
        return;
    }

    const QString host = m_hostInput->text().trimmed();
    const int port = m_portInput->text().toInt();
    const QString user = m_userInput->text().trimmed();
    const QString password = m_passwordInput->text();

    appendStatus(tr("Connecting to %1:%2 as %3...").arg(host).arg(port).arg(user));
    if (!m_sshClient->connectToHost(host, port, user, password)) {
        appendStatus(tr("Connection failed."));
    }
}

void MainWindow::handleConnected()
{
    appendStatus(tr("Connected."));
    m_connectButton->setText(tr("Disconnect"));
}

void MainWindow::handleDisconnected()
{
    appendStatus(tr("Disconnected."));
    m_connectButton->setText(tr("Connect"));
}

void MainWindow::handleError(const QString &message)
{
    appendStatus(tr("Error: %1").arg(message));
    qCWarning(amberApp) << message;
}

void MainWindow::handleScreenUpdate()
{
    m_terminalView->setPlainText(m_terminal->lines().join('\n'));
}

void MainWindow::appendStatus(const QString &message)
{
    m_statusView->appendPlainText(message);
}
