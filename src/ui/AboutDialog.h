// AboutDialog.h — the About Amber SSH box.
//
// A single owner-painted panel: the application mark on the left, the wordmark
// and product details on the right. It takes its colours from the active
// interface skin (Chrome.h / Theme.h) like every other native surface, so it
// looks native to Cyberpunk, LCARS, Blueprint, Swiss and the rest rather than
// being a fixed dark box.
#pragma once

#include <Windows.h>

namespace amber
{

// Modal. Returns when the user closes the box (Esc, the close button, or a
// click anywhere on it).
void ShowAboutDialog(HWND owner);

// Product strings, shared with the version resource and the title bar.
inline constexpr wchar_t kProductName[] = L"Amber SSH";
inline constexpr wchar_t kProductTagline[] = L"GPU PARTICLE TERMINAL";
inline constexpr wchar_t kProductVersion[] = L"1.0.0";

// Attribution shown in the About box. Most of what AmberSSH links requires it
// (BSD-3, Apache-2.0, MIT, OFL all carry a notice condition), and this is
// where a user looks. The full terms live in THIRD-PARTY-NOTICES.md beside the
// executable, and the bundled fonts carry their own licences in exe\fonts.
//
// Nothing here is an X server or an RDP client: AmberSSH bundles neither and
// hands off to whatever is installed, which is why the GPL on VcXsrv and the
// terms on current Xming never apply to it. See docs/REMOTE-DISPLAY.md.
inline constexpr wchar_t kProductCredits[] =
    L"libssh2 \x00B7 OpenSSL \x00B7 nlohmann/json \x00B7 Dear ImGui \x00B7 "
    L"DirectX Shader Compiler \x00B7 eleven fonts under the SIL Open Font, "
    L"Ubuntu Font and Hack licences.\nFull terms in THIRD-PARTY-NOTICES.md "
    L"and fonts\\LICENSES.md.";

} // namespace amber
