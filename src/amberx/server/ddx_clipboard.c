/* ddx_clipboard.c — the X half of the clipboard bridge. Original AmberSSH
 * file.
 *
 * Text only, UTF-8, bounded, and only in the directions the session's mode
 * allows. The policy itself lives in AmberSSH; this half enforces its own
 * copy of it, so a host that has been talked into misbehaving still cannot
 * move text in a direction the user did not enable.
 *
 * Local → remote (the Windows clipboard reaching an X client):
 *   AmberSSH sends the text down; the server takes ownership of CLIPBOARD
 *   and PRIMARY and holds it. A client asking for the selection is answered
 *   from that buffer. The answer is written straight into the requestor's
 *   property, so it never passes through a request and never needs INCR.
 *
 * Remote → local (an X client's selection reaching Windows):
 *   When a client takes CLIPBOARD, the server sends it a SelectionRequest
 *   for UTF8_STRING naming the owner's OWN window as the requestor, then
 *   watches that window's properties. (Naming a server-owned window there
 *   is the obvious design and the wrong one: a restricted client may not put
 *   a property on a window the server owns, so the transfer would work only
 *   for trusted clients — see request_from_owner.) The answer arrives as a
 *   property; if its type is
 *   INCR the value follows in pieces, each announced by a property change,
 *   and deleting the property is what asks for the next one. The assembled
 *   text goes up to AmberSSH, which decides what to do with it.
 *
 * Why the server answers ConvertSelection itself rather than owning the
 * selection as an ordinary client: there is no ordinary client to be. The
 * server client cannot receive events — WriteToClient discards them by
 * construction — so a SelectionRequest addressed to it would go nowhere.
 * Wrapping the request instead is both smaller and impossible to miss.
 *
 * Nothing here logs clipboard content, and nothing keeps it after the
 * session: the buffers are freed when the screen closes. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "windowstr.h"
#include "propertyst.h"
#include "selection.h"
#include "property.h"
#include "scrnintstr.h"
#include "amberos.h"
#include "amberwin.h"
#include "amberlimits.h"

/* ---- atoms and state ---------------------------------------------------- */
static Atom aClipboard, aTargets, aTimestamp, aUtf8, aIncr, aTransfer, aText;
static WindowPtr clipWindow;        /* the server's own transfer window */
static ScreenPtr clipScreen;

/* local → remote: what the server hands to a client that asks */
static char *ownedText;
static unsigned long ownedLen;
static TimeStamp ownedTime;

/* remote → local: the transfer in progress */
static enum { IDLE, WAITING, INCREMENTAL } incoming;
static WindowPtr incomingWindow;   /* the owner window the answer lands on */
static char *incomingBuf;
static unsigned long incomingLen;
static OsTimerPtr incomingTimer;

static int (*origConvertSelection) (ClientPtr);
static int (*origSConvertSelection) (ClientPtr);

static int mode_of(void)
{
    return amberwin_get_config()->clipboard_mode;
}

static Bool
may_send_to_remote(void)
{
    const int m = mode_of();
    return m == AMBERWIN_CLIP_ASK || m == AMBERWIN_CLIP_TO_REMOTE || m == AMBERWIN_CLIP_BOTH;
}

static Bool
may_send_to_local(void)
{
    const int m = mode_of();
    return m == AMBERWIN_CLIP_ASK || m == AMBERWIN_CLIP_TO_LOCAL || m == AMBERWIN_CLIP_BOTH;
}

/* The window the server receives selection transfers on: an InputOnly child
 * of the root, never mapped, so the rootless layer makes no frame for it.
 * Created on first use because the root does not exist while the screen is
 * being initialised. */
static Bool
ensure_window(void)
{
    XID wid;
    int rc = Success;

    if (clipWindow)
        return TRUE;
    if (!clipScreen || !clipScreen->root)
        return FALSE;
    wid = FakeClientID(0);
    clipWindow = CreateWindow(wid, clipScreen->root, 0, 0, 1, 1, 0, InputOnly, 0,
                              NULL, 0, serverClient, CopyFromParent, &rc);
    /* CreateWindow builds the window; registering the id is the caller's job
     * (ProcCreateWindow does it too). Without it the window exists and its
     * id resolves to nothing, so the client asked to put a property on it
     * gets BadWindow — which is exactly what happened the first time. */
    if (!clipWindow || rc != Success ||
        !AddResource(clipWindow->drawable.id, RT_WINDOW, clipWindow)) {
        clipWindow = NULL;
        LogMessage(X_ERROR, "AmberX: the clipboard transfer window could not be created\n");
        return FALSE;
    }
    return TRUE;
}

