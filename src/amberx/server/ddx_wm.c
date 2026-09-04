/* ddx_wm.c — AmberXWM: the window manager inside the server.
 * Original AmberSSH file.
 *
 * A window manager is ordinarily an X client. AmberX has no listener and
 * no client library, so its manager lives in the server and works from the
 * server's own structures: it reads the ICCCM/EWMH properties a frame
 * needs when the frame appears and whenever they change
 * (PropertyStateCallback), answers the client messages a WM is sent
 * (_NET_WM_STATE, _NET_ACTIVE_WINDOW, _NET_CLOSE_WINDOW) by observing them
 * through the XACE send hook, and turns what the user does to a native
 * window — close, move, resize, activate, minimise — into the X-side
 * actions a WM would take. It does not reparent: the native window IS the
 * frame, and native stacking is the stacking.
 *
 * What it handles: WM_PROTOCOLS, WM_DELETE_WINDOW, WM_TAKE_FOCUS,
 * WM_TRANSIENT_FOR, WM_NORMAL_HINTS, WM_HINTS (urgency), WM_NAME,
 * _NET_WM_NAME, _NET_WM_ICON, _NET_WM_STATE (fullscreen, maximized h/v,
 * hidden, demands_attention, modal), _NET_ACTIVE_WINDOW, _NET_CLIENT_LIST,
 * _NET_SUPPORTED, _NET_SUPPORTING_WM_CHECK, WM_STATE.
 *
 * Everything remote-supplied is bounded here before it crosses to the
 * Windows side: titles are sanitised and cut, icons are size-checked
 * against the property length before a byte is read. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
/* ICCCM values normally spelled by libX11's Xutil.h, which is not a proto header */
#define PMinSize (1L << 4)
#define PMaxSize (1L << 5)
#define NormalState 1
#define IconicState 3
#include "misc.h"
#include "os.h"
#include "dix.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "propertyst.h"
#include "property.h"
#include "input.h"
#include "inputstr.h"
#include "xace.h"
#include "xacestr.h"
#include "rootless.h"
#include "amberos.h"
#include "amberwin.h"

/* ---- atoms ------------------------------------------------------------- */
static Atom aWM_PROTOCOLS, aWM_DELETE_WINDOW, aWM_TAKE_FOCUS, aWM_TRANSIENT_FOR,
    aWM_NORMAL_HINTS, aWM_HINTS, aWM_NAME, aWM_STATE, aUTF8_STRING,
    aNET_WM_NAME, aNET_WM_ICON, aNET_WM_STATE, aNET_WM_STATE_FULLSCREEN,
    aNET_WM_STATE_MAXIMIZED_HORZ, aNET_WM_STATE_MAXIMIZED_VERT,
    aNET_WM_STATE_HIDDEN, aNET_WM_STATE_DEMANDS_ATTENTION, aNET_WM_STATE_MODAL,
    aNET_ACTIVE_WINDOW, aNET_CLIENT_LIST, aNET_SUPPORTED, aNET_SUPPORTING_WM_CHECK,
    aNET_CLOSE_WINDOW, aNET_WM_PID;

#define A(name) (a##name = MakeAtom(#name, (unsigned) strlen(#name), TRUE))

static void
make_atoms(void)
{
    A(WM_PROTOCOLS); A(WM_DELETE_WINDOW); A(WM_TAKE_FOCUS); A(WM_TRANSIENT_FOR);
    A(WM_NORMAL_HINTS); A(WM_HINTS); A(WM_NAME); A(WM_STATE); A(UTF8_STRING);
    aNET_WM_NAME = MakeAtom("_NET_WM_NAME", 12, TRUE);
    aNET_WM_ICON = MakeAtom("_NET_WM_ICON", 12, TRUE);
    aNET_WM_STATE = MakeAtom("_NET_WM_STATE", 13, TRUE);
    aNET_WM_STATE_FULLSCREEN = MakeAtom("_NET_WM_STATE_FULLSCREEN", 24, TRUE);
    aNET_WM_STATE_MAXIMIZED_HORZ = MakeAtom("_NET_WM_STATE_MAXIMIZED_HORZ", 28, TRUE);
    aNET_WM_STATE_MAXIMIZED_VERT = MakeAtom("_NET_WM_STATE_MAXIMIZED_VERT", 28, TRUE);
    aNET_WM_STATE_HIDDEN = MakeAtom("_NET_WM_STATE_HIDDEN", 20, TRUE);
    aNET_WM_STATE_DEMANDS_ATTENTION = MakeAtom("_NET_WM_STATE_DEMANDS_ATTENTION", 31, TRUE);
    aNET_WM_STATE_MODAL = MakeAtom("_NET_WM_STATE_MODAL", 19, TRUE);
    aNET_ACTIVE_WINDOW = MakeAtom("_NET_ACTIVE_WINDOW", 18, TRUE);
    aNET_CLIENT_LIST = MakeAtom("_NET_CLIENT_LIST", 16, TRUE);
    aNET_SUPPORTED = MakeAtom("_NET_SUPPORTED", 14, TRUE);
    aNET_SUPPORTING_WM_CHECK = MakeAtom("_NET_SUPPORTING_WM_CHECK", 24, TRUE);
    aNET_CLOSE_WINDOW = MakeAtom("_NET_CLOSE_WINDOW", 17, TRUE);
    aNET_WM_PID = MakeAtom("_NET_WM_PID", 11, TRUE);
}

