// Keysyms.cpp — see Keysyms.h. Virtual-key values are the WinUser.h
// constants, written as numbers so this file needs no Windows header and
// the tests can compile it anywhere.
#include "Keysyms.h"

namespace amber::vnc
{

uint32_t KeysymFromVirtualKey(unsigned vk, bool right)
{
    switch (vk)
    {
    case 0x08: return XK_BackSpace;      // VK_BACK
    case 0x09: return XK_Tab;            // VK_TAB
    case 0x0D: return XK_Return;         // VK_RETURN (keypad Enter is not distinguished by VK)
    case 0x10: return right ? XK_Shift_R : XK_Shift_L;       // VK_SHIFT
    case 0x11: return right ? XK_Control_R : XK_Control_L;   // VK_CONTROL
    case 0x12: return right ? XK_Alt_R : XK_Alt_L;           // VK_MENU
    case 0x13: return XK_Pause;          // VK_PAUSE
    case 0x14: return XK_Caps_Lock;      // VK_CAPITAL
    case 0x1B: return XK_Escape;         // VK_ESCAPE
    case 0x21: return XK_Page_Up;        // VK_PRIOR
    case 0x22: return XK_Page_Down;      // VK_NEXT
    case 0x23: return XK_End;            // VK_END
    case 0x24: return XK_Home;           // VK_HOME
    case 0x25: return XK_Left;           // VK_LEFT
    case 0x26: return XK_Up;             // VK_UP
    case 0x27: return XK_Right;          // VK_RIGHT
    case 0x28: return XK_Down;           // VK_DOWN
    case 0x2C: return XK_Print;          // VK_SNAPSHOT
    case 0x2D: return XK_Insert;         // VK_INSERT
    case 0x2E: return XK_Delete;         // VK_DELETE
    case 0x5B: return XK_Super_L;        // VK_LWIN
    case 0x5C: return XK_Super_R;        // VK_RWIN
    case 0x5D: return XK_Menu;           // VK_APPS
    case 0x90: return XK_Num_Lock;       // VK_NUMLOCK
    case 0x91: return XK_Scroll_Lock;    // VK_SCROLL
    case 0xA0: return XK_Shift_L;        // VK_LSHIFT
    case 0xA1: return XK_Shift_R;        // VK_RSHIFT
    case 0xA2: return XK_Control_L;      // VK_LCONTROL
    case 0xA3: return XK_Control_R;      // VK_RCONTROL
    case 0xA4: return XK_Alt_L;          // VK_LMENU
    case 0xA5: return XK_Alt_R;          // VK_RMENU
    default:
        break;
    }
    if (vk >= 0x70 && vk <= 0x87)        // VK_F1 .. VK_F24
        return XK_F1 + (vk - 0x70);
    // Everything else — letters, digits, OEM keys, space, the keypad's
    // digits and operators — produces a character and comes through WM_CHAR.
    return 0;
}

uint32_t KeysymFromCodePoint(char32_t cp)
{
    if (cp < 0x100)
        return static_cast<uint32_t>(cp);
    return 0x01000000u + static_cast<uint32_t>(cp);
}

} // namespace amber::vnc
