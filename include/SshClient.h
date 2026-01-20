#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

struct ssh_session_struct;
struct ssh_channel_struct;

class SshClient : public QObject
{
    Q_OBJECT

public:
    explicit SshClient(QObject *parent = nullptr);
    ~SshClient();

    bool connectToHost(const QString &host, int port, const QString &user, const QString &password);
    void disconnectFromHost();
    bool isConnected() const;

    bool writeData(const QByteArray &data);

signals:
    void connected();
    void disconnected();
    void dataReceived(const QByteArray &data);
    void errorOccurred(const QString &message);

private:
    void emitLibsshError(const QString &context);

    ssh_session_struct *m_session = nullptr;
    ssh_channel_struct *m_channel = nullptr;
    bool m_connected = false;
};
