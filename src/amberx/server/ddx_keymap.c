/* ddx_keymap.c — keymap delivery without xkbcomp. Original AmberSSH file;
 * replaces xserver/xkb/ddxLoad.c, which is off the allowlist because it
 * spawns a process and the host is not allowed to.
 *
 * Two sources, in order:
 *
 *   1. an .xkm file named in the configuration — the binary form xkbcomp
 *      produces from xkeyboard-config, read with the server's own XkmReadFile
 *      (xkb/xkmread.c). This is the full-fidelity path for any layout;
 *      AmberSSH will generate and ship these later. Nothing is compiled at
 *      runtime and nothing is executed;
 *
 *   2. the built-in map: a US layout constructed directly in the server's
 *      XkbDesc structures from the table below. It exists so the server can
 *      start on a machine that has no .xkm at all, and it is what the
 *      Phase 2 gate runs on. It is not derived from any keymap file; the
 *      keysym names come from xorgproto's keysymdef.h and the key names
 *      follow the conventional xkeyboard-config ones so clients that show
 *      them see familiar values.
 *
 * XkbCompileKeymapFromString (XkbSetMap with a text keymap) is refused: it
 * would need the compiler.
 */
#include <dix-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include "misc.h"
#include "os.h"
#include "inputstr.h"
#include "xkbsrv.h"
#include "xkbstr.h"
#include "xkbfile.h"
#include <X11/extensions/XKMformat.h>
#include "amberwin.h"

/* ---- the built-in US table ------------------------------------------------ */
enum { T_ONE, T_TWO, T_ALPHA, T_KEYPAD };

/* two types beyond the four canonical ones, for layouts with an AltGr level */
#define T_FOUR_LEVEL       (XkbNumRequiredTypes)
#define T_FOUR_LEVEL_ALPHA (XkbNumRequiredTypes + 1)
#define AMBER_NUM_TYPES    (XkbNumRequiredTypes + 2)

/* RALT's X keycode: the key that becomes ISO_Level3_Shift on a layout that
 * has a third level, and stays Alt_R on one that does not */
#define KEYCODE_RALT (100 + 8)

typedef struct {
    unsigned char evdev;      /* evdev keycode; X keycode is +8 */
    char name[5];             /* XKB key name */
    KeySym lo, hi;            /* level 1, level 2 */
    unsigned char type;
    unsigned char mod;        /* modifier mask for the modmap, or 0 */
} BuiltinKey;