/* ---- per-frame state ----------------------------------------------------- */
#define MAX_TITLE 256
#define MAX_ICON_DIM 256
#define CLOSE_GRACE_MS 5000

typedef struct managed {
    struct managed *next;
    WindowPtr win;
    struct amberwin_frame *frame;
    Bool dirty;                 /* a property of interest changed */
    Bool minimized, maximized, fullscreen, urgent, modal;
    OsTimerPtr closeTimer;      /* running after WM_DELETE_WINDOW was sent */
} Managed, *ManagedPtr;

static ManagedPtr managed;
static ScreenPtr wmScreen;
static WindowPtr wmCheckWindow;
static Bool wmReady;
static Bool listDirty;

static ManagedPtr
find_managed(WindowPtr pWin)
{
    ManagedPtr m;
    for (m = managed; m; m = m->next)
        if (m->win == pWin)
            return m;
    return NULL;
}

static ManagedPtr
find_managed_by_xid(XID xid)
{
    ManagedPtr m;
    for (m = managed; m; m = m->next)
        if (m->win->drawable.id == xid)
            return m;
    return NULL;
}

/* ---- property reading -------------------------------------------------- */
static PropertyPtr
get_prop(WindowPtr pWin, Atom name)
{
    PropertyPtr p = NULL;
    if (dixLookupProperty(&p, pWin, name, serverClient, DixReadAccess) != Success)
        return NULL;
    return p;
}

/* A remote-supplied title: printable, no control characters, no line
 * breaks, cut at MAX_TITLE bytes on a UTF-8 boundary. */
static void
sanitise_title(const unsigned char *src, unsigned len, char *out)
{
    unsigned i = 0, o = 0;
    if (len > MAX_TITLE)
        len = MAX_TITLE;
    while (i < len && o < MAX_TITLE - 1) {
        unsigned char c = src[i];
        unsigned n = (c < 0x80) ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (c < 0x20 || c == 0x7f) {
            out[o++] = ' ';
            i++;
            continue;
        }
        if (i + n > len || o + n >= MAX_TITLE - 1)
            break;
        memcpy(out + o, src + i, n);
        o += n;
        i += n;
    }
    out[o] = '\0';
}

static void
apply_title(ManagedPtr m)
{
    char title[MAX_TITLE];
    PropertyPtr p = get_prop(m->win, aNET_WM_NAME);
    if (!p || p->type != aUTF8_STRING || p->format != 8) {
        p = get_prop(m->win, aWM_NAME);
        if (p && p->format != 8)
            p = NULL;
    }
    if (p && p->size)
        sanitise_title(p->data, p->size, title);
    else
        strcpy(title, "X11 window");
    amberwin_frame_set_title(m->frame, title);
}

static Bool
has_protocol(WindowPtr pWin, Atom proto)
{
    PropertyPtr p = get_prop(pWin, aWM_PROTOCOLS);
    unsigned i;
    if (!p || p->format != 32 || p->type != XA_ATOM)
        return FALSE;
    for (i = 0; i < p->size; i++)
        if (((CARD32 *) p->data)[i] == proto)
            return TRUE;
    return FALSE;
}

