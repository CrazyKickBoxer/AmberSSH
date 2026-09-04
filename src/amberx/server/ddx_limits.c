/* ddx_limits.c — enforcement of amberlimits.h. Original AmberSSH file.
 *
 * Three mechanisms, each the smallest that reaches the thing limited:
 *
 *   * wrappers on the ProcVector entries for ChangeProperty and InternAtom
 *     see those requests before dix runs them, with the request bytes in
 *     client->requestBuffer, and refuse with BadAlloc before anything is
 *     allocated. (XACE's dispatch hook is not the tool here: in this
 *     xserver XaceHookDispatch fires for extension opcodes only, so core
 *     requests never reach XACE_CORE_DISPATCH. Wrapping the vector is what
 *     upstream extensions do for the same reason.)
 *   * the XACE resource hook sees every window creation; the screen's
 *     DestroyWindow wrapper sees every destruction; together they keep a
 *     per-client window count;
 *   * a CreatePixmap/DestroyPixmap wrapper on the screen judges dimensions
 *     and keeps the running total of pixmap storage.
 *
 * Refusing is BadAlloc — the error a client already expects for "the
 * server could not give you that" — so a well-written client survives. */
#include <dix-config.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "pixmapstr.h"
#include "resource.h"
#include "xace.h"
#include "xacestr.h"
#include "amberlimits.h"
#include "amberwin.h"

amber_limits amberLimits = {
    /* windows_per_client */    4096,
    /* windows_per_session */   16384,
    /* pixmap_max_dim */        8192,
    /* pixmap_max_bytes */      64LL * 1024 * 1024,
    /* pixmap_total_bytes */    512LL * 1024 * 1024,
    /* property_max_bytes */    1LL * 1024 * 1024,
    /* icon_max_dim */          256,
    /* request_max_bytes */     4LL * 1024 * 1024,
    /* atoms_per_window */      2000,
    /* atoms_window_ms */       10000,
    /* clipboard_max_bytes */   1LL * 1024 * 1024,
    /* selection_timeout_ms */  5000,
};

/* ---- per-client bookkeeping ------------------------------------------------ */
typedef struct {
    int windows;
    int atoms;
    CARD32 atomWindowStart;
} ClientLimitsRec;

static ClientLimitsRec perClient[MAXCLIENTS];
static int windowsTotal;
static long long pixmapBytesTotal;

static ClientLimitsRec *
limits_of(ClientPtr client)
{
    return &perClient[client->index];
}

/* ---- request wrappers ------------------------------------------------------ */
/* In this xserver the XACE dispatch hook runs only for extension opcodes,
 * so core requests are judged by wrapping their entries in the request
 * vectors: the wrapper looks at the request bytes and either refuses with
 * BadAlloc or hands the request to dix's own procedure. Both the native
 * and the byte-swapped vectors are wrapped; the swapped procedures swap
 * the request in place before calling the native one, which is why the
 * wrapper for a swapped client reads the fields swapped. */
static int (*origChangeProperty) (ClientPtr);
static int (*origSChangeProperty) (ClientPtr);
static int (*origInternAtom) (ClientPtr);
static int (*origSInternAtom) (ClientPtr);

static int
property_too_large(ClientPtr client, Bool swapped)
{
    const xChangePropertyReq *req = client->requestBuffer;
    unsigned long units, bytes;
    if (client->req_len < bytes_to_int32(sizeof(xChangePropertyReq)))
        return 0;   /* dix answers BadLength */
    units = swapped ? bswap_32(req->nUnits) : req->nUnits;
    bytes = units * (req->format == 32 ? 4 : req->format == 16 ? 2 : 1);
    if ((long long) bytes > amberLimits.property_max_bytes) {
        LogMessage(X_WARNING, "AmberX limit: client %d property of %lu bytes refused\n",
                   client->index, bytes);
        return 1;
    }
    return 0;
}

static int
limits_ProcChangeProperty(ClientPtr client)
{
    if (client != serverClient && property_too_large(client, FALSE))
        return BadAlloc;
    return (*origChangeProperty) (client);
}

static int
limits_SProcChangeProperty(ClientPtr client)
{
    if (client != serverClient && property_too_large(client, TRUE))
        return BadAlloc;
    return (*origSChangeProperty) (client);
}

static int
atom_rate_exceeded(ClientPtr client)
{
    const xInternAtomReq *req = client->requestBuffer;
    ClientLimitsRec *l = limits_of(client);
    CARD32 now = GetTimeInMillis();
    if (req->onlyIfExists)
        return 0;
    if (now - l->atomWindowStart > (CARD32) amberLimits.atoms_window_ms) {
        l->atomWindowStart = now;
        l->atoms = 0;
    }
    if (++l->atoms > amberLimits.atoms_per_window) {
        if (l->atoms == amberLimits.atoms_per_window + 1)
            LogMessage(X_WARNING, "AmberX limit: client %d atom creation rate exceeded\n",
                       client->index);
        return 1;
    }
    return 0;
}

static int
limits_ProcInternAtom(ClientPtr client)
{
    if (client != serverClient && atom_rate_exceeded(client))
        return BadAlloc;
    return (*origInternAtom) (client);
}

static int
limits_SProcInternAtom(ClientPtr client)
{
    if (client != serverClient && atom_rate_exceeded(client))
        return BadAlloc;
    return (*origSInternAtom) (client);
}

