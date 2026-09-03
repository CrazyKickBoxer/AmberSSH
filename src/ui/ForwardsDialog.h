// ForwardsDialog.h — port-forwarding editor for a profile: local, remote and
// dynamic (SOCKS5) tunnels plus the jump host. Edits the profile's
// `forwards` spec string ("L8080:localhost:80;D1080;R9000:127.0.0.1:9000").
#pragma once

#include <Windows.h>

#include "../profiles/ConnectionProfile.h"

namespace amber
{

class ForwardsDialog
{
public:
    // Modal. Returns true when the user pressed OK (profile updated in place).
    static bool Show(HWND owner, ConnectionProfile& profile);
};

} // namespace amber