static const BuiltinKey builtin[] = {
    {  1, "ESC",  XK_Escape, NoSymbol, T_ONE, 0 },
    {  2, "AE01", XK_1, XK_exclam, T_TWO, 0 },
    {  3, "AE02", XK_2, XK_at, T_TWO, 0 },
    {  4, "AE03", XK_3, XK_numbersign, T_TWO, 0 },
    {  5, "AE04", XK_4, XK_dollar, T_TWO, 0 },
    {  6, "AE05", XK_5, XK_percent, T_TWO, 0 },
    {  7, "AE06", XK_6, XK_asciicircum, T_TWO, 0 },
    {  8, "AE07", XK_7, XK_ampersand, T_TWO, 0 },
    {  9, "AE08", XK_8, XK_asterisk, T_TWO, 0 },
    { 10, "AE09", XK_9, XK_parenleft, T_TWO, 0 },
    { 11, "AE10", XK_0, XK_parenright, T_TWO, 0 },
    { 12, "AE11", XK_minus, XK_underscore, T_TWO, 0 },
    { 13, "AE12", XK_equal, XK_plus, T_TWO, 0 },
    { 14, "BKSP", XK_BackSpace, NoSymbol, T_ONE, 0 },
    { 15, "TAB",  XK_Tab, XK_ISO_Left_Tab, T_TWO, 0 },
    { 16, "AD01", XK_q, XK_Q, T_ALPHA, 0 },
    { 17, "AD02", XK_w, XK_W, T_ALPHA, 0 },
    { 18, "AD03", XK_e, XK_E, T_ALPHA, 0 },
    { 19, "AD04", XK_r, XK_R, T_ALPHA, 0 },
    { 20, "AD05", XK_t, XK_T, T_ALPHA, 0 },
    { 21, "AD06", XK_y, XK_Y, T_ALPHA, 0 },
    { 22, "AD07", XK_u, XK_U, T_ALPHA, 0 },
    { 23, "AD08", XK_i, XK_I, T_ALPHA, 0 },
    { 24, "AD09", XK_o, XK_O, T_ALPHA, 0 },
    { 25, "AD10", XK_p, XK_P, T_ALPHA, 0 },
    { 26, "AD11", XK_bracketleft, XK_braceleft, T_TWO, 0 },
    { 27, "AD12", XK_bracketright, XK_braceright, T_TWO, 0 },
    { 28, "RTRN", XK_Return, NoSymbol, T_ONE, 0 },
    { 29, "LCTL", XK_Control_L, NoSymbol, T_ONE, ControlMask },
    { 30, "AC01", XK_a, XK_A, T_ALPHA, 0 },
    { 31, "AC02", XK_s, XK_S, T_ALPHA, 0 },
    { 32, "AC03", XK_d, XK_D, T_ALPHA, 0 },
    { 33, "AC04", XK_f, XK_F, T_ALPHA, 0 },
    { 34, "AC05", XK_g, XK_G, T_ALPHA, 0 },
    { 35, "AC06", XK_h, XK_H, T_ALPHA, 0 },
    { 36, "AC07", XK_j, XK_J, T_ALPHA, 0 },
    { 37, "AC08", XK_k, XK_K, T_ALPHA, 0 },
    { 38, "AC09", XK_l, XK_L, T_ALPHA, 0 },
    { 39, "AC10", XK_semicolon, XK_colon, T_TWO, 0 },
    { 40, "AC11", XK_apostrophe, XK_quotedbl, T_TWO, 0 },
    { 41, "TLDE", XK_grave, XK_asciitilde, T_TWO, 0 },
    { 42, "LFSH", XK_Shift_L, NoSymbol, T_ONE, ShiftMask },
    { 43, "BKSL", XK_backslash, XK_bar, T_TWO, 0 },
    { 44, "AB01", XK_z, XK_Z, T_ALPHA, 0 },
    { 45, "AB02", XK_x, XK_X, T_ALPHA, 0 },
    { 46, "AB03", XK_c, XK_C, T_ALPHA, 0 },
    { 47, "AB04", XK_v, XK_V, T_ALPHA, 0 },
    { 48, "AB05", XK_b, XK_B, T_ALPHA, 0 },
    { 49, "AB06", XK_n, XK_N, T_ALPHA, 0 },
    { 50, "AB07", XK_m, XK_M, T_ALPHA, 0 },
    { 51, "AB08", XK_comma, XK_less, T_TWO, 0 },
    { 52, "AB09", XK_period, XK_greater, T_TWO, 0 },
    { 53, "AB10", XK_slash, XK_question, T_TWO, 0 },
    { 54, "RTSH", XK_Shift_R, NoSymbol, T_ONE, ShiftMask },
    { 55, "KPMU", XK_KP_Multiply, NoSymbol, T_ONE, 0 },
    { 56, "LALT", XK_Alt_L, XK_Meta_L, T_ONE, Mod1Mask },
    { 57, "SPCE", XK_space, NoSymbol, T_ONE, 0 },
    { 58, "CAPS", XK_Caps_Lock, NoSymbol, T_ONE, LockMask },
    { 59, "FK01", XK_F1, NoSymbol, T_ONE, 0 },
    { 60, "FK02", XK_F2, NoSymbol, T_ONE, 0 },
    { 61, "FK03", XK_F3, NoSymbol, T_ONE, 0 },
    { 62, "FK04", XK_F4, NoSymbol, T_ONE, 0 },
    { 63, "FK05", XK_F5, NoSymbol, T_ONE, 0 },
    { 64, "FK06", XK_F6, NoSymbol, T_ONE, 0 },
    { 65, "FK07", XK_F7, NoSymbol, T_ONE, 0 },
    { 66, "FK08", XK_F8, NoSymbol, T_ONE, 0 },
    { 67, "FK09", XK_F9, NoSymbol, T_ONE, 0 },
    { 68, "FK10", XK_F10, NoSymbol, T_ONE, 0 },
    { 69, "NMLK", XK_Num_Lock, NoSymbol, T_ONE, Mod2Mask },
    { 70, "SCLK", XK_Scroll_Lock, NoSymbol, T_ONE, 0 },
    { 71, "KP7",  XK_KP_Home, XK_KP_7, T_KEYPAD, 0 },
    { 72, "KP8",  XK_KP_Up, XK_KP_8, T_KEYPAD, 0 },
    { 73, "KP9",  XK_KP_Prior, XK_KP_9, T_KEYPAD, 0 },
    { 74, "KPSU", XK_KP_Subtract, NoSymbol, T_ONE, 0 },
    { 75, "KP4",  XK_KP_Left, XK_KP_4, T_KEYPAD, 0 },
    { 76, "KP5",  XK_KP_Begin, XK_KP_5, T_KEYPAD, 0 },
    { 77, "KP6",  XK_KP_Right, XK_KP_6, T_KEYPAD, 0 },
    { 78, "KPAD", XK_KP_Add, NoSymbol, T_ONE, 0 },
    { 79, "KP1",  XK_KP_End, XK_KP_1, T_KEYPAD, 0 },
    { 80, "KP2",  XK_KP_Down, XK_KP_2, T_KEYPAD, 0 },
    { 81, "KP3",  XK_KP_Next, XK_KP_3, T_KEYPAD, 0 },
    { 82, "KP0",  XK_KP_Insert, XK_KP_0, T_KEYPAD, 0 },
    { 83, "KPDL", XK_KP_Delete, XK_KP_Decimal, T_KEYPAD, 0 },
    { 87, "FK11", XK_F11, NoSymbol, T_ONE, 0 },
    { 88, "FK12", XK_F12, NoSymbol, T_ONE, 0 },
    { 96, "KPEN", XK_KP_Enter, NoSymbol, T_ONE, 0 },
    { 97, "RCTL", XK_Control_R, NoSymbol, T_ONE, ControlMask },
    { 98, "KPDV", XK_KP_Divide, NoSymbol, T_ONE, 0 },
    { 99, "PRSC", XK_Print, XK_Sys_Req, T_TWO, 0 },
    { 100, "RALT", XK_Alt_R, XK_Meta_R, T_ONE, Mod1Mask },
    { 102, "HOME", XK_Home, NoSymbol, T_ONE, 0 },
    { 103, "UP",   XK_Up, NoSymbol, T_ONE, 0 },
    { 104, "PGUP", XK_Prior, NoSymbol, T_ONE, 0 },
    { 105, "LEFT", XK_Left, NoSymbol, T_ONE, 0 },
    { 106, "RGHT", XK_Right, NoSymbol, T_ONE, 0 },
    { 107, "END",  XK_End, NoSymbol, T_ONE, 0 },
    { 108, "DOWN", XK_Down, NoSymbol, T_ONE, 0 },
    { 109, "PGDN", XK_Next, NoSymbol, T_ONE, 0 },
    { 110, "INS",  XK_Insert, NoSymbol, T_ONE, 0 },
    { 111, "DELE", XK_Delete, NoSymbol, T_ONE, 0 },
    { 119, "PAUS", XK_Pause, XK_Break, T_TWO, 0 },
    { 125, "LWIN", XK_Super_L, NoSymbol, T_ONE, Mod4Mask },
    { 126, "RWIN", XK_Super_R, NoSymbol, T_ONE, Mod4Mask },
    { 127, "COMP", XK_Menu, NoSymbol, T_ONE, 0 },
};
#define NBUILTIN (sizeof builtin / sizeof builtin[0])