/* ---- local → remote: the server owns the selection ------------------------ */
static void
own_selection(Atom sel)
{
    Selection *pSel = NULL;

    if (dixLookupSelection(&pSel, sel, serverClient, DixSetAttrAccess) != Success || !pSel) {
        /* no entry yet: make one the way dix does */
        pSel = calloc(1, sizeof(Selection));
        if (!pSel)
            return;
        pSel->selection = sel;
        pSel->next = CurrentSelections;
        CurrentSelections = pSel;
    }
    pSel->lastTimeChanged = currentTime;
    pSel->window = clipWindow->drawable.id;
    pSel->pWin = clipWindow;
    pSel->client = serverClient;
    ownedTime = currentTime;
}

/* Answers one ConvertSelection out of the buffer. Returns TRUE when this
 * request was ours to answer. */
static Bool
answer_conversion(ClientPtr client, Atom selection, Atom target, Atom property,
                  Window requestorId, Time time)
{
    WindowPtr pReq;
    xEvent ev;
    Atom actual = property;
    int rc;

    if (!ownedText || !may_send_to_remote())
        return FALSE;
    if (selection != aClipboard && selection != XA_PRIMARY)
        return FALSE;

    rc = dixLookupWindow(&pReq, requestorId, client, DixSetPropAccess);
    if (rc != Success)
        return FALSE;
    if (property == None)
        actual = target;            /* obsolete clients; answer where they look */

    if (target == aTargets) {
        Atom targets[4] = { aTargets, aTimestamp, aUtf8, XA_STRING };
        rc = dixChangeWindowProperty(serverClient, pReq, actual, XA_ATOM, 32,
                                     PropModeReplace, 4, targets, TRUE);
    }
    else if (target == aTimestamp) {
        CARD32 when = ownedTime.milliseconds;
        rc = dixChangeWindowProperty(serverClient, pReq, actual, XA_INTEGER, 32,
                                     PropModeReplace, 1, &when, TRUE);
    }
    else if (target == aUtf8 || target == XA_STRING || target == aText) {
        rc = dixChangeWindowProperty(serverClient, pReq, actual,
                                     target == aUtf8 ? aUtf8 : XA_STRING, 8,
                                     PropModeReplace, ownedLen, ownedText, TRUE);
    }
    else {
        actual = None;              /* a target the bridge does not do */
        rc = Success;
    }
    if (rc != Success)
        actual = None;

    memset(&ev, 0, sizeof ev);
    ev.u.u.type = SelectionNotify;
    ev.u.selectionNotify.time = time;
    ev.u.selectionNotify.requestor = requestorId;
    ev.u.selectionNotify.selection = selection;
    ev.u.selectionNotify.target = target;
    ev.u.selectionNotify.property = actual;
    WriteEventsToClient(client, 1, &ev);
    return TRUE;
}

static int
clip_ProcConvertSelection(ClientPtr client)
{
    const xConvertSelectionReq *req = client->requestBuffer;
    Selection *pSel = NULL;

    if (client->req_len >= bytes_to_int32(sizeof(xConvertSelectionReq)) &&
        (req->selection == aClipboard || req->selection == XA_PRIMARY) &&
        dixLookupSelection(&pSel, req->selection, serverClient, DixGetAttrAccess) == Success &&
        pSel && pSel->client == serverClient &&
        answer_conversion(client, req->selection, req->target, req->property,
                          req->requestor, req->time))
        return Success;
    return (*origConvertSelection) (client);
}

static int
clip_SProcConvertSelection(ClientPtr client)
{
    /* The swapped procedure swaps the request in place and then calls the
     * native one, which is this file's wrapper, so there is nothing to do
     * here but pass it on. */
    return (*origSConvertSelection) (client);
}

