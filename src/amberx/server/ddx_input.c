/* ddx_input.c — AmberWinDDX: one keyboard, one pointer. Original AmberSSH
 * file.
 *
 * Input never comes from Win32 directly into the X core. The Windows side
 * turns messages into amberwin_events (already in evdev keycodes and screen
 * pixels); WaitForSomething drains them on the server thread and hands
 * them here, where they become X events through the ordinary
 * QueuePointerEvents/QueueKeyboardEvents path. No input thread. */
#include <dix-config.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include "misc.h"
#include "os.h"
#include "input.h"
#include "inputstr.h"
#include "exevents.h"
#include "xserver-properties.h"
#include "xkbsrv.h"
#include "mi.h"
#include "mipointer.h"
#include "scrnintstr.h"
#include "amberos.h"
#include "amberwin.h"

static DeviceIntPtr amberPointer;
static DeviceIntPtr amberKeyboard;
static ValuatorMask *amberMask;

#define NUM_BUTTONS 7   /* 1-3, wheel 4/5, tilt 6/7 */

/* ---- pointer -------------------------------------------------------------- */
static void
amberPtrCtrl(DeviceIntPtr dev, PtrCtrl *ctrl)
{
    (void) dev; (void) ctrl;
}

static int
amberPointerProc(DeviceIntPtr dev, int what)
{
    const amberwin_config *cfg = amberwin_get_config();
    CARD8 map[NUM_BUTTONS + 1];
    Atom btn_labels[NUM_BUTTONS] = { 0 };
    Atom axes_labels[2] = { 0 };
    int i;

    switch (what) {
    case DEVICE_INIT:
        for (i = 0; i <= NUM_BUTTONS; i++)
            map[i] = (CARD8) i;
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
        btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
        btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
        btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y);
        if (!InitPointerDeviceStruct((DevicePtr) dev, map, NUM_BUTTONS, btn_labels,
                                     amberPtrCtrl, GetMotionHistorySize(), 2,
                                     axes_labels))
            return BadValue;
        /* absolute axes in screen pixels: the Windows side reports where
         * the cursor is, it does not report deltas */
        InitValuatorAxisStruct(dev, 0, axes_labels[0], 0, cfg->width - 1,
                               10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(dev, 1, axes_labels[1], 0, cfg->height - 1,
                               10000, 0, 10000, Absolute);
        return Success;
    case DEVICE_ON:
        dev->public.on = TRUE;
        return Success;
    case DEVICE_OFF:
        dev->public.on = FALSE;
        return Success;
    case DEVICE_CLOSE:
        return Success;
    }
    return BadMatch;
}

/* ---- keyboard ------------------------------------------------------------- */
static void
amberBell(int percent, DeviceIntPtr dev, void *ctrl, int class)
{
    (void) dev; (void) ctrl; (void) class;
    amberwin_bell(percent);
}

static void
amberKbdCtrl(DeviceIntPtr dev, KeybdCtrl *ctrl)
{
    (void) dev; (void) ctrl;    /* LEDs and autorepeat rates: nothing to drive */
}

static int
amberKeyboardProc(DeviceIntPtr dev, int what)
{
    switch (what) {
    case DEVICE_INIT:
        /* NULL rmlvo: XkbInitKeyboardDeviceStruct asks XkbCompileKeymap for
         * the defaults, and AmberX's XkbCompileKeymap (ddx_keymap.c) answers
         * with the configured .xkm or the built-in map. Nothing is spawned. */
        if (!InitKeyboardDeviceStruct(dev, NULL, amberBell, amberKbdCtrl))
            return BadValue;
        return Success;
    case DEVICE_ON:
        dev->public.on = TRUE;
        return Success;
    case DEVICE_OFF:
        dev->public.on = FALSE;
        return Success;
    case DEVICE_CLOSE:
        return Success;
    }
    return BadMatch;
}

/* ---- DDX entry points ----------------------------------------------------- */
void
InitInput(int argc, char **argv)
{
    (void) argc; (void) argv;
    amberPointer = AddInputDevice(serverClient, amberPointerProc, TRUE);
    amberKeyboard = AddInputDevice(serverClient, amberKeyboardProc, TRUE);
    if (!amberPointer || !amberKeyboard)
        FatalError("AmberX: could not create the core input devices");
    amberPointer->name = strdup("AmberX pointer");
    amberKeyboard->name = strdup("AmberX keyboard");
    amberMask = valuator_mask_new(2);
    if (!amberMask)
        FatalError("AmberX: out of memory creating the valuator mask");
    mieqInit();
}

