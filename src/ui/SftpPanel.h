// SftpPanel.h — a themed modal SFTP file browser for the active session.
//
// It opens its OWN libssh2 connection on a worker thread (never multiplexing
// the shell channel), authenticating with the same profile + retained secrets.
// The window lists a remote directory, navigates on double-click, and
// downloads / uploads single files through the standard file dialogs.
#pragma once

#include <Windows.h>

#include "../profiles/ConnectionProfile.h"
#include "../utility/SecureString.h"

namespace amber
{

class SftpPanel
{
public:
    // Runs modally against a copy of the profile and secrets. Returns when the
    // user closes the window.
    static void Open(HWND owner, const ConnectionProfile& profile,
                     const SecureString& password,
                     const SecureString& passphrase);
};

} // namespace amber
