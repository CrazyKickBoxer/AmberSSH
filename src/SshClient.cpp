#include "SshClient.h"

#include "Logging.h"

#include <libssh/libssh.h>

SshClient::SshClient(QObject *parent)
    : QObject(parent)
{
}

SshClient::~SshClient()
{
    disconnectFromHost();
}

bool SshClient::connectToHost(const QString &host, int port, const QString &user, const QString &password)
{
    if (m_connected) {
        emit errorOccurred("Already connected.");
        return false;
    }

    m_session = ssh_new();
    if (!m_session) {
        emit errorOccurred("Unable to allocate SSH session.");
        return false;
    }

    ssh_options_set(m_session, SSH_OPTIONS_HOST, host.toUtf8().constData());
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(m_session, SSH_OPTIONS_USER, user.toUtf8().constData());

    const int rc = ssh_connect(m_session);
    if (rc != SSH_OK) {
        emitLibsshError("ssh_connect");
        return false;
    }

    if (ssh_userauth_password(m_session, nullptr, password.toUtf8().constData()) != SSH_AUTH_SUCCESS) {
        emitLibsshError("ssh_userauth_password");
        return false;
    }

    m_channel = ssh_channel_new(m_session);
    if (!m_channel) {
        emit errorOccurred("Unable to allocate SSH channel.");
        return false;
    }

    if (ssh_channel_open_session(m_channel) != SSH_OK) {
        emitLibsshError("ssh_channel_open_session");
        return false;
    }

    if (ssh_channel_request_pty(m_channel) != SSH_OK) {
        emitLibsshError("ssh_channel_request_pty");
        return false;
    }

    if (ssh_channel_request_shell(m_channel) != SSH_OK) {
        emitLibsshError("ssh_channel_request_shell");
        return false;
    }

    ssh_channel_set_blocking(m_channel, 0);
    m_connected = true;
    emit connected();
    qCInfo(amberSsh) << "Connected to" << host << "on port" << port;
    return true;
}

void SshClient::disconnectFromHost()
{
    if (m_channel) {
        ssh_channel_send_eof(m_channel);
        ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }

    if (m_session) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }

    if (m_connected) {
        m_connected = false;
        emit disconnected();
        qCInfo(amberSsh) << "Disconnected.";
    }
}

bool SshClient::isConnected() const
{
    return m_connected;
}

bool SshClient::writeData(const QByteArray &data)
{
    if (!m_channel) {
        emit errorOccurred("SSH channel is not available.");
        return false;
    }

    const int written = ssh_channel_write(m_channel, data.constData(), data.size());
    if (written < 0) {
        emitLibsshError("ssh_channel_write");
        return false;
    }

    return true;
}

void SshClient::emitLibsshError(const QString &context)
{
    const QString message = QString::fromLatin1("%1 failed: %2").arg(context, ssh_get_error(m_session));
    qCWarning(amberSsh) << message;
    emit errorOccurred(message);
}
