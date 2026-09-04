/* ddx_frames.c — the rootless frame procs. Original AmberSSH file.
 *
 * miext/rootless (X.Org, MIT) keeps every top-level window's pixels in its
 * own buffer and tells an implementation when frames appear, move, resize,
 * restack, and change. This is that implementation: each call becomes one
 * amberwin_frame_* call to the Windows side, which owns the native window
 * and the buffer. Nothing here knows what a window looks like on screen;
 * nothing on the other side knows what an X window is. */
#include <dix-config.h>
#include <string.h>
#include <X11/X.h>
#include "misc.h"
#include "os.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "pixmapstr.h"
#include "regionstr.h"
#include "rootless.h"
#include "amberos.h"
#include "amberwin.h"

#define FRAME(wid) ((struct amberwin_frame *) (wid))

struct amberwin_frame *
amber_frame_of(WindowPtr pWin)
{
    RootlessFrameID wid = RootlessFrameForWindow(pWin, FALSE);
    return FRAME(wid);
}

static Bool
amberCreateFrame(RootlessWindowPtr pFrame, ScreenPtr pScreen, int newX, int newY,
                 RegionPtr pShape)
{
    struct amberwin_frame *f;
    (void) pScreen;
    f = amberwin_frame_create(pFrame->win->drawable.id, newX, newY,
                              (int) pFrame->width, (int) pFrame->height,
                              pFrame->win->overrideRedirect ? 1 : 0);
    if (!f)
        return FALSE;
    pFrame->wid = (RootlessFrameID) f;
    if (pShape) {
        int n = RegionNumRects(pShape), i;
        BoxPtr b = RegionRects(pShape);
        int16_t *q = malloc(sizeof(int16_t) * 4 * (n ? n : 1));
        if (q) {
            for (i = 0; i < n; i++) {
                q[i * 4 + 0] = b[i].x1; q[i * 4 + 1] = b[i].y1;
                q[i * 4 + 2] = b[i].x2; q[i * 4 + 3] = b[i].y2;
            }
            amberwin_frame_set_shape(f, n, q);
            free(q);
        }
    }
    amber_wm_frame_created(pFrame->win, f);
    return TRUE;
}

static void
amberDestroyFrame(RootlessFrameID wid)
{
    amber_wm_frame_destroyed(NULL, FRAME(wid));
    amberwin_frame_destroy(FRAME(wid));
}

static void
amberMoveFrame(RootlessFrameID wid, ScreenPtr pScreen, int newX, int newY)
{
    (void) pScreen;
    amberwin_frame_move(FRAME(wid), newX, newY);
}

static void
amberResizeFrame(RootlessFrameID wid, ScreenPtr pScreen, int newX, int newY,
                 unsigned int newW, unsigned int newH, unsigned int gravity)
{
    (void) pScreen; (void) gravity;
    amberwin_frame_resize(FRAME(wid), newX, newY, (int) newW, (int) newH);
}

static void
amberRestackFrame(RootlessFrameID wid, RootlessFrameID nextWid)
{
    amberwin_frame_restack(FRAME(wid), FRAME(nextWid));
}

static void
amberReshapeFrame(RootlessFrameID wid, RegionPtr pNewShape)
{
    if (!pNewShape) {
        amberwin_frame_set_shape(FRAME(wid), 0, NULL);
        return;
    }
    {
        int n = RegionNumRects(pNewShape), i;
        BoxPtr b = RegionRects(pNewShape);
        int16_t *q = malloc(sizeof(int16_t) * 4 * (n ? n : 1));
        if (!q)
            return;
        for (i = 0; i < n; i++) {
            q[i * 4 + 0] = b[i].x1; q[i * 4 + 1] = b[i].y1;
            q[i * 4 + 2] = b[i].x2; q[i * 4 + 3] = b[i].y2;
        }
        amberwin_frame_set_shape(FRAME(wid), n, q);
        free(q);
    }
}

static void
amberUnmapFrame(RootlessFrameID wid)
{
    amberwin_frame_unmap(FRAME(wid));
}

static void
amberStartDrawing(RootlessFrameID wid, char **pixelData, int *bytesPerRow)
{
    *pixelData = amberwin_frame_bits(FRAME(wid), bytesPerRow);
}

static void
amberStopDrawing(RootlessFrameID wid, Bool flush)
{
    if (flush)
        amberwin_frame_present(FRAME(wid), 0, 0, -1, -1);
}

static void
amberUpdateRegion(RootlessFrameID wid, RegionPtr pDamage)
{
    if (!pDamage) {
        amberwin_frame_present(FRAME(wid), 0, 0, -1, -1);
        return;
    }
    {
        int n = RegionNumRects(pDamage), i;
        BoxPtr b = RegionRects(pDamage);
        for (i = 0; i < n; i++)
            amberwin_frame_present(FRAME(wid), b[i].x1, b[i].y1,
                                   b[i].x2 - b[i].x1, b[i].y2 - b[i].y1);
    }
}

static void
amberDamageRects(RootlessFrameID wid, int nrects, const BoxRec *rects,
                 int shift_x, int shift_y)
{
    int i;
    for (i = 0; i < nrects; i++)
        amberwin_frame_present(FRAME(wid), rects[i].x1 + shift_x, rects[i].y1 + shift_y,
                               rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
}

static void
amberSwitchWindow(RootlessWindowPtr pFrame, WindowPtr oldWin)
{
    amberwin_frame_set_xid(FRAME(pFrame->wid), pFrame->win->drawable.id);
    amber_wm_frame_switched(pFrame->win, oldWin, FRAME(pFrame->wid));
}

static void
amberHideWindow(RootlessFrameID wid)
{
    amberwin_frame_unmap(FRAME(wid));
}

static RootlessFrameProcsRec amberFrameProcs = {
    amberCreateFrame,
    amberDestroyFrame,
    amberMoveFrame,
    amberResizeFrame,
    amberRestackFrame,
    amberReshapeFrame,
    amberUnmapFrame,
    amberStartDrawing,
    amberStopDrawing,
    amberUpdateRegion,
    amberDamageRects,
    amberSwitchWindow,      /* SwitchWindow */
    NULL,                   /* DoReorderWindow: no ordering animation */
    amberHideWindow,        /* HideWindow */
    NULL,                   /* UpdateColormap: TrueColor only */
    NULL,                   /* CopyBytes: memmove is fine */
    NULL,                   /* CopyWindow */
};

struct _RootlessFrameProcs *
amber_frame_procs(void)
{
    return &amberFrameProcs;
}
