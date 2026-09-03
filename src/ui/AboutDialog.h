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

} // namespace amber
