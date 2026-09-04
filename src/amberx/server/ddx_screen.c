/* ddx_screen.c — AmberWinDDX: the screen. Original AmberSSH file.
 *
 * Two modes, chosen by the host:
 *
 *   rootless (default, Phase 3): the X screen is the Windows virtual
 *   desktop and has no framebuffer of its own. miext/rootless keeps each
 *   top-level window's pixels in a per-frame buffer that the Windows side
 *   owns (ddx_frames.c), and every top-level window is a native window.
 *   The root is never drawn; the native desktop is the root.
 *
 *   rootful (--rootful, Phase 2): one native window shows one system-memory
 *   framebuffer (32 bpp BGRX); fb renders into it, a DAMAGE record on the
 *   screen pixmap accumulates what changed, and the block handler presents
 *   the dirty extents. Kept for the gate tests and for debugging. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "misc.h"
#include "os.h"
#include "scrnintstr.h"
#include "screenint.h"
#include "servermd.h"
#include "pixmapstr.h"
#include "windowstr.h"
#include "colormapst.h"
#include "cursorstr.h"
#include "fb.h"
#include "mi.h"
#include "micmap.h"
#include "mipointer.h"
#include "damage.h"
#include "rootless.h"
#include "amberos.h"
#include "amberwin.h"
#include "amberlimits.h"

typedef struct {
    void *bits;
    int stride;                 /* bytes per row */
    int width, height;
    DamagePtr damage;
    CreateScreenResourcesProcPtr CreateScreenResources;
    CloseScreenProcPtr CloseScreen;
} AmberScreenRec, *AmberScreenPtr;

/* A size-0 key holding a pointer, never a sized screen private: a sized key
 * registered once the screen exists makes dix reallocate and repack every
 * screen private fb and mi already own, and the visual masks and damage
 * record came back as garbage when that happened here. */
static DevPrivateKeyRec amberScreenKeyRec;
#define amberScreenKey (&amberScreenKeyRec)
#define AMBER_SCREEN(s) ((AmberScreenPtr) dixLookupPrivate(&(s)->devPrivates, amberScreenKey))

/* ---- the pointer's view of the screen ----------------------------------- */
static Bool
amberCursorOffScreen(ScreenPtr *ppScreen, int *x, int *y)
{
    (void) ppScreen; (void) x; (void) y;
    return FALSE;   /* one screen: the cursor is never off it */
}

static void
amberCrossScreen(ScreenPtr pScreen, Bool entering)
{
    (void) pScreen; (void) entering;
}

static void
amberWarpCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    miPointerWarpCursor(pDev, pScreen, x, y);
}

static miPointerScreenFuncRec amberPointerScreenFuncs = {
    amberCursorOffScreen,
    amberCrossScreen,
    amberWarpCursor,
};

/* Rootless: the native cursor is the cursor. mi still tracks the sprite
 * position; it just never draws one. X cursor images are Phase 6. */
static Bool
amberRealizeCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor)
{
    (void) pDev; (void) pScreen; (void) pCursor;
    return TRUE;
}

static Bool
amberUnrealizeCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor)
{
    (void) pDev; (void) pScreen; (void) pCursor;
    return TRUE;
}

static void
amberSetCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor, int x, int y)
{
    (void) pDev; (void) pScreen; (void) pCursor; (void) x; (void) y;
}

static void
amberMoveCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    (void) pDev; (void) pScreen; (void) x; (void) y;
}

static Bool
amberDeviceCursorInitialize(DeviceIntPtr pDev, ScreenPtr pScreen)
{
    (void) pDev; (void) pScreen;
    return TRUE;
}

static void
amberDeviceCursorCleanup(DeviceIntPtr pDev, ScreenPtr pScreen)
{
    (void) pDev; (void) pScreen;
}

static miPointerSpriteFuncRec amberNativeSpriteFuncs = {
    amberRealizeCursor,
    amberUnrealizeCursor,
    amberSetCursor,
    amberMoveCursor,
    amberDeviceCursorInitialize,
    amberDeviceCursorCleanup,
};