/* ---- windows per client ------------------------------------------------- */
static void
resource_hook(CallbackListPtr *pcbl, void *data, void *calldata)
{
    XaceResourceAccessRec *rec = calldata;
    (void) pcbl; (void) data;
    if (!(rec->access_mode & DixCreateAccess) || rec->rtype != RT_WINDOW)
        return;
    if (!rec->client || rec->client == serverClient)
        return;
    {
        ClientLimitsRec *l = limits_of(rec->client);
        if (l->windows >= amberLimits.windows_per_client ||
            windowsTotal >= amberLimits.windows_per_session) {
            LogMessage(X_WARNING, "AmberX limit: client %d window count refused\n",
                       rec->client->index);
            rec->status = BadAlloc;
            return;
        }
        l->windows++;
        windowsTotal++;
    }
}

static DestroyWindowProcPtr wrappedDestroyWindow;

static Bool
limits_DestroyWindow(WindowPtr pWin)
{
    ClientPtr owner = wClient(pWin);
    Bool ret;
    ScreenPtr pScreen = pWin->drawable.pScreen;
    if (owner && owner != serverClient) {
        ClientLimitsRec *l = limits_of(owner);
        if (l->windows > 0)
            l->windows--;
        if (windowsTotal > 0)
            windowsTotal--;
    }
    pScreen->DestroyWindow = wrappedDestroyWindow;
    ret = (*pScreen->DestroyWindow) (pWin);
    wrappedDestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = limits_DestroyWindow;
    return ret;
}

/* when a client goes, its counters go with it; the windows themselves are
 * destroyed through the wrapper as dix frees the client's resources */
static void
client_state(CallbackListPtr *pcbl, void *data, void *calldata)
{
    NewClientInfoRec *pci = calldata;
    (void) pcbl; (void) data;
    if (pci->client->clientState == ClientStateGone) {
        ClientLimitsRec *l = limits_of(pci->client);
        l->windows = 0;
        l->atoms = 0;
    }
}

/* ---- pixmaps -------------------------------------------------------------- */
static CreatePixmapProcPtr wrappedCreatePixmap;
static DestroyPixmapProcPtr wrappedDestroyPixmap;

static PixmapPtr
limits_CreatePixmap(ScreenPtr pScreen, int width, int height, int depth, unsigned usage_hint)
{
    PixmapPtr p;
    long long bytes = (long long) width * height * 4;
    if (width > amberLimits.pixmap_max_dim || height > amberLimits.pixmap_max_dim ||
        bytes > amberLimits.pixmap_max_bytes ||
        pixmapBytesTotal + bytes > amberLimits.pixmap_total_bytes) {
        LogMessage(X_WARNING, "AmberX limit: pixmap %dx%d refused (%lld bytes live)\n",
                   width, height, pixmapBytesTotal);
        return NullPixmap;
    }
    pScreen->CreatePixmap = wrappedCreatePixmap;
    p = (*pScreen->CreatePixmap) (pScreen, width, height, depth, usage_hint);
    wrappedCreatePixmap = pScreen->CreatePixmap;
    pScreen->CreatePixmap = limits_CreatePixmap;
    if (p) {
        /* what the pixmap really uses */
        long long real = (long long) p->devKind * p->drawable.height;
        pixmapBytesTotal += real > 0 ? real : bytes;
    }
    return p;
}

static Bool
limits_DestroyPixmap(PixmapPtr pPixmap)
{
    ScreenPtr pScreen = pPixmap->drawable.pScreen;
    Bool ret;
    long long real = (long long) pPixmap->devKind * pPixmap->drawable.height;
    Bool last = pPixmap->refcnt == 1;
    pScreen->DestroyPixmap = wrappedDestroyPixmap;
    ret = (*pScreen->DestroyPixmap) (pPixmap);
    wrappedDestroyPixmap = pScreen->DestroyPixmap;
    pScreen->DestroyPixmap = limits_DestroyPixmap;
    if (last && ret) {
        pixmapBytesTotal -= real;
        if (pixmapBytesTotal < 0)
            pixmapBytesTotal = 0;
    }
    return ret;
}

/* ---- installation ------------------------------------------------------------ */
int
amber_limits_screen_init(ScreenPtr pScreen)
{
    if (!origChangeProperty) {
        origChangeProperty = ProcVector[X_ChangeProperty];
        origSChangeProperty = SwappedProcVector[X_ChangeProperty];
        origInternAtom = ProcVector[X_InternAtom];
        origSInternAtom = SwappedProcVector[X_InternAtom];
        ProcVector[X_ChangeProperty] = limits_ProcChangeProperty;
        SwappedProcVector[X_ChangeProperty] = limits_SProcChangeProperty;
        ProcVector[X_InternAtom] = limits_ProcInternAtom;
        SwappedProcVector[X_InternAtom] = limits_SProcInternAtom;
    }
    if (!XaceRegisterCallback(XACE_RESOURCE_ACCESS, resource_hook, NULL))
        return 0;
    if (!AddCallback(&ClientStateCallback, client_state, NULL))
        return 0;
    wrappedDestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = limits_DestroyWindow;
    wrappedCreatePixmap = pScreen->CreatePixmap;
    pScreen->CreatePixmap = limits_CreatePixmap;
    wrappedDestroyPixmap = pScreen->DestroyPixmap;
    pScreen->DestroyPixmap = limits_DestroyPixmap;
    memset(perClient, 0, sizeof perClient);
    windowsTotal = 0;
    pixmapBytesTotal = 0;
    return 1;
}

void
amber_limits_os_init(void)
{
    /* BIG-REQUESTS: the largest request, in 4-byte units */
    maxBigRequestSize = (long) (amberLimits.request_max_bytes / 4);
}
