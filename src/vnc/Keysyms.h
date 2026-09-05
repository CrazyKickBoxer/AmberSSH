// Keysyms.h — Windows keys to X11 keysyms, for the RFB KeyEvent message.
//
// The split that makes this work follows vncfree's note that keyboard
// layouts belong to Windows, not to the client: keys that produce text are
// taken from WM_CHAR, which already knows the layout, dead keys and
// AltGr, and are sent as their Unicode code point (Latin-1 as itself,
// anything else as 0x01000000 + code point — the X11 convention RFC 6143
// §7.5.4 defers to). Keys that produce no text — navigation, function,
// editing and modifier keys — come from the virtual key and are the only
// ones this table has to know. A virtual key that WM_CHAR will report maps
// to 0 here, so nothing is sent twice.
//
// Modifiers are tracked as pressed and released, with the keysym captured
// at press time repeated at release; that is the caller's job, and it is
// what keeps a Shift released after a letter from stranding a capital.
#pragma once

#include <cstdint>

namespace amber::vnc
{

// The keysym for a virtual key that produces no character, or 0 for one
// that WM_CHAR carries. `right` distinguishes the right Shift/Control/Alt.
uint32_t KeysymFromVirtualKey(unsigned vk, bool right);

// The keysym for a Unicode code point typed as text.
uint32_t KeysymFromCodePoint(char32_t cp);

// The ones the mapping uses, from <X11/keysymdef.h>, so a test can name them.
constexpr uint32_t XK_BackSpace = 0xff08, XK_Tab = 0xff09, XK_Return = 0xff0d, XK_Pause = 0xff13,
                   XK_Scroll_Lock = 0xff14, XK_Escape = 0xff1b, XK_Home = 0xff50, XK_Left = 0xff51,
                   XK_Up = 0xff52, XK_Right = 0xff53, XK_Down = 0xff54, XK_Page_Up = 0xff55,
                   XK_Page_Down = 0xff56, XK_End = 0xff57, XK_Print = 0xff61, XK_Insert = 0xff63,
                   XK_Menu = 0xff67, XK_Num_Lock = 0xff7f, XK_KP_Enter = 0xff8d, XK_F1 = 0xffbe,
                   XK_Shift_L = 0xffe1, XK_Shift_R = 0xffe2, XK_Control_L = 0xffe3, XK_Control_R = 0xffe4,
                   XK_Caps_Lock = 0xffe5, XK_Alt_L = 0xffe9, XK_Alt_R = 0xffea, XK_Super_L = 0xffeb,
                   XK_Super_R = 0xffec, XK_Delete = 0xffff;

} // namespace amber::vnc
