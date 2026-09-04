/* ddx_screen.c — AmberWinDDX: the one screen. Original AmberSSH file.
 *
 * The screen is a system-memory framebuffer the Windows side allocates
 * (32 bpp BGRX) and shows in a window. fb renders into it; a DAMAGE record
 * on the screen pixmap accumulates what changed; the block handler — which
 * runs once per dispatch round, right before the server waits — hands the
 * dirty extents to the Windows side to present. That is the whole output
 * path: no GPU, no compositor, nothing copied twice.
 *
 * Rootful for now: one window is the root window. Rootless (per-toplevel
 * native windows) is Phase 3 and layers on top of this without replacing it. */
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
#include "fb.h"
#include "mi.h"
#include "micmap.h"
#include "mipointer.h"
#include "damage.h"
#include "amberwin.h"

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

/* ---- presenting ---------------------------------------------------------- */
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
    /* first frame: everything */
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
        pScreen->CloseScreen = as->CloseScreen;   /* fb's, installed by fbScreenInit */
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

    if (!amberwin_screen_create(as->width, as->height, &bits, &stride))
        return FALSE;
    as->bits = bits;
    as->stride = stride;

    /* One visual: 24-bit TrueColor over a 32 bpp BGRX framebuffer. The
     * masks say where each channel sits in the 32-bit pixel; with pixman's
     * x8r8g8b8 that is red in bits 16-23, which is what a Windows DIB
     * expects, so the framebuffer is presented without a swizzle. */
    miClearVisualTypes();
    if (!miSetVisualTypesAndMasks(24, TrueColorMask, 8, TrueColor,
                                  0x00ff0000, 0x0000ff00, 0x000000ff))
        return FALSE;
    if (!miSetPixmapDepths())
        return FALSE;

    if (!fbScreenInit(pScreen, bits, as->width, as->height, dpi, dpi,
                      stride / 4, 32))
        return FALSE;
    if (!fbPictureInit(pScreen, 0, 0))
        return FALSE;

    pScreen->SaveScreen = amberSaveScreen;
    as->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = amberCreateScreenResources;
    as->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = amberCloseScreen;

    /* software cursor drawn into the framebuffer by mi */
    if (!miDCInitialize(pScreen, &amberPointerScreenFuncs))
        return FALSE;
    if (!miCreateDefColormap(pScreen))
        return FALSE;

    if (!RegisterBlockAndWakeupHandlers(amberBlockHandler, amberWakeupHandler, pScreen))
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