void
CloseInput(void)
{
    mieqFini();
    valuator_mask_free(&amberMask);
    amberPointer = NULL;
    amberKeyboard = NULL;
}

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

/* ---- lock keys ------------------------------------------------------------
 *
 * Caps Lock and Num Lock are latched state, and the user can change them in
 * another window while a forwarded application has no idea. Windows reports
 * the truth when a frame is activated; the X side reconciles by pressing
 * and releasing the lock key itself, which goes through XKB's own path, so
 * indicators, the modifier mask and every client's idea of the state all
 * move together. Setting the mask behind XKB's back does none of that.
 *
 * Scroll Lock has no modifier in AmberX's map, so it is accepted and
 * ignored rather than bound to something arbitrary. */
#define KEYCODE_CAPS (58 + 8)
#define KEYCODE_NUM  (69 + 8)

static void
tap_key(int keycode)
{
    QueueKeyboardEvents(amberKeyboard, KeyPress, keycode);
    QueueKeyboardEvents(amberKeyboard, KeyRelease, keycode);
}

static void
sync_locks(int caps, int num)
{
    XkbSrvInfoPtr xkbi;
    unsigned locked;

    if (!amberKeyboard || !amberKeyboard->key || !amberKeyboard->key->xkbInfo)
        return;
    xkbi = amberKeyboard->key->xkbInfo;
    locked = xkbi->state.locked_mods;
    if (!!(locked & LockMask) != !!caps)
        tap_key(KEYCODE_CAPS);
    if (!!(locked & Mod2Mask) != !!num)
        tap_key(KEYCODE_NUM);
}

/* The user switched Windows keyboard layout. Rebuilding the map through
 * XkbCompileKeymap picks up the new one (ddx_keymap.c reads it from the
 * Windows side), and XkbDeviceApplyKeymap installs it and tells clients,
 * which is how a running application follows the switch without
 * reconnecting. */
static void
reload_keymap(void)
{
    XkbDescPtr xkb;

    if (!amberKeyboard)
        return;
    xkb = XkbCompileKeymap(amberKeyboard, NULL);
    if (!xkb) {
        LogMessage(X_WARNING, "AmberX: the new keyboard layout could not be built; keeping the old one\n");
        return;
    }
    if (!XkbDeviceApplyKeymap(amberKeyboard, xkb))
        LogMessage(X_WARNING, "AmberX: the new keyboard layout was rejected; keeping the old one\n");
    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
}

/* One event from the Windows side. Coordinates are already screen pixels
 * and keycodes are already evdev codes; the +8 is the X keycode offset. */
void
amber_ddx_input_event(const struct amberwin_event *ev)
{
    if (!amberPointer || !amberKeyboard)
        return;
    switch (ev->type) {
    case AMBERWIN_EV_POINTER_MOVE:
        valuator_mask_zero(amberMask);
        valuator_mask_set_double(amberMask, 0, ev->x);
        valuator_mask_set_double(amberMask, 1, ev->y);
        QueuePointerEvents(amberPointer, MotionNotify, 0,
                           POINTER_ABSOLUTE | POINTER_SCREEN, amberMask);
        break;
    case AMBERWIN_EV_BUTTON:
        if (ev->button < 1 || ev->button > NUM_BUTTONS)
            break;
        valuator_mask_zero(amberMask);
        QueuePointerEvents(amberPointer, ev->pressed ? ButtonPress : ButtonRelease,
                           ev->button, 0, amberMask);
        break;
    case AMBERWIN_EV_KEY:
        if (ev->keycode + 8 > 255)
            break;
        QueueKeyboardEvents(amberKeyboard, ev->pressed ? KeyPress : KeyRelease,
                            (int) ev->keycode + 8);
        break;
    case AMBERWIN_EV_LOCKS:
        sync_locks(ev->x, ev->y);
        break;
    case AMBERWIN_EV_KEYMAP:
        reload_keymap();
        break;
    default:
        break;
    }
}