/* ---- remote → local: ask the owner, then read the property --------------- */
static void
incoming_reset(void)
{
    incoming = IDLE;
    incomingWindow = NULL;
    free(incomingBuf);
    incomingBuf = NULL;
    incomingLen = 0;
    if (incomingTimer) {
        TimerFree(incomingTimer);
        incomingTimer = NULL;
    }
}

static CARD32
incoming_timed_out(OsTimerPtr timer, CARD32 now, void *arg)
{
    (void) timer; (void) now; (void) arg;
    if (incoming != IDLE) {
        LogMessage(X_WARNING, "AmberX: a clipboard transfer from the session timed out\n");
        incomingTimer = NULL;       /* freed by the timer layer on return 0 */
        incoming_reset();
    }
    return 0;
}

static void
arm_incoming_timer(void)
{
    incomingTimer = TimerSet(incomingTimer, 0,
                             (CARD32) amberLimits.selection_timeout_ms,
                             incoming_timed_out, NULL);
}

/* Asks the current owner of `sel` for its text.
 *
 * The requestor named in the request is the owner's OWN window, not one of
 * the server's. That looks odd until you try it the other way: in restricted
 * mode an untrusted client may not put a property on a window the server
 * owns — that is precisely the access restricted mode exists to enforce, and
 * it is the same check that makes ChangeProperty on the root fail — so a
 * transfer through a server-owned window works for trusted clients and fails
 * for exactly the clients the default mode is built for. Asking the owner to
 * write on its own window is allowed in both modes, and the server, being
 * trusted, may read it back. ICCCM does not require the requestor to be a
 * window the requesting party owns, and every toolkit writes wherever it is
 * told. */
static void
request_from_owner(Selection *pSel)
{
    xEvent ev;

    if (!pSel || !pSel->client || pSel->client == serverClient || !pSel->pWin)
        return;
    if (incoming != IDLE)
        return;                     /* one at a time; the timer bounds it */

    memset(&ev, 0, sizeof ev);
    ev.u.u.type = SelectionRequest;
    ev.u.selectionRequest.time = currentTime.milliseconds;
    ev.u.selectionRequest.owner = pSel->window;
    ev.u.selectionRequest.requestor = pSel->window;
    ev.u.selectionRequest.selection = pSel->selection;
    ev.u.selectionRequest.target = aUtf8;
    ev.u.selectionRequest.property = aTransfer;
    WriteEventsToClient(pSel->client, 1, &ev);

    incomingWindow = pSel->pWin;
    incoming = WAITING;
    arm_incoming_timer();
}

/* Appends `len` bytes, refusing to grow past the limit. */
static Bool
incoming_append(const char *data, unsigned long len)
{
    char *grown;
    if (incomingLen + len > (unsigned long) amberLimits.clipboard_max_bytes) {
        LogMessage(X_WARNING, "AmberX: a clipboard transfer over the size limit was dropped\n");
        return FALSE;
    }
    grown = realloc(incomingBuf, incomingLen + len + 1);
    if (!grown)
        return FALSE;
    incomingBuf = grown;
    memcpy(incomingBuf + incomingLen, data, len);
    incomingLen += len;
    incomingBuf[incomingLen] = '\0';
    return TRUE;
}

static void
incoming_finished(void)
{
    if (incomingBuf && incomingLen > 0 && may_send_to_local())
        amberwin_clipboard_push(incomingBuf, (uint32_t) incomingLen);
    incoming_reset();
}

/* Every property change on the transfer window is a step of the protocol. */
static void
property_hook(CallbackListPtr *pcbl, void *data, void *calldata)
{
    PropertyStateRec *rec = calldata;
    PropertyPtr prop;
    (void) pcbl; (void) data;

    if (!incomingWindow || rec->win != incomingWindow || rec->state != PropertyNewValue)
        return;
    prop = rec->prop;
    if (!prop || prop->propertyName != aTransfer)
        return;

    if (incoming == WAITING && prop->type == aIncr) {
        /* the owner will send the value in pieces; deleting the property is
         * the acknowledgement that starts them */
        incoming = INCREMENTAL;
        DeleteProperty(serverClient, incomingWindow, aTransfer);
        arm_incoming_timer();
        return;
    }
    if (incoming == WAITING) {
        if (prop->format == 8 && prop->size > 0)
            incoming_append(prop->data, (unsigned long) prop->size);
        DeleteProperty(serverClient, incomingWindow, aTransfer);
        incoming_finished();
        return;
    }
    if (incoming == INCREMENTAL) {
        if (prop->size == 0) {      /* zero-length piece: the end */
            DeleteProperty(serverClient, incomingWindow, aTransfer);
            incoming_finished();
            return;
        }
        if (prop->format == 8)
            if (!incoming_append(prop->data, (unsigned long) prop->size)) {
                DeleteProperty(serverClient, incomingWindow, aTransfer);
                incoming_reset();
                return;
            }
        DeleteProperty(serverClient, incomingWindow, aTransfer);
        arm_incoming_timer();
    }
}