static void
apply_hints(ManagedPtr m)
{
    PropertyPtr p = get_prop(m->win, aWM_NORMAL_HINTS);
    int minw = 0, minh = 0, maxw = 0, maxh = 0, resizable = 1;
    if (p && p->format == 32 && p->size >= 15) {
        const CARD32 *h = p->data;   /* XSizeHints wire layout */
        CARD32 flags = h[0];
        if (flags & PMinSize) { minw = (int) h[5]; minh = (int) h[6]; }
        if (flags & PMaxSize) { maxw = (int) h[7]; maxh = (int) h[8]; }
        if ((flags & PMinSize) && (flags & PMaxSize) && minw == maxw && minh == maxh && minw > 0)
            resizable = 0;
    }
    amberwin_frame_set_hints(m->frame, minw, minh, maxw, maxh, resizable);
}

static void
apply_transient(ManagedPtr m)
{
    PropertyPtr p = get_prop(m->win, aWM_TRANSIENT_FOR);
    struct amberwin_frame *owner = NULL;
    if (p && p->format == 32 && p->size >= 1 && p->type == XA_WINDOW) {
        ManagedPtr o = find_managed_by_xid(((CARD32 *) p->data)[0]);
        if (o && o != m)
            owner = o->frame;
    }
    amberwin_frame_set_transient(m->frame, owner, m->modal);
}

static void
apply_icon(ManagedPtr m)
{
    PropertyPtr p = get_prop(m->win, aNET_WM_ICON);
    const CARD32 *d;
    unsigned n, best = 0, bestw = 0, besth = 0, i = 0;
    if (!p || p->format != 32 || p->type != XA_CARDINAL) {
        amberwin_frame_set_icon(m->frame, 0, 0, NULL);
        return;
    }
    d = p->data;
    n = p->size;
    /* pick the largest icon that fits the limit; every length is checked
     * against the property before it is trusted */
    while (i + 2 <= n) {
        CARD32 w = d[i], h = d[i + 1];
        if (w == 0 || h == 0 || w > MAX_ICON_DIM || h > MAX_ICON_DIM)
            break;
        if ((unsigned long) w * h > n - (i + 2))
            break;
        if (w * h > bestw * besth) {
            best = i + 2;
            bestw = w;
            besth = h;
        }
        i += 2 + w * h;
    }
    if (bestw)
        amberwin_frame_set_icon(m->frame, (int) bestw, (int) besth, d + best);
    else
        amberwin_frame_set_icon(m->frame, 0, 0, NULL);
}

static void
read_net_wm_state(ManagedPtr m)
{
    PropertyPtr p = get_prop(m->win, aNET_WM_STATE);
    unsigned i;
    Bool fs = FALSE, mh = FALSE, mv = FALSE, hidden = FALSE, att = FALSE, modal = FALSE;
    if (p && p->format == 32 && p->type == XA_ATOM) {
        for (i = 0; i < p->size; i++) {
            Atom a = ((CARD32 *) p->data)[i];
            if (a == aNET_WM_STATE_FULLSCREEN) fs = TRUE;
            else if (a == aNET_WM_STATE_MAXIMIZED_HORZ) mh = TRUE;
            else if (a == aNET_WM_STATE_MAXIMIZED_VERT) mv = TRUE;
            else if (a == aNET_WM_STATE_HIDDEN) hidden = TRUE;
            else if (a == aNET_WM_STATE_DEMANDS_ATTENTION) att = TRUE;
            else if (a == aNET_WM_STATE_MODAL) modal = TRUE;
        }
    }
    m->fullscreen = fs;
    m->maximized = mh && mv;
    m->minimized = hidden;
    m->urgent = att;
    m->modal = modal;
}

static void
apply_urgency(ManagedPtr m)
{
    PropertyPtr p = get_prop(m->win, aWM_HINTS);
    if (p && p->format == 32 && p->size >= 1 && (((CARD32 *) p->data)[0] & (1L << 8)))
        m->urgent = TRUE;   /* XUrgencyHint */
}

static void
write_net_wm_state(ManagedPtr m)
{
    CARD32 atoms[6];
    int n = 0;
    if (m->fullscreen) atoms[n++] = aNET_WM_STATE_FULLSCREEN;
    if (m->maximized) { atoms[n++] = aNET_WM_STATE_MAXIMIZED_HORZ; atoms[n++] = aNET_WM_STATE_MAXIMIZED_VERT; }
    if (m->minimized) atoms[n++] = aNET_WM_STATE_HIDDEN;
    if (m->urgent) atoms[n++] = aNET_WM_STATE_DEMANDS_ATTENTION;
    if (m->modal) atoms[n++] = aNET_WM_STATE_MODAL;
    dixChangeWindowProperty(serverClient, m->win, aNET_WM_STATE, XA_ATOM, 32,
                            PropModeReplace, (unsigned long) n, atoms, TRUE);
}