/* The compat interpretations: how a keysym on a key becomes an action.
 * Three are enough for a US map — any key with a modmap entry sets those
 * modifiers while held; Caps Lock and Num Lock latch theirs. */
static void
set_interp(XkbSymInterpretPtr si, KeySym sym, unsigned char match,
           unsigned char mods, Bool lock)
{
    XkbModAction *act = (XkbModAction *) &si->act;
    memset(si, 0, sizeof *si);
    si->sym = sym;
    si->match = match;
    si->mods = mods;
    si->flags = XkbSI_AutoRepeat;
    act->type = lock ? XkbSA_LockMods : XkbSA_SetMods;
    act->flags = XkbSA_UseModMapMods;
}

static Bool
set_type(XkbDescPtr xkb, int ndx, const char *name, unsigned char mask,
         int levels, int nmap, const unsigned char *entry_mods,
         const unsigned char *entry_levels)
{
    XkbKeyTypePtr t;
    int i;
    if (XkbResizeKeyType(xkb, ndx, nmap, FALSE, levels) != Success)
        return FALSE;
    t = &xkb->map->types[ndx];
    t->mods.mask = mask;
    t->mods.real_mods = mask;
    t->mods.vmods = 0;
    t->num_levels = (unsigned char) levels;
    t->map_count = (unsigned char) nmap;
    for (i = 0; i < nmap; i++) {
        t->map[i].active = TRUE;
        t->map[i].level = entry_levels[i];
        t->map[i].mods.mask = entry_mods[i];
        t->map[i].mods.real_mods = entry_mods[i];
        t->map[i].mods.vmods = 0;
    }
    t->name = MakeAtom(name, (unsigned) strlen(name), TRUE);
    return TRUE;
}

