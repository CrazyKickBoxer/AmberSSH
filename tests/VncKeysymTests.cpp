// VncKeysymTests.cpp — Windows keys to X11 keysyms. The values are
// <X11/keysymdef.h>'s; the virtual keys are WinUser.h's.
#include <catch2/catch_test_macros.hpp>

#include "vnc/Keysyms.h"

using namespace amber::vnc;

TEST_CASE("navigation, editing and function keys map to their keysyms", "[vnc][keysym]")
{
    CHECK(KeysymFromVirtualKey(0x0D, false) == XK_Return);
    CHECK(KeysymFromVirtualKey(0x1B, false) == XK_Escape);
    CHECK(KeysymFromVirtualKey(0x08, false) == XK_BackSpace);
    CHECK(KeysymFromVirtualKey(0x09, false) == XK_Tab);
    CHECK(KeysymFromVirtualKey(0x25, false) == XK_Left);
    CHECK(KeysymFromVirtualKey(0x28, false) == XK_Down);
    CHECK(KeysymFromVirtualKey(0x24, false) == XK_Home);
    CHECK(KeysymFromVirtualKey(0x22, false) == XK_Page_Down);
    CHECK(KeysymFromVirtualKey(0x2E, false) == XK_Delete);
    CHECK(KeysymFromVirtualKey(0x70, false) == XK_F1);
    CHECK(KeysymFromVirtualKey(0x7B, false) == XK_F1 + 11);   // F12
    CHECK(KeysymFromVirtualKey(0x5D, false) == XK_Menu);
}

TEST_CASE("modifiers are told apart by side", "[vnc][keysym]")
{
    CHECK(KeysymFromVirtualKey(0x10, false) == XK_Shift_L);
    CHECK(KeysymFromVirtualKey(0x10, true) == XK_Shift_R);
    CHECK(KeysymFromVirtualKey(0x11, false) == XK_Control_L);
    CHECK(KeysymFromVirtualKey(0x11, true) == XK_Control_R);
    CHECK(KeysymFromVirtualKey(0x12, true) == XK_Alt_R);
    CHECK(KeysymFromVirtualKey(0xA0, true) == XK_Shift_L);    // the explicit left VK is left whatever the flag says
    CHECK(KeysymFromVirtualKey(0x5B, false) == XK_Super_L);
}

TEST_CASE("text keys are left to WM_CHAR", "[vnc][keysym]")
{
    for (unsigned vk : { 0x41u, 0x5Au, 0x30u, 0x39u, 0x20u, 0xBAu, 0xBFu, 0x60u, 0x6Bu })
        CHECK(KeysymFromVirtualKey(vk, false) == 0);   // A, Z, 0, 9, space, OEM_1, OEM_2, numpad 0, numpad +
}

TEST_CASE("code points become keysyms by the X11 convention", "[vnc][keysym]")
{
    CHECK(KeysymFromCodePoint(U'a') == 0x61);
    CHECK(KeysymFromCodePoint(U'A') == 0x41);
    CHECK(KeysymFromCodePoint(U'é') == 0xE9);              // é: Latin-1 as itself
    CHECK(KeysymFromCodePoint(U'€') == 0x01000000 + 0x20AC); // €: Unicode keysym
    CHECK(KeysymFromCodePoint(U'\U0001F600') == 0x01000000 + 0x1F600);
}
