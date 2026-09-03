// SftpBrowser.h — WinSCP-style tabbed, dual-pane SFTP file manager.
//
// One top-level (non-modal) window shared by every connection; each tab is
// its own SftpClient on its own worker thread. Left pane: local files.
// Right pane: remote files. Commander keys: F2 rename, F4 open/edit,
// F5 copy to the other side, F7 new folder, F8 delete, Backspace up,
// Enter open. Explorer drops onto the remote pane upload. Transfers run in
// the background with a progress queue; edited remote files re-upload when
// saved by the external editor.
#pragma once

#include <Windows.h>

#include <string>

#include "../profiles/ConnectionProfile.h"
#include "../utility/SecureString.h"

namespace amber
{

class SftpBrowser
{
public:
    // Shows the browser (creating it on first use) and opens a tab for the
    // profile. initialDir empty = the remote home directory.
    static void OpenTab(HWND owner, const ConnectionProfile& profile,
                        const SecureString& password, const SecureString& passphrase,
                        const std::string& initialDir = {});
    // Opens a tab (or reuses one for the same profile) and opens/edits the
    // given remote file with its default Windows application.
    static void OpenRemoteFile(HWND owner, const ConnectionProfile& profile,
                               const SecureString& password,
                               const SecureString& passphrase,
                               const std::string& remotePath);
    // Callback used by the "+" tab button: the app shows its connection
    // manager and, on success, calls OpenTab.
    using NewTabFn = void (*)(void* ctx);
    static void SetNewTabHandler(NewTabFn fn, void* ctx);
    static bool IsOpen();
};

} // namespace amber