static Bool
init_canonical_types(XkbDescPtr xkb)
{
    static const unsigned char two_mods[] = { ShiftMask };
    static const unsigned char two_lvls[] = { 1 };
    static const unsigned char alpha_mods[] = { ShiftMask, LockMask };
    static const unsigned char alpha_lvls[] = { 1, 1 };
    static const unsigned char kp_mods[] = { ShiftMask, Mod2Mask };
    static const unsigned char kp_lvls[] = { 1, 1 };
    /* AltGr is a real modifier here (Mod5 on the RALT key) rather than a
     * virtual one: there is no rules file to bind a virtual modifier
     * through, and every client reads the type's map, not its provenance. */
    static const unsigned char four_mods[] = {
        ShiftMask, Mod5Mask, ShiftMask | Mod5Mask
    };
    static const unsigned char four_lvls[] = { 1, 2, 3 };
    static const unsigned char foura_mods[] = {
        ShiftMask, LockMask, ShiftMask | LockMask,
        Mod5Mask, ShiftMask | Mod5Mask, LockMask | Mod5Mask
    };
    static const unsigned char foura_lvls[] = { 1, 1, 0, 2, 3, 3 };

    if (xkb->map->num_types < AMBER_NUM_TYPES)
        xkb->map->num_types = AMBER_NUM_TYPES;
    return set_type(xkb, XkbOneLevelIndex, "ONE_LEVEL", 0, 1, 0, NULL, NULL) &&
           set_type(xkb, XkbTwoLevelIndex, "TWO_LEVEL", ShiftMask, 2, 1, two_mods, two_lvls) &&
           set_type(xkb, XkbAlphabeticIndex, "ALPHABETIC", ShiftMask | LockMask, 2, 2, alpha_mods, alpha_lvls) &&
           set_type(xkb, XkbKeypadIndex, "KEYPAD", ShiftMask | Mod2Mask, 2, 2, kp_mods, kp_lvls) &&
           set_type(xkb, T_FOUR_LEVEL, "FOUR_LEVEL", ShiftMask | Mod5Mask, 4, 3,
                    four_mods, four_lvls) &&
           set_type(xkb, T_FOUR_LEVEL_ALPHA, "FOUR_LEVEL_ALPHABETIC",
                    ShiftMask | LockMask | Mod5Mask, 4, 6, foura_mods, foura_lvls);
}

/* ---- the live Windows layout ------------------------------------------------
 *
 * The Windows side reports Unicode code points (amberwin.h); here they become
 * keysyms and key types, and are written over the built-in table's character
 * keys. Structure — Escape, Return, the function keys, the modifiers, the
 * keypad — is never touched, so an odd layout can leave a key blank but can
 * never take the keyboard away. */

/* The X convention: Latin-1 is its own keysym, everything else is the code
 * point with the Unicode flag. Nothing here needs a table. */
static KeySym
keysym_from_ucs(uint32_t cp)
{
    if (cp == 0)
        return NoSymbol;
    if (cp < 0x100)
        return (KeySym) cp;
    return (KeySym) (cp | 0x01000000);
}

/* A dead key is reported as the spacing character it would otherwise type;
 * these are the ones X has a dead keysym for. An unrecognised dead key
 * keeps its literal character, which types something rather than nothing. */