static void
write_wm_state(ManagedPtr m)
{
    CARD32 v[2];
    v[0] = m->minimized ? IconicState : NormalState;
    v[1] = None;
    dixChangeWindowProperty(serverClient, m->win, aWM_STATE, aWM_STATE, 32,
                            PropModeReplace, 2, v, TRUE);
}

static void
apply_state(ManagedPtr m)
{
    amberwin_frame_set_state(m->frame, m->minimized, m->maximized, m->fullscreen, m->urgent);
}

static void
apply_all(ManagedPtr m)
{
    read_net_wm_state(m);
    apply_urgency(m);
    apply_title(m);
    apply_hints(m);
    apply_transient(m);
    apply_icon(m);
    apply_state(m);
    write_wm_state(m);
    m->dirty = FALSE;
}

/* ---- root properties ------------------------------------------------------ */
static void
write_client_list(void)
{
    CARD32 ids[256];
    int n = 0;
    ManagedPtr m;
    if (!wmReady)
        return;
    for (m = managed; m && n < 256; m = m->next)
        if (!m->win->overrideRedirect)
            ids[n++] = m->win->drawable.id;
    dixChangeWindowProperty(serverClient, wmScreen->root, aNET_CLIENT_LIST, XA_WINDOW, 32,
                            PropModeReplace, (unsigned long) n, ids, TRUE);
    listDirty = FALSE;
}

static void
write_active_window(XID xid)
{
    CARD32 v = xid;
    if (!wmReady)
        return;
    dixChangeWindowProperty(serverClient, wmScreen->root, aNET_ACTIVE_WINDOW, XA_WINDOW, 32,
                            PropModeReplace, 1, &v, TRUE);
}

/* The manager announces itself the way EWMH asks: a check window whose
 * _NET_SUPPORTING_WM_CHECK points at itself, and the list of what is
 * supported. Done once the root exists. */
static void
wm_ready_init(void)
{
    int err = Success;
    CARD32 v;
    CARD32 supported[12];
    int n = 0;
    static const char name[] = "AmberXWM";

    if (wmReady || !wmScreen || !wmScreen->root)
        return;
    wmCheckWindow = CreateWindow(FakeClientID(0), wmScreen->root, -1, -1, 1, 1, 0,
                                 InputOnly, 0, NULL, 0, serverClient, CopyFromParent, &err);
    if (!wmCheckWindow || err != Success)
        return;
    v = wmCheckWindow->drawable.id;
    dixChangeWindowProperty(serverClient, wmCheckWindow, aNET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
                            PropModeReplace, 1, &v, FALSE);
    dixChangeWindowProperty(serverClient, wmCheckWindow, aNET_WM_NAME, aUTF8_STRING, 8,
                            PropModeReplace, sizeof name - 1, name, FALSE);
    dixChangeWindowProperty(serverClient, wmScreen->root, aNET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
                            PropModeReplace, 1, &v, TRUE);
    supported[n++] = aNET_SUPPORTING_WM_CHECK;
    supported[n++] = aNET_CLIENT_LIST;
    supported[n++] = aNET_ACTIVE_WINDOW;
    supported[n++] = aNET_CLOSE_WINDOW;
    supported[n++] = aNET_WM_NAME;
    supported[n++] = aNET_WM_ICON;
    supported[n++] = aNET_WM_STATE;
    supported[n++] = aNET_WM_STATE_FULLSCREEN;
    supported[n++] = aNET_WM_STATE_MAXIMIZED_HORZ;
    supported[n++] = aNET_WM_STATE_MAXIMIZED_VERT;
    supported[n++] = aNET_WM_STATE_HIDDEN;
    supported[n++] = aNET_WM_STATE_DEMANDS_ATTENTION;
    dixChangeWindowProperty(serverClient, wmScreen->root, aNET_SUPPORTED, XA_ATOM, 32,
                            PropModeReplace, (unsigned long) n, supported, TRUE);
    wmReady = TRUE;
    write_client_list();
}