/* ---- rootful presenting --------------------------------------------------- */
static void
amberBlockHandler(void *blockData, void *timeout)
{
    ScreenPtr pScreen = blockData;
    AmberScreenPtr as = AMBER_SCREEN(pScreen);
    RegionPtr region;
    BoxPtr box;
    (void) timeout;
    if (!as || !as->damage)
        return;
    region = DamageRegion(as->damage);
    if (!region || !RegionNotEmpty(region))
        return;
    box = RegionExtents(region);
    amberwin_screen_present(box->x1, box->y1, box->x2 - box->x1, box->y2 - box->y1);
    DamageEmpty(as->damage);
}

static void
amberWakeupHandler(void *blockData, int result)
{
    (void) blockData; (void) result;
}

static Bool
amberCreateScreenResources(ScreenPtr pScreen)
{
    AmberScreenPtr as = AMBER_SCREEN(pScreen);
    Bool ok;

    pScreen->CreateScreenResources = as->CreateScreenResources;
    ok = pScreen->CreateScreenResources ? (*pScreen->CreateScreenResources) (pScreen) : TRUE;
    pScreen->CreateScreenResources = amberCreateScreenResources;
    if (!ok)
        return FALSE;

    /* ReportNone: nobody is told, the region just accumulates for the
     * block handler. Registered on the screen pixmap so every drawable
     * backed by the framebuffer — root, children, the cursor — counts. */
    as->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!as->damage)
        return FALSE;
    DamageRegister(&(*pScreen->GetScreenPixmap) (pScreen)->drawable, as->damage);
    amberwin_screen_present(0, 0, as->width, as->height);
    return TRUE;
}

static Bool
amberSaveScreen(ScreenPtr pScreen, int on)
{
    (void) pScreen; (void) on;
    return TRUE;    /* the desktop's own power management owns blanking */
}

static Bool
amberCloseScreen(ScreenPtr pScreen)
{
    AmberScreenPtr as = AMBER_SCREEN(pScreen);
    Bool ret = TRUE;
    if (as) {
        if (as->damage) {
            DamageUnregister(as->damage);
            DamageDestroy(as->damage);
            as->damage = NULL;
        }
        pScreen->CloseScreen = as->CloseScreen;
        if (pScreen->CloseScreen)
            ret = (*pScreen->CloseScreen) (pScreen);
        dixSetPrivate(&pScreen->devPrivates, amberScreenKey, NULL);
        free(as);
    }
    return ret;
}