static KeySym
keysym_from_dead(uint32_t cp)
{
    switch (cp) {
    case 0x0060: return XK_dead_grave;
    case 0x0027: return XK_dead_acute;        /* US-International */
    case 0x00B4: return XK_dead_acute;
    case 0x005E: return XK_dead_circumflex;
    case 0x007E: return XK_dead_tilde;
    case 0x0022: return XK_dead_diaeresis;    /* US-International */
    case 0x00A8: return XK_dead_diaeresis;
    case 0x00AF: return XK_dead_macron;
    case 0x02D8: return XK_dead_breve;
    case 0x02D9: return XK_dead_abovedot;
    case 0x00B0: return XK_dead_abovering;
    case 0x02DA: return XK_dead_abovering;
    case 0x02DD: return XK_dead_doubleacute;
    case 0x02C7: return XK_dead_caron;
    case 0x00B8: return XK_dead_cedilla;
    case 0x02DB: return XK_dead_ogonek;
    case 0x0387: return XK_dead_belowdot;
    default:     return keysym_from_ucs(cp);
    }
}

/* Simple upper-casing, enough to recognise "this key is a letter": ASCII,
 * Latin-1, and the even/odd pairs of Latin Extended-A. A letter AmberX
 * fails to recognise gets TWO_LEVEL instead of ALPHABETIC, which types
 * correctly and only differs under Caps Lock. */
static uint32_t
ucs_upper(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z')
        return cp - 32;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
        return cp - 32;
    if (cp >= 0x100 && cp < 0x180 && (cp & 1))
        return cp - 1;
    return cp;
}

static Bool
layout_is_alphabetic(const struct amberwin_key *k)
{
    return k->ucs[1] != 0 && k->ucs[0] != k->ucs[1] &&
           ucs_upper(k->ucs[0]) == k->ucs[1];
}

static void
apply_layout(XkbDescPtr xkb, const struct amberwin_keymap *km)
{
    int i, lv;
    int applied = 0;

    for (i = 0; i < km->nkeys; i++) {
        const struct amberwin_key *k = &km->keys[i];
        KeyCode kc = (KeyCode) (k->evdev + 8);
        KeySym syms[AMBERWIN_KEY_LEVELS];
        int width = 0;
        int type;
        KeySym *dst;

        if (k->evdev == 0 || (unsigned) (k->evdev + 8) > 255)
            continue;
        for (lv = 0; lv < AMBERWIN_KEY_LEVELS; lv++) {
            syms[lv] = k->dead[lv] ? keysym_from_dead(k->ucs[lv])
                                   : keysym_from_ucs(k->ucs[lv]);
            if (syms[lv] != NoSymbol)
                width = lv + 1;
        }
        if (width == 0 || syms[0] == NoSymbol)
            continue;

        if (width > 2) {
            width = 4;      /* a four-level type must have four levels */
            type = layout_is_alphabetic(k) ? T_FOUR_LEVEL_ALPHA : T_FOUR_LEVEL;
        }
        else if (width == 2) {
            type = layout_is_alphabetic(k) ? XkbAlphabeticIndex : XkbTwoLevelIndex;
        }
        else {
            type = XkbOneLevelIndex;
        }

        dst = XkbResizeKeySyms(xkb, kc, width);
        if (!dst)
            continue;
        for (lv = 0; lv < width; lv++)
            dst[lv] = syms[lv];
        xkb->map->key_sym_map[kc].kt_index[0] = (unsigned char) type;
        xkb->map->key_sym_map[kc].group_info = XkbSetNumGroups(0, 1);
        xkb->map->key_sym_map[kc].width = (unsigned char) width;
        applied++;
    }

    /* A layout with a third level needs a key that reaches it. Windows
     * sends right-alt as Ctrl+Alt; the Windows side drops the phantom
     * control, and here right-alt becomes the level-three shift rather
     * than a second Alt. On a layout with no third level it stays Alt_R,
     * because taking Alt away from a US keyboard would be a regression. */
    if (km->has_level3) {
        KeySym *dst = XkbResizeKeySyms(xkb, KEYCODE_RALT, 1);
        if (dst) {
            dst[0] = XK_ISO_Level3_Shift;
            xkb->map->key_sym_map[KEYCODE_RALT].kt_index[0] = XkbOneLevelIndex;
            xkb->map->key_sym_map[KEYCODE_RALT].group_info = XkbSetNumGroups(0, 1);
            xkb->map->key_sym_map[KEYCODE_RALT].width = 1;
            xkb->map->modmap[KEYCODE_RALT] = Mod5Mask;
        }
    }

    if (km->repeat_delay_ms > 0 && km->repeat_interval_ms > 0) {
        xkb->ctrls->repeat_delay = (unsigned short) km->repeat_delay_ms;
        xkb->ctrls->repeat_interval = (unsigned short) km->repeat_interval_ms;
    }

    LogMessage(X_INFO, "AmberX: keyboard layout %s applied to %d keys%s\n",
               km->name[0] ? km->name : "(unnamed)", applied,
               km->has_level3 ? ", AltGr is level three" : "");
}