/* A client took a selection: if it is the clipboard and the session allows
 * it, ask for the text now rather than when someone pastes, so the Windows
 * clipboard is what the user expects it to be. */
static void
selection_hook(CallbackListPtr *pcbl, void *data, void *calldata)
{
    SelectionInfoRec *rec = calldata;
    (void) pcbl; (void) data;

    if (rec->kind != SelectionSetOwner || !rec->selection)
        return;
    if (rec->selection->selection != aClipboard)
        return;                     /* PRIMARY changes on every drag-select */
    if (rec->selection->client == serverClient)
        return;                     /* our own ownership, not a client's */
    if (!may_send_to_local())
        return;

    /* the server no longer owns it, so its buffer is stale */
    free(ownedText);
    ownedText = NULL;
    ownedLen = 0;

    request_from_owner(rec->selection);
}

/* ---- the Windows side pushed new text ------------------------------------ */
void
amber_clipboard_from_windows(void)
{
    char *buf;
    uint32_t len;

    if (!may_send_to_remote() || !ensure_window())
        return;
    buf = malloc((size_t) amberLimits.clipboard_max_bytes + 1);
    if (!buf)
        return;
    len = amberwin_clipboard_pull(buf, (uint32_t) amberLimits.clipboard_max_bytes);
    if (len == 0) {
        free(buf);
        return;
    }
    free(ownedText);
    ownedText = buf;
    ownedLen = len;
    own_selection(aClipboard);
    own_selection(XA_PRIMARY);
}

/* ---- installation --------------------------------------------------------- */
Bool
amber_clipboard_screen_init(ScreenPtr pScreen)
{
    XID wid;
    int rc = Success;

    if (mode_of() == AMBERWIN_CLIP_OFF)
        return TRUE;                /* nothing installed, nothing to go wrong */

    aClipboard = MakeAtom("CLIPBOARD", 9, TRUE);
    aTargets = MakeAtom("TARGETS", 7, TRUE);
    aTimestamp = MakeAtom("TIMESTAMP", 9, TRUE);
    aUtf8 = MakeAtom("UTF8_STRING", 11, TRUE);
    aIncr = MakeAtom("INCR", 4, TRUE);
    aText = MakeAtom("TEXT", 4, TRUE);
    aTransfer = MakeAtom("AMBERX_SELECTION", 16, TRUE);
    if (!aClipboard || !aTargets || !aTimestamp || !aUtf8 || !aIncr || !aTransfer)
        return FALSE;

    /* The transfer window is made on first use, not here: screen init runs
     * inside InitOutput, and dix does not create the root window until
     * afterwards, so there is nothing to parent it to yet. */
    clipScreen = pScreen;
    (void) wid;
    (void) rc;

    if (!origConvertSelection) {
        origConvertSelection = ProcVector[X_ConvertSelection];
        origSConvertSelection = SwappedProcVector[X_ConvertSelection];
        ProcVector[X_ConvertSelection] = clip_ProcConvertSelection;
        SwappedProcVector[X_ConvertSelection] = clip_SProcConvertSelection;
    }
    if (!AddCallback(&SelectionCallback, selection_hook, NULL))
        return FALSE;
    if (!AddCallback(&PropertyStateCallback, property_hook, NULL))
        return FALSE;

    LogMessage(X_INFO, "AmberX: clipboard bridge active, mode %d, text only, %lld bytes maximum\n",
               mode_of(), amberLimits.clipboard_max_bytes);
    return TRUE;
}

void
amber_clipboard_close(void)
{
    incoming_reset();
    free(ownedText);
    ownedText = NULL;
    ownedLen = 0;
    clipWindow = NULL;
    clipScreen = NULL;
}