/* ---- ScreenInit ----------------------------------------------------------- */
static Bool
amberScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    const amberwin_config *cfg = amberwin_get_config();
    AmberScreenPtr as;
    void *bits = NULL;
    int stride = 0;
    int dpi = 96;
    (void) argc; (void) argv;

    if (!dixRegisterPrivateKey(&amberScreenKeyRec, PRIVATE_SCREEN, 0))
        return FALSE;
    as = calloc(1, sizeof(AmberScreenRec));
    if (!as)
        return FALSE;
    dixSetPrivate(&pScreen->devPrivates, amberScreenKey, as);
    as->width = cfg->width;
    as->height = cfg->height;

    if (!cfg->rootless) {
        if (!amberwin_screen_create(as->width, as->height, &bits, &stride))
            return FALSE;
        as->bits = bits;
        as->stride = stride;
    }

    /* One visual: 24-bit TrueColor over 32 bpp BGRX buffers. The masks say
     * where each channel sits; with pixman's x8r8g8b8 that is red in bits
     * 16-23, which is what a Windows DIB expects, so buffers are presented
     * without a swizzle. */
    miClearVisualTypes();
    if (!miSetVisualTypesAndMasks(24, TrueColorMask, 8, TrueColor,
                                  0x00ff0000, 0x0000ff00, 0x000000ff))
        return FALSE;
    if (!miSetPixmapDepths())
        return FALSE;

    /* Rootless: no framebuffer and a zero pitch, which makes mi give the
     * screen pixmap a NULL base that RootlessUpdateScreenPixmap then
     * replaces with a one-row dummy. Nothing ever draws to the root. */
    if (!fbScreenInit(pScreen, bits, as->width, as->height, dpi, dpi,
                      cfg->rootless ? 0 : stride / 4, 32))
        return FALSE;
    if (!fbPictureInit(pScreen, 0, 0))
        return FALSE;

    pScreen->SaveScreen = amberSaveScreen;
    as->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = amberCloseScreen;

    if (cfg->rootless) {
        /* DAMAGE must wrap beneath the rootless layer */
        if (!DamageSetup(pScreen))
            return FALSE;
        if (!RootlessInit(pScreen, amber_frame_procs()))
            return FALSE;
        if (!miPointerInitialize(pScreen, &amberNativeSpriteFuncs, &amberPointerScreenFuncs, FALSE))
            return FALSE;
        if (!amber_wm_screen_init(pScreen))
            return FALSE;
        /* RANDR describes the desktop the screen actually is: one output
         * per Windows monitor. Rootful mode has no such correspondence —
         * its screen is one window, not the desktop — so it has no RANDR
         * rather than a wrong one. */
        if (!amber_randr_screen_init(pScreen))
            return FALSE;
        /* the clipboard bridge needs a root to hang its transfer window
         * from, which rootless has and rootful does not use the same way */
        if (!amber_clipboard_screen_init(pScreen))
            return FALSE;
    }
    else {
        as->CreateScreenResources = pScreen->CreateScreenResources;
        pScreen->CreateScreenResources = amberCreateScreenResources;
        /* software cursor drawn into the framebuffer by mi */
        if (!miDCInitialize(pScreen, &amberPointerScreenFuncs))
            return FALSE;
        if (!RegisterBlockAndWakeupHandlers(amberBlockHandler, amberWakeupHandler, pScreen))
            return FALSE;
    }
    if (!miCreateDefColormap(pScreen))
        return FALSE;
    /* limits wrap last, so they are outermost on every path */
    if (!amber_limits_screen_init(pScreen))
        return FALSE;
    /* The extension allowlist for restricted clients. Registered here,
     * during InitOutput, so that it runs *after* the SECURITY extension's
     * own hook, which is registered later in InitExtensions: dix prepends
     * callbacks and calls them from the head, so the earlier registration
     * is the later call, and only the later call can widen what the
     * earlier one denied. */
    if (!amber_policy_init())
        return FALSE;
    /* Diagnostics last: the PutImage probe sees the request after every
     * limit and policy wrapper has already had its say. */
    if (!amber_probe_init())
        return FALSE;
    return TRUE;
}

/* ---- InitOutput ----------------------------------------------------------- */
static const PixmapFormatRec amberFormats[] = {
    { 1,  1,  BITMAP_SCANLINE_PAD },
    { 8,  8,  BITMAP_SCANLINE_PAD },
    { 15, 16, BITMAP_SCANLINE_PAD },
    { 16, 16, BITMAP_SCANLINE_PAD },
    { 24, 32, BITMAP_SCANLINE_PAD },
    { 32, 32, BITMAP_SCANLINE_PAD },
};

void
InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv)
{
    int i;
    pScreenInfo->imageByteOrder = IMAGE_BYTE_ORDER;
    pScreenInfo->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    pScreenInfo->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    pScreenInfo->bitmapBitOrder = BITMAP_BIT_ORDER;
    pScreenInfo->numPixmapFormats = sizeof amberFormats / sizeof amberFormats[0];
    for (i = 0; i < pScreenInfo->numPixmapFormats; i++)
        pScreenInfo->formats[i] = amberFormats[i];
    if (AddScreen(amberScreenInit, argc, argv) < 0)
        FatalError("AmberX: could not create the screen");
}