static XkbDescPtr
build_builtin(void)
{
    XkbDescPtr xkb;
    XkbChangesRec changes;
    unsigned i;

    xkb = XkbAllocKeyboard();
    if (!xkb)
        return NULL;
    xkb->min_key_code = 8;
    xkb->max_key_code = 255;

    if (XkbAllocClientMap(xkb, XkbAllClientInfoMask, AMBER_NUM_TYPES) != Success ||
        XkbAllocServerMap(xkb, XkbAllServerInfoMask, 0) != Success ||
        XkbAllocCompatMap(xkb, XkbAllCompatMask, 3) != Success ||
        XkbAllocNames(xkb, XkbAllNamesMask, 0, 0) != Success ||
        XkbAllocControls(xkb, XkbAllControlsMask) != Success ||
        XkbAllocIndicatorMaps(xkb) != Success)
        goto fail;

    /* the four canonical types: ONE_LEVEL, TWO_LEVEL, ALPHABETIC, KEYPAD.
     * (XkbInitCanonicalKeyTypes is a libxkbfile function, not a server one;
     * the server expects a compiled keymap to carry its types, so the
     * built-in map carries them here.) KEYPAD uses the real Num Lock
     * modifier (Mod2, from the modmap) rather than a virtual one. */
    if (!init_canonical_types(xkb))
        goto fail;

    for (i = 0; i < NBUILTIN; i++) {
        const BuiltinKey *k = &builtin[i];
        KeyCode kc = (KeyCode) (k->evdev + 8);
        int nsyms = (k->hi == NoSymbol) ? 1 : 2;
        int type;
        KeySym *syms;

        switch (k->type) {
        case T_TWO:    type = XkbTwoLevelIndex; break;
        case T_ALPHA:  type = XkbAlphabeticIndex; break;
        case T_KEYPAD: type = XkbKeypadIndex; break;
        default:       type = XkbOneLevelIndex; break;
        }
        if (nsyms == 1)
            type = XkbOneLevelIndex;

        syms = XkbResizeKeySyms(xkb, kc, nsyms);
        if (!syms)
            goto fail;
        syms[0] = k->lo;
        if (nsyms == 2)
            syms[1] = k->hi;
        xkb->map->key_sym_map[kc].kt_index[0] = (unsigned char) type;
        xkb->map->key_sym_map[kc].group_info = XkbSetNumGroups(0, 1);
        xkb->map->key_sym_map[kc].width = (unsigned char) nsyms;
        xkb->map->modmap[kc] = k->mod;
        memcpy(xkb->names->keys[kc].name, k->name, XkbKeyNameLength);
        /* modifiers do not autorepeat; everything else does */
        if (k->mod)
            xkb->ctrls->per_key_repeat[kc / 8] &= (unsigned char) ~(1 << (kc % 8));
        else
            xkb->ctrls->per_key_repeat[kc / 8] |= (unsigned char) (1 << (kc % 8));
    }

    set_interp(&xkb->compat->sym_interpret[0], NoSymbol, XkbSI_AnyOf, 0xff, FALSE);
    set_interp(&xkb->compat->sym_interpret[1], XK_Caps_Lock, XkbSI_AnyOfOrNone, 0, TRUE);
    set_interp(&xkb->compat->sym_interpret[2], XK_Num_Lock, XkbSI_AnyOfOrNone, 0, TRUE);
    xkb->compat->num_si = 3;

    xkb->names->keycodes = MakeAtom("amberx", 6, TRUE);
    xkb->names->symbols = MakeAtom("amberx(us)", 10, TRUE);
    xkb->names->types = MakeAtom("amberx", 6, TRUE);
    xkb->names->compat = MakeAtom("amberx", 6, TRUE);
    xkb->names->keycodes = MakeAtom("amberx", 6, TRUE);
    xkb->names->groups[0] = MakeAtom("English (US)", 12, TRUE);

    /* the user's own layout, over the character keys only */
    {
        const struct amberwin_keymap *km = amberwin_get_keymap();
        if (km && km->nkeys > 0) {
            apply_layout(xkb, km);
            xkb->names->symbols = MakeAtom("amberx(windows)", 15, TRUE);
        }
        else {
            LogMessage(X_INFO, "AmberX: no Windows layout available, using the built-in US map\n");
        }
    }

    /* derive the per-key actions from the interpretations and the modmap */
    memset(&changes, 0, sizeof changes);
    XkbUpdateDescActions(xkb, xkb->min_key_code, XkbNumKeys(xkb), &changes);
    return xkb;

 fail:
    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
    return NULL;
}

