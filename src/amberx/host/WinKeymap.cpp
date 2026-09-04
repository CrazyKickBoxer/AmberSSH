// WinKeymap.cpp — reads the live Windows keyboard layout and describes it in
// the fixed-width terms of amberwin.h, so the X side can build an XKB map
// from it without ever calling Win32. Original AmberSSH file.
//
// How a layout is read: for every scan code in the main block, ask the
// layout which virtual key it is, then ask ToUnicodeEx what that key
// produces under four modifier states — nothing, Shift, AltGr, Shift+AltGr.
// That is the same question a text editor asks, so what comes back is what
// the user's own keyboard would type, for any layout Windows has installed,
// without shipping a keymap database or running a compiler.
//
// Three details make the difference between this working and nearly working:
//
//   * ToUnicodeEx normally *changes* the layout's dead-key state, so asking
//     what a key produces would swallow the user's next real keystroke. Bit
//     2 of wFlags ("do not change keyboard state", Windows 10 1607+) stops
//     that; the state is also flushed afterwards for older builds.
//   * AltGr is Ctrl+Alt to Windows. Asking with both held gives the AltGr
//     symbol on layouts that have one, and a control character (< 0x20) on
//     layouts that do not — so control characters are rejected at levels 3
//     and 4, which is what makes has_level3 mean something.
//   * A key whose first level is not a printable character is not reported
//     at all. Escape, Tab, Return and the rest are structural; the X side
//     already knows them, and letting a layout query redefine them is how
//     you end up with a keyboard that cannot type Escape.

#include "WinKeymap.h"

#include <windows.h>

#include <string>
#include <vector>

namespace amber::amberx
{
namespace
{

// The scan codes that carry characters: the main block, no extended keys.
// evdev numbers these identically (see EvdevFromScan in WinBackend.cpp), so
// the scan code is also the evdev code here.
constexpr UINT kFirstScan = 2;    // 1 is Escape
constexpr UINT kLastScan = 53;    // AB10, before RTSH

// The four levels, as the modifier state each one asks about.
struct LevelState
{
    bool shift;
    bool altgr;
};
constexpr LevelState kLevels[AMBERWIN_KEY_LEVELS] = {
    { false, false }, { true, false }, { false, true }, { true, true }
};

void SetKeyState(BYTE (&state)[256], const LevelState& l)
{
    memset(state, 0, sizeof state);
    if (l.shift)
    {
        state[VK_SHIFT] = 0x80;
        state[VK_LSHIFT] = 0x80;
    }
    if (l.altgr)
    {
        // AltGr is right-alt, which Windows delivers as Ctrl+Alt.
        state[VK_CONTROL] = 0x80;
        state[VK_LCONTROL] = 0x80;
        state[VK_MENU] = 0x80;
        state[VK_RMENU] = 0x80;
    }
}

// Empties any dead-key state the layout may be holding, for builds where
// the "do not change state" flag is not honoured. Asking about the space
// bar twice is the conventional way to do it.
void FlushDeadKeys(HKL hkl)
{
    BYTE state[256] = {};
    wchar_t buf[8];
    const UINT sc = MapVirtualKeyExW(VK_SPACE, MAPVK_VK_TO_VSC, hkl);
    for (int i = 0; i < 4; ++i)
        if (ToUnicodeEx(VK_SPACE, sc, state, buf, 8, 0, hkl) >= 0)
            break;
}

bool Printable(uint32_t c)
{
    // Control characters and the delete character are not layout symbols;
    // C1 controls (0x80-0x9f) are not either.
    return c >= 0x20 && c != 0x7f && !(c >= 0x80 && c <= 0x9f);
}

std::vector<amberwin_key> g_keys;
amberwin_keymap g_map;
bool g_have;

} // namespace

const amberwin_keymap* ReadCurrentLayout()
{
    const HKL hkl = GetKeyboardLayout(0);
    if (!hkl)
        return nullptr;

    std::vector<amberwin_key> keys;
    keys.reserve(kLastScan - kFirstScan + 1);
    bool anyLevel3 = false;

    for (UINT scan = kFirstScan; scan <= kLastScan; ++scan)
    {
        const UINT vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, hkl);
        if (!vk)
            continue;

        amberwin_key k{};
        k.evdev = static_cast<uint16_t>(scan);
        bool anyLevel = false;

        for (int lv = 0; lv < AMBERWIN_KEY_LEVELS; ++lv)
        {
            BYTE state[256];
            SetKeyState(state, kLevels[lv]);
            wchar_t buf[8] = {};
            // wFlags bit 2: do not disturb the layout's own dead-key state.
            const int n = ToUnicodeEx(vk, scan, state, buf, 8, 4, hkl);
            if (n == 0)
                continue;

            uint32_t cp = buf[0];
            // A surrogate pair is one code point; layouts rarely produce
            // them, but a keysym is a code point either way.
            if (n >= 2 && buf[0] >= 0xd800 && buf[0] <= 0xdbff && buf[1] >= 0xdc00 && buf[1] <= 0xdfff)
                cp = 0x10000 + ((buf[0] - 0xd800u) << 10) + (buf[1] - 0xdc00u);
            else if (n > 1 && !(n < 0))
                continue;      // a multi-character result is not one keysym

            if (!Printable(cp))
                continue;      // includes the control characters AltGr asks for

            k.ucs[lv] = cp;
            k.dead[lv] = (n < 0) ? 1 : 0;
            anyLevel = true;
            if (lv >= 2)
                anyLevel3 = true;
        }

        // The first level decides whether this is a character key at all.
        if (!anyLevel || !k.ucs[0])
            continue;
        keys.push_back(k);
    }
    FlushDeadKeys(hkl);

    if (keys.empty())
        return nullptr;

    g_keys = std::move(keys);
    g_map = {};
    g_map.keys = g_keys.data();
    g_map.nkeys = static_cast<int>(g_keys.size());
    g_map.has_level3 = anyLevel3 ? 1 : 0;

    // Repeat delay is 0-3 meaning roughly 250-1000 ms; repeat speed is 0-31
    // meaning roughly 2.5-30 characters a second. Both are what the user set
    // for their own keyboard, so a forwarded application repeats the way
    // every other window on the desktop does.
    DWORD delay = 1, speed = 31;
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    if (delay > 3)
        delay = 3;
    if (speed > 31)
        speed = 31;
    g_map.repeat_delay_ms = static_cast<int>(250 * (delay + 1));
    const double cps = 2.5 + (30.0 - 2.5) * (static_cast<double>(speed) / 31.0);
    g_map.repeat_interval_ms = static_cast<int>(1000.0 / cps + 0.5);

    wchar_t nameW[KL_NAMELENGTH] = {};
    if (GetKeyboardLayoutNameW(nameW))
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, nameW, -1, g_map.name,
                                          static_cast<int>(sizeof g_map.name), nullptr, nullptr);
        if (n <= 0)
            g_map.name[0] = '\0';
    }
    if (!g_map.name[0])
        snprintf(g_map.name, sizeof g_map.name, "%08llx",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hkl)));

    g_have = true;
    return &g_map;
}

const amberwin_keymap* CurrentLayout()
{
    return g_have ? &g_map : nullptr;
}

} // namespace amber::amberx

extern "C" const amberwin_keymap* amberwin_get_keymap(void)
{
    return amber::amberx::CurrentLayout();
}
