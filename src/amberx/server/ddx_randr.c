/* ddx_randr.c — the Windows monitor topology as RANDR outputs. Original
 * AmberSSH file.
 *
 * The X screen is the whole virtual desktop (ddx_screen.c). RANDR is how a
 * client finds out that the desktop is not one flat surface: one CRTC and
 * one output per Windows monitor, each at its own position and its own
 * physical size, so a toolkit that asks "which monitor is my window on and
 * what scale is it" gets a true answer rather than an average.
 *
 * Three things this deliberately does not do:
 *
 *   * no mode setting. rrCrtcSet refuses, because AmberX does not own the
 *     monitors — Windows does, and a forwarded application changing the
 *     user's screen resolution is not a feature. Clients see the current
 *     mode and nothing else to choose from.
 *   * no rotation or transforms, for the same reason.
 *   * no screen resize by a client. The screen changes size when Windows
 *     says so (a monitor was plugged in), never because a client asked.
 *
 * When the desktop does change, amber_randr_monitors_changed rebuilds the
 * outputs and RRTellChanged sends the notifications; a client that watches
 * for them moves its windows, and one that does not keeps working with the
 * geometry it had. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "misc.h"
#include "os.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "randrstr.h"
#include "amberos.h"
#include "amberwin.h"

#define MAX_MONITORS 16

typedef struct {
    RRCrtcPtr crtc;
    RROutputPtr output;
    RRModePtr mode;
} AmberOutputRec;

static AmberOutputRec outputs[MAX_MONITORS];
static int noutputs;
static ScreenPtr randrScreen;

/* A mode named for what it is: the monitor's current resolution. Refresh is
 * reported as 60 Hz because Windows' composited desktop does not hand out a
 * meaningful per-monitor scanout rate here, and a client that draws to the
 * number rather than to damage would be misled by anything more specific. */
static RRModePtr
mode_for(int w, int h)
{
    xRRModeInfo info;
    char name[32];
    int len;

    memset(&info, 0, sizeof info);
    len = snprintf(name, sizeof name, "%dx%d", w, h);
    info.width = (CARD16) w;
    info.height = (CARD16) h;
    info.hTotal = (CARD16) w;
    info.hSyncStart = (CARD16) w;
    info.hSyncEnd = (CARD16) w;
    info.vTotal = (CARD16) h;
    info.vSyncStart = (CARD16) h;
    info.vSyncEnd = (CARD16) h;
    info.dotClock = (CARD32) w * h * 60;
    info.nameLength = (CARD16) len;
    return RRModeGet(&info, name);
}

static void
free_outputs(void)
{
    int i;
    /* the primary is one of the outputs about to go: clear it first, or
     * the screen keeps a pointer to a destroyed output */
    if (randrScreen) {
        rrScrPrivPtr priv = rrGetScrPriv(randrScreen);
        if (priv)
            priv->primaryOutput = NULL;
    }
    for (i = 0; i < noutputs; i++) {
        if (outputs[i].output)
            RROutputDestroy(outputs[i].output);
        if (outputs[i].crtc)
            RRCrtcDestroy(outputs[i].crtc);
        if (outputs[i].mode)
            RRModeDestroy(outputs[i].mode);
        memset(&outputs[i], 0, sizeof outputs[i]);
    }
    noutputs = 0;
}

/* Builds one CRTC + output per monitor. Called at screen init and again
 * whenever the Windows topology changes. */