/* ---- the .xkm path ---------------------------------------------------------- */
static XkbDescPtr
load_xkm(const char *path, unsigned need, unsigned *provided)
{
    FILE *f;
    XkbDescPtr xkb = NULL;
    *provided = 0;
    f = fopen(path, "rb");
    if (!f) {
        LogMessage(X_ERROR, "AmberX: keymap file could not be opened\n");
        return NULL;
    }
    *provided = XkmReadFile(f, need, XkmAllIndicesMask, &xkb);
    fclose(f);
    if ((*provided & need) != need) {
        if (xkb)
            XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
        LogMessage(X_ERROR, "AmberX: keymap file lacks required components\n");
        return NULL;
    }
    return xkb;
}

/* ---- what ddxLoad.c exported ------------------------------------------------ */
#define BUILTIN_PROVIDES (XkmSymbolsMask | XkmCompatMapMask | XkmTypesMask | \
                          XkmKeyNamesMask | XkmVirtualModsMask | XkmIndicatorsMask)

unsigned
XkbDDXLoadKeymapByNames(DeviceIntPtr keybd, XkbComponentNamesPtr names,
                        unsigned want, unsigned need, XkbDescPtr *xkbRtrn,
                        char *nameRtrn, int nameRtrnLen)
{
    const amberwin_config *cfg = amberwin_get_config();
    unsigned provided = 0;
    (void) keybd; (void) names; (void) want;

    *xkbRtrn = NULL;
    if (nameRtrn && nameRtrnLen > 0)
        nameRtrn[0] = '\0';

    if (cfg->keymap_path) {
        *xkbRtrn = load_xkm(cfg->keymap_path, need, &provided);
        if (*xkbRtrn) {
            if (nameRtrn)
                strncpy(nameRtrn, "amberx-file", nameRtrnLen);
            return provided;
        }
        LogMessage(X_WARNING, "AmberX: falling back to the built-in US keymap\n");
    }
    *xkbRtrn = build_builtin();
    if (!*xkbRtrn)
        return 0;
    if (nameRtrn)
        strncpy(nameRtrn, "amberx-builtin", nameRtrnLen);
    return BUILTIN_PROVIDES;
}

Bool
XkbDDXNamesFromRules(DeviceIntPtr keybd, const char *rules,
                     XkbRF_VarDefsPtr defs, XkbComponentNamesPtr names)
{
    (void) keybd; (void) rules; (void) defs;
    /* there is no rules file to evaluate; the names are informational */
    names->keycodes = Xstrdup("amberx");
    names->types = Xstrdup("amberx");
    names->compat = Xstrdup("amberx");
    names->symbols = Xstrdup("amberx(us)");
    names->geometry = NULL;
    return TRUE;
}

XkbDescPtr
XkbCompileKeymap(DeviceIntPtr dev, XkbRMLVOSet *rmlvo)
{
    XkbDescPtr xkb = NULL;
    XkbComponentNamesRec kccgst = { 0 };
    unsigned need = XkmSymbolsMask | XkmCompatMapMask | XkmTypesMask |
                    XkmKeyNamesMask | XkmVirtualModsMask;
    unsigned provided;
    char name[64];
    (void) rmlvo;

    if (!dev) {
        LogMessage(X_ERROR, "XKB: No device specified\n");
        return NULL;
    }
    provided = XkbDDXLoadKeymapByNames(dev, &kccgst, XkmAllIndicesMask, need,
                                       &xkb, name, sizeof name);
    if ((need & provided) != need) {
        if (xkb)
            XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
        LogMessage(X_ERROR, "XKB: keymap delivery failed\n");
        return NULL;
    }
    return xkb;
}

XkbDescPtr
XkbCompileKeymapFromString(DeviceIntPtr dev, const char *keymap, int keymap_length)
{
    (void) dev; (void) keymap; (void) keymap_length;
    LogMessage(X_ERROR, "XKB: AmberX does not compile keymap text (no xkbcomp)\n");
    return NULL;
}