/* ---- client messages the WM sends ----------------------------------------- */
static void
send_protocol(WindowPtr pWin, Atom proto)
{
    xEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.u.u.type = ClientMessage | 0x80;   /* send_event */
    ev.u.u.detail = 32;
    ev.u.clientMessage.window = pWin->drawable.id;
    ev.u.clientMessage.u.l.type = aWM_PROTOCOLS;
    ev.u.clientMessage.u.l.longs0 = proto;
    ev.u.clientMessage.u.l.longs1 = GetTimeInMillis();
    /* XSendEvent with an empty mask goes to the window's creator; this is
     * that delivery, server-side */
    WriteEventsToClient(wClient(pWin), 1, &ev);
}

static CARD32
close_grace_expired(OsTimerPtr timer, CARD32 now, void *arg)
{
    ManagedPtr m = arg;
    (void) timer; (void) now;
    m->closeTimer = NULL;
    /* the client had its chance: a WM would XKillClient here */
    if (m->win && wClient(m->win) && !wClient(m->win)->clientGone)
        CloseDownClient(wClient(m->win));
    return 0;
}

static void
request_close(ManagedPtr m)
{
    if (has_protocol(m->win, aWM_DELETE_WINDOW)) {
        send_protocol(m->win, aWM_DELETE_WINDOW);
        if (!m->closeTimer)
            m->closeTimer = TimerSet(NULL, 0, CLOSE_GRACE_MS, close_grace_expired, m);
    }
    else {
        CloseDownClient(wClient(m->win));
    }
}

static void
activate(ManagedPtr m)
{
    DeviceIntPtr kbd = inputInfo.keyboard;
    if (!kbd)
        return;
    SetInputFocus(serverClient, kbd, m->win->drawable.id, RevertToParent,
                  GetTimeInMillis(), FALSE);
    if (has_protocol(m->win, aWM_TAKE_FOCUS))
        send_protocol(m->win, aWM_TAKE_FOCUS);
    write_active_window(m->win->drawable.id);
}

/* ---- callbacks ------------------------------------------------------------- */
static void
property_changed(CallbackListPtr *pcbl, void *data, void *calldata)
{
    PropertyStateRec *rec = calldata;
    ManagedPtr m;
    Atom a;
    (void) pcbl; (void) data;
    if (!rec || !rec->win || !rec->prop)
        return;
    m = find_managed(rec->win);
    if (!m)
        return;
    a = rec->prop->propertyName;
    if (a == aWM_NAME || a == aNET_WM_NAME || a == aWM_NORMAL_HINTS || a == aWM_TRANSIENT_FOR ||
        a == aNET_WM_ICON || a == aWM_HINTS || a == aWM_PROTOCOLS)
        m->dirty = TRUE;
}

/* _NET_WM_STATE and friends are ClientMessages a client sends to the root
 * for the WM. Nothing selects them here, so they would vanish; the send
 * hook sees them first. */
static void
send_hook(CallbackListPtr *pcbl, void *data, void *calldata)
{
    XaceSendAccessRec *rec = calldata;
    int i;
    (void) pcbl; (void) data;
    if (!wmReady || !rec || rec->pWin != wmScreen->root || !rec->events)
        return;
    for (i = 0; i < rec->count; i++) {
        xEvent *ev = &rec->events[i];
        ManagedPtr m;
        if ((ev->u.u.type & 0x7f) != ClientMessage || ev->u.u.detail != 32)
            continue;
        m = find_managed_by_xid(ev->u.clientMessage.window);
        if (!m)
            continue;
        if (ev->u.clientMessage.u.l.type == aNET_WM_STATE) {
            CARD32 action = ev->u.clientMessage.u.l.longs0;
            Atom a1 = ev->u.clientMessage.u.l.longs1, a2 = ev->u.clientMessage.u.l.longs2;
            int k;
            for (k = 0; k < 2; k++) {
                Atom a = k ? a2 : a1;
                Bool *flag = NULL;
                if (a == aNET_WM_STATE_FULLSCREEN) flag = &m->fullscreen;
                else if (a == aNET_WM_STATE_MAXIMIZED_HORZ || a == aNET_WM_STATE_MAXIMIZED_VERT) flag = &m->maximized;
                else if (a == aNET_WM_STATE_HIDDEN) flag = &m->minimized;
                else if (a == aNET_WM_STATE_DEMANDS_ATTENTION) flag = &m->urgent;
                else if (a == aNET_WM_STATE_MODAL) flag = &m->modal;
                if (!flag)
                    continue;
                *flag = action == 0 ? FALSE : action == 1 ? TRUE : !*flag;
            }
            apply_state(m);
            apply_transient(m);
            write_net_wm_state(m);
            write_wm_state(m);
        }
        else if (ev->u.clientMessage.u.l.type == aNET_ACTIVE_WINDOW) {
            amberwin_frame_activate(m->frame);
        }
        else if (ev->u.clientMessage.u.l.type == aNET_CLOSE_WINDOW) {
            request_close(m);
        }
    }
}