static Bool
build_outputs(ScreenPtr pScreen)
{
    amberwin_monitor mons[MAX_MONITORS];
    int n, i;

    n = amberwin_monitors(mons, MAX_MONITORS);
    if (n > MAX_MONITORS) {
        LogMessage(X_WARNING, "AmberX: %d monitors, reporting the first %d\n",
                   n, MAX_MONITORS);
        n = MAX_MONITORS;
    }
    if (n < 1)
        return FALSE;

    free_outputs();
    for (i = 0; i < n; i++) {
        AmberOutputRec *o = &outputs[i];
        const amberwin_monitor *m = &mons[i];
        char name[32];
        int len;

        /* the Windows device name is \\.\DISPLAYn; the tail is the name a
         * user recognises, and it is ours, not the remote's, so it is safe
         * to show */
        len = snprintf(name, sizeof name, "%s",
                       m->name[0] ? m->name : "AmberX");
        if (len > (int) sizeof name - 1)
            len = (int) sizeof name - 1;

        o->crtc = RRCrtcCreate(pScreen, NULL);
        o->output = RROutputCreate(pScreen, name, len, NULL);
        o->mode = mode_for(m->w, m->h);
        if (!o->crtc || !o->output || !o->mode) {
            free_outputs();
            return FALSE;
        }
        RRCrtcSetRotations(o->crtc, RR_Rotate_0);
        RRCrtcSetTransformSupport(o->crtc, FALSE);
        if (!RROutputSetCrtcs(o->output, &o->crtc, 1) ||
            !RROutputSetModes(o->output, &o->mode, 1, 1) ||
            !RROutputSetConnection(o->output, RR_Connected) ||
            !RROutputSetPhysicalSize(o->output, m->mm_w, m->mm_h) ||
            !RRCrtcNotify(o->crtc, o->mode, m->x, m->y, RR_Rotate_0, NULL, 1,
                          &o->output)) {
            free_outputs();
            return FALSE;
        }
        noutputs = i + 1;
        /* The user's primary monitor is the primary output, so a client
         * asking "where should a new window go" gets the answer Windows
         * would give. Upstream's RRSetPrimaryOutput is static to
         * rroutput.c and tells clients about the change; here the outputs
         * were only just created and the caller sends one notification for
         * the whole rebuild, so the field is set directly. */
        if (m->primary) {
            rrScrPrivPtr priv = rrGetScrPriv(pScreen);
            priv->primaryOutput = o->output;
            priv->layoutChanged = TRUE;
        }
    }
    return TRUE;
}

/* RANDR asks the driver to refresh its idea of the hardware. Everything is
 * already current here — the Windows side pushes changes — so this only
 * says what rotations exist. */
static Bool
amber_rr_get_info(ScreenPtr pScreen, Rotation *rotations)
{
    (void) pScreen;
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool
amber_rr_screen_set_size(ScreenPtr pScreen, CARD16 width, CARD16 height,
                         CARD32 mmWidth, CARD32 mmHeight)
{
    (void) pScreen; (void) width; (void) height; (void) mmWidth; (void) mmHeight;
    /* the desktop belongs to Windows; a client does not resize it */
    return FALSE;
}

static Bool
amber_rr_crtc_set(ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x,
                  int y, Rotation rotation, int numOutputs, RROutputPtr *out)
{
    (void) pScreen; (void) crtc; (void) mode; (void) x; (void) y;
    (void) rotation; (void) numOutputs; (void) out;
    /* likewise: no forwarded application changes the user's screen mode */
    return FALSE;
}

Bool
amber_randr_screen_init(ScreenPtr pScreen)
{
    rrScrPrivPtr priv;
    const amberwin_config *cfg = amberwin_get_config();

    if (!RRScreenInit(pScreen))
        return FALSE;
    priv = rrGetScrPriv(pScreen);
    priv->rrGetInfo = amber_rr_get_info;
    priv->rrScreenSetSize = amber_rr_screen_set_size;
    priv->rrCrtcSet = amber_rr_crtc_set;

    /* the screen is exactly the virtual desktop, and stays that size until
     * Windows says otherwise */
    RRScreenSetSizeRange(pScreen, cfg->width, cfg->height, cfg->width, cfg->height);
    randrScreen = pScreen;
    if (!build_outputs(pScreen))
        return FALSE;
    LogMessage(X_INFO, "AmberX: %d monitor%s reported through RANDR\n",
               noutputs, noutputs == 1 ? "" : "s");
    return TRUE;
}

void
amber_randr_monitors_changed(void)
{
    amberwin_monitor mons[MAX_MONITORS];
    int n, i;
    Bool same;

    if (!randrScreen)
        return;

    /* Nothing to do unless the topology actually differs: Windows sends
     * WM_DISPLAYCHANGE for changes that do not move a monitor, and a
     * spurious RANDR notification makes toolkits re-lay-out for nothing. */
    n = amberwin_monitors(mons, MAX_MONITORS);
    if (n > MAX_MONITORS)
        n = MAX_MONITORS;
    same = (n == noutputs);
    for (i = 0; same && i < n; i++) {
        const RRCrtcPtr c = outputs[i].crtc;
        same = c && c->x == mons[i].x && c->y == mons[i].y &&
               c->mode && c->mode->mode.width == mons[i].w &&
               c->mode->mode.height == mons[i].h;
    }
    if (same)
        return;

    if (!build_outputs(randrScreen)) {
        LogMessage(X_WARNING, "AmberX: monitor topology could not be rebuilt\n");
        return;
    }
    RRTellChanged(randrScreen);
    LogMessage(X_INFO, "AmberX: monitor topology changed, %d now reported\n",
               noutputs);
}
