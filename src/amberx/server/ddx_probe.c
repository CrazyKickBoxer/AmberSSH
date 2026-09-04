/* ddx_probe.c — what the server believes it is drawing into. Original
 * AmberSSH file.
 *
 * A fault inside the blitter names the function that died and the address
 * it touched, and nothing about which drawable the request named or what
 * the server thought that drawable's geometry was. This wraps PutImage —
 * the request that hands over a rendered frame, and the one GTK 4 killed
 * the host with — and logs, for the first few dozen of a session, the X
 * side's own view of the target: the window, the clip its writes will be
 * bounded by, its top-level parent, and that parent's frame and buffer as
 * the Windows side knows them. Whatever the blitter then does, the last of
 * these lines says what it was asked to do it to.
 *
 * Geometry only: no pixels, no titles, no request bytes. Bounded, so it
 * costs nothing once a session is under way — startup is where the
 * failures it exists for have happened. A byte-swapped client would go
 * unprobed; none has ever connected, and the enforcement in ddx_limits.c
 * still covers it. */
#include <dix-config.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "pixmapstr.h"
#include "regionstr.h"
#include "amberos.h"
#include "amberwin.h"

#define PROBE_LINES 48

static int (*origPutImage) (ClientPtr);
static int lines;

static int
probe_ProcPutImage(ClientPtr client)
{
    REQUEST(xPutImageReq);

    if (lines < PROBE_LINES &&
        client->req_len >= bytes_to_int32(sizeof(xPutImageReq))) {
        DrawablePtr pDraw = NULL;
        int rc = dixLookupDrawable(&pDraw, stuff->drawable, client, M_ANY,
                                   DixReadAccess);
        lines++;
        if (rc != Success || !pDraw) {
            LogMessage(X_INFO, "AmberX: PutImage #%d: drawable 0x%lx not found (%d)\n",
                       lines, (unsigned long) stuff->drawable, rc);
        }
        else if (pDraw->type != DRAWABLE_WINDOW) {
            LogMessage(X_INFO,
                       "AmberX: PutImage #%d: %ux%u at (%d,%d) -> pixmap 0x%lx %dx%d\n",
                       lines, stuff->width, stuff->height, stuff->dstX, stuff->dstY,
                       (unsigned long) stuff->drawable, pDraw->width, pDraw->height);
        }
        else {
            WindowPtr w = (WindowPtr) pDraw;
            WindowPtr top = w;
            BoxPtr ext = RegionExtents(&w->clipList);
            struct amberwin_frame *f;
            char frame[96];

            while (top->parent && top->parent->parent)
                top = top->parent;
            f = amber_frame_of(w);
            if (f) {
                int fx, fy, fw, fh, bw, bh;
                amberwin_frame_geometry(f, &fx, &fy, &fw, &fh, &bw, &bh);
                snprintf(frame, sizeof frame, "frame %dx%d at (%d,%d), buffer %dx%d",
                         fw, fh, fx, fy, bw, bh);
            }
            else
                snprintf(frame, sizeof frame, "NO FRAME");

            LogMessage(X_INFO,
                       "AmberX: PutImage #%d: %ux%u at (%d,%d) depth %u -> "
                       "win 0x%lx %dx%d at (%d,%d) bw %d %s%s; "
                       "clip %ld boxes [%d,%d..%d,%d]; "
                       "top 0x%lx %dx%d at (%d,%d); %s\n",
                       lines, stuff->width, stuff->height, stuff->dstX, stuff->dstY,
                       stuff->depth,
                       (unsigned long) w->drawable.id, w->drawable.width, w->drawable.height,
                       w->drawable.x, w->drawable.y, wBorderWidth(w),
                       w->realized ? "realized" : "unrealized",
                       w->viewable ? " viewable" : "",
                       RegionNumRects(&w->clipList), ext->x1, ext->y1, ext->x2, ext->y2,
                       (unsigned long) top->drawable.id, top->drawable.width,
                       top->drawable.height, top->drawable.x, top->drawable.y, frame);
        }
    }
    return origPutImage(client);
}

Bool
amber_probe_init(void)
{
    if (!origPutImage) {
        origPutImage = ProcVector[X_PutImage];
        ProcVector[X_PutImage] = probe_ProcPutImage;
    }
    return TRUE;
}