static void
wm_block_handler(void *blockData, void *timeout)
{
    ManagedPtr m;
    (void) blockData; (void) timeout;
    wm_ready_init();
    for (m = managed; m; m = m->next)
        if (m->dirty)
            apply_all(m);
    if (listDirty)
        write_client_list();
}

static void
wm_wakeup_handler(void *blockData, int result)
{
    (void) blockData; (void) result;
}

/* ---- the DDX-facing surface -------------------------------------------------- */
Bool
amber_wm_screen_init(ScreenPtr pScreen)
{
    wmScreen = pScreen;
    make_atoms();
    if (!AddCallback(&PropertyStateCallback, property_changed, NULL))
        return FALSE;
    if (!XaceRegisterCallback(XACE_SEND_ACCESS, send_hook, NULL))
        return FALSE;
    return RegisterBlockAndWakeupHandlers(wm_block_handler, wm_wakeup_handler, pScreen);
}

void
amber_wm_frame_created(WindowPtr pWin, struct amberwin_frame *f)
{
    ManagedPtr m = calloc(1, sizeof *m);
    if (!m)
        return;
    m->win = pWin;
    m->frame = f;
    m->next = managed;
    managed = m;
    wm_ready_init();
    apply_all(m);
    listDirty = TRUE;
}

void
amber_wm_frame_destroyed(WindowPtr pWin, struct amberwin_frame *f)
{
    ManagedPtr *pp;
    (void) pWin;
    for (pp = &managed; *pp; pp = &(*pp)->next) {
        if ((*pp)->frame == f) {
            ManagedPtr m = *pp;
            *pp = m->next;
            if (m->closeTimer)
                TimerFree(m->closeTimer);
            free(m);
            break;
        }
    }
    listDirty = TRUE;
}

void
amber_wm_frame_switched(WindowPtr pNew, WindowPtr pOld, struct amberwin_frame *f)
{
    ManagedPtr m;
    (void) pOld;
    for (m = managed; m; m = m->next)
        if (m->frame == f) {
            m->win = pNew;
            m->dirty = TRUE;
        }
}

void
amber_wm_event(const struct amberwin_event *ev)
{
    ManagedPtr m = find_managed_by_xid(ev->xid);
    if (!m)
        return;
    switch (ev->type) {
    case AMBERWIN_EV_FRAME_CLOSE:
        request_close(m);
        break;
    case AMBERWIN_EV_FRAME_CONFIGURE: {
        XID vlist[4];
        Mask mask = 0;
        WindowPtr w = m->win;
        int bw = wBorderWidth(w);
        if (w->drawable.x - bw != ev->x || w->drawable.y - bw != ev->y) {
            mask |= CWX | CWY;
        }
        if ((int) w->drawable.width != ev->w || (int) w->drawable.height != ev->h)
            mask |= CWWidth | CWHeight;
        if (mask) {
            int n = 0;
            if (mask & CWX) { vlist[n++] = ev->x; vlist[n++] = ev->y; }
            if (mask & CWWidth) { vlist[n++] = ev->w > 0 ? ev->w : 1; vlist[n++] = ev->h > 0 ? ev->h : 1; }
            ConfigureWindow(w, mask, vlist, serverClient);
        }
        break;
    }
    case AMBERWIN_EV_FRAME_ACTIVATE:
        if (ev->pressed)
            activate(m);
        break;
    case AMBERWIN_EV_FRAME_STATE:
        m->minimized = ev->x != 0;
        m->maximized = ev->y != 0;
        write_net_wm_state(m);
        write_wm_state(m);
        break;
    default:
        break;
    }
}
