// input.h — Win32 key events → terminal escape sequences, configurable the
// way PuTTY's Keyboard page is: Backspace code, Home/End flavour, function
// key flavour, application cursor keys, application keypad.
#pragma once

#include <string>

struct KeyMods
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct KeyOptions
{
    bool appCursorKeys = false;   // DECCKM: arrows send SS3 instead of CSI
    bool appKeypad = false;       // DECKPAM: numeric keypad sends SS3 p..y
    bool backspaceIsDel = true;   // 0x7F (Control-?) vs 0x08 (Control-H)
    int  homeEnd = 0;             // 0 xterm ESC[H/ESC[F, 1 standard ESC[1~/ESC[4~, 2 rxvt ESC[H/ESC Ow
    int  fnKeys = 0;              // 0 xterm, 1 Linux, 2 VT100+, 3 SCO
};

// Returns the byte sequence for a non-character key (arrows, F-keys, nav
// cluster, Backspace, application keypad), or "" if the key produces a
// WM_CHAR instead.
std::string TranslateKey(unsigned vk, const KeyMods& mods, const KeyOptions& opt);

// Legacy form (xterm defaults, no keypad mode).
inline std::string TranslateKey(unsigned vk, const KeyMods& mods, bool appCursorKeys)
{
    KeyOptions o;
    o.appCursorKeys = appCursorKeys;
    return TranslateKey(vk, mods, o);
}
