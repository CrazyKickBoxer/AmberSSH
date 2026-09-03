#include "input.h"

#include <windows.h>
#include <cstdio>

// xterm modifier parameter: 1 + shift(1) + alt(2) + ctrl(4).
static int ModParam(const KeyMods& m)
{
    int v = 1;
    if (m.shift) v += 1;
    if (m.alt) v += 2;
    if (m.ctrl) v += 4;
    return v;
}

static std::string CursorKey(char letter, const KeyMods& mods, bool app)
{
    char buf[16];
    int mp = ModParam(mods);
    if (mp != 1)
        snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mp, letter);
    else if (app)
        snprintf(buf, sizeof(buf), "\x1bO%c", letter);
    else
        snprintf(buf, sizeof(buf), "\x1b[%c", letter);
    return buf;
}

static std::string TildeKey(int num, const KeyMods& mods)
{
    char buf[16];
    int mp = ModParam(mods);
    if (mp != 1)
        snprintf(buf, sizeof(buf), "\x1b[%d;%d~", num, mp);
    else
        snprintf(buf, sizeof(buf), "\x1b[%d~", num);
    return buf;
}

static std::string Ss3Key(char letter, const KeyMods& mods)   // SS3 P..S etc.
{
    char buf[16];
    int mp = ModParam(mods);
    if (mp != 1)
        snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mp, letter);
    else
        snprintf(buf, sizeof(buf), "\x1bO%c", letter);
    return buf;
}

// Function keys in the four PuTTY flavours.
static std::string FnKey(int n, const KeyMods& mods, int mode)
{
    static const int xtermTilde[13] = { 0, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24 };
    switch (mode)
    {
    case 1:   // Linux console: F1-F5 = ESC[[A..E, the rest xterm-style
        if (n <= 5)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\x1b[[%c", 'A' + (n - 1));
            return buf;
        }
        return TildeKey(xtermTilde[n], mods);
    case 2:   // VT100+: F1-F12 = SS3 P..[  (P Q R S T U V W X Y Z [)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "\x1bO%c", 'P' + (n - 1));
        return buf;
    }
    case 3:   // SCO: F1-F12 = ESC[M..X; shift/ctrl variants continue the run
    {
        int base = 'M' + (n - 1);
        if (mods.shift) base += 12;
        if (mods.ctrl)  base += 24;
        if (base > 'z') base = 'z';
        char buf[8];
        snprintf(buf, sizeof(buf), "\x1b[%c", base);
        return buf;
    }
    default:  // xterm: F1-F4 SS3 P..S, F5.. CSI n~
        if (n <= 4)
            return Ss3Key(static_cast<char>('P' + (n - 1)), mods);
        return TildeKey(xtermTilde[n], mods);
    }
}

std::string TranslateKey(unsigned vk, const KeyMods& mods, const KeyOptions& opt)
{
    const bool app = opt.appCursorKeys;
    switch (vk)
    {
    case VK_UP:     return CursorKey('A', mods, app);
    case VK_DOWN:   return CursorKey('B', mods, app);
    case VK_RIGHT:  return CursorKey('C', mods, app);
    case VK_LEFT:   return CursorKey('D', mods, app);
    case VK_HOME:
        if (opt.homeEnd == 1) return TildeKey(1, mods);
        if (opt.homeEnd == 2) return mods.ctrl || mods.shift || mods.alt ? CursorKey('H', mods, false) : "\x1b[H";
        return CursorKey('H', mods, app);
    case VK_END:
        if (opt.homeEnd == 1) return TildeKey(4, mods);
        if (opt.homeEnd == 2) return mods.ctrl || mods.shift || mods.alt ? CursorKey('F', mods, false) : "\x1bOw";
        return CursorKey('F', mods, app);
    case VK_PRIOR:  return TildeKey(5, mods);
    case VK_NEXT:   return TildeKey(6, mods);
    case VK_INSERT: return TildeKey(2, mods);
    case VK_DELETE: return TildeKey(3, mods);
    case VK_BACK:
    {
        // Shift+Backspace sends the other code, like PuTTY.
        bool del = opt.backspaceIsDel != mods.shift;
        std::string s;
        if (mods.alt)
            s.push_back(0x1B);
        s.push_back(del ? '\x7f' : '\x08');
        return s;
    }
    case VK_F1:  case VK_F2:  case VK_F3:  case VK_F4:
    case VK_F5:  case VK_F6:  case VK_F7:  case VK_F8:
    case VK_F9:  case VK_F10: case VK_F11: case VK_F12:
        return FnKey(static_cast<int>(vk - VK_F1) + 1, mods, opt.fnKeys);
    default:
        break;
    }

    // Application keypad (DECKPAM): the numeric keypad sends SS3 sequences.
    if (opt.appKeypad && !mods.ctrl && !mods.alt)
    {
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\x1bO%c", 'p' + static_cast<int>(vk - VK_NUMPAD0));
            return buf;
        }
        switch (vk)
        {
        case VK_DECIMAL:  return "\x1bOn";
        case VK_ADD:      return "\x1bOk";
        case VK_SUBTRACT: return "\x1bOm";
        case VK_MULTIPLY: return "\x1bOj";
        case VK_DIVIDE:   return "\x1bOo";
        default: break;
        }
    }
    return "";
}
