// Hello.h — Windows Hello user-presence verification (PIN / fingerprint /
// face) via the WinRT UserConsentVerifier. Gates the use of secrets stored
// in Credential Manager when the user opts in.
#pragma once

#include <Windows.h>

#include <string>

namespace amber
{

// Blocks (pumping messages) until the user verifies or cancels. Returns
// true when verified — and also when Windows Hello is not configured on this
// machine, so the gate never locks a user out of their own sessions.
bool VerifyUserPresence(HWND owner, const std::wstring& message);

} // namespace amber
