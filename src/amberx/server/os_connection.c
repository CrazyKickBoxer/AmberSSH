/* os_connection.c — clients over AmberXControl channels.
 *
 * This is the substantive port: what xserver/os/connection.c, io.c, access.c
 * and the wait half of WaitFor.c do over sockets, done over channels that
 * arrive already authenticated from the controller. It is smaller than what
 * it replaces because there is nothing to listen on, no host list to
 * consult, and no file descriptor to multiplex.
 *
 * The request framing in ReadRequestFromClient, and the coalescing in
 * WriteToClient/FlushClient, follow xserver/os/io.c (MIT; X Consortium,
 * Digital, Keith Packard) closely: that logic is the X protocol's, not a
 * platform's, and BIG-REQUESTS makes it subtle enough that re-deriving it
 * would be a way to introduce bugs upstream fixed twenty years ago. The
 * socket calls in it are replaced by amberwin_channel_read/write.
 * Original AmberSSH file with that derivation.
 *
 * Trust: every byte read here came from a remote machine. Lengths are
 * checked before they are used, oversized requests are skipped and turned
 * into BadLength by Dispatch, and a client whose channel closes is torn
 * down through the ordinary CloseDownClient path. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xmd.h>
#include <X11/extensions/bigreqsproto.h>
#include "misc.h"
#include "os.h"
#include "osdep.h"
#include "dixstruct.h"
#include "opaque.h"
#include "input.h"
#include "scrnintstr.h"
#include "amberos.h"
#include "amberwin.h"

#define BUFSIZE 16384
#define BUFWATERMARK 32768
#define MAX_CHANNELS 64

int LimitClients = MAX_CHANNELS + 1;    /* + the server client at index 0 */

CallbackListPtr ReplyCallback;
CallbackListPtr FlushCallback;

static Bool NewOutputPending;
static Bool CriticalOutputPending;
static Bool connections_ready;

/* channel id → client. A flat table: 64 entries is not worth a hash. */
static struct {
    CARD32 channel;
    ClientPtr client;
} channel_map[MAX_CHANNELS];

static ClientPtr
client_for_channel(CARD32 ch)
{
    int i;
    for (i = 0; i < MAX_CHANNELS; i++)
        if (channel_map[i].client && channel_map[i].channel == ch)
            return channel_map[i].client;
    return NULL;
}

static Bool
map_channel(CARD32 ch, ClientPtr client)
{
    int i;
    for (i = 0; i < MAX_CHANNELS; i++)
        if (!channel_map[i].client) {
            channel_map[i].channel = ch;
            channel_map[i].client = client;
            return TRUE;
        }
    return FALSE;
}

static void
unmap_channel(CARD32 ch)
{
    int i;
    for (i = 0; i < MAX_CHANNELS; i++)
        if (channel_map[i].client && channel_map[i].channel == ch)
            channel_map[i].client = NULL;
}

static unsigned int
open_channel_count(void)
{
    unsigned int n = 0;
    int i;
    for (i = 0; i < MAX_CHANNELS; i++)
        if (channel_map[i].client)
            n++;
    return n;
}

static uint64_t bytes_in_total;
static Bool cookie_seen;

static void
report(void)
{
    amberwin_report(open_channel_count(), bytes_in_total, cookie_seen);
}

/* ---- request length helpers (io.c) ------------------------------------- */
#define get_req_len(req,cli) ((cli)->swapped ? \
                              bswap_16((req)->length) : (req)->length)
#define get_big_req_len(req,cli) ((cli)->swapped ? \
                                  bswap_32(((xBigReq *)(req))->length) : \
                                  ((xBigReq *)(req))->length)

static void
YieldControl(void)
{
    isItTimeToYield = TRUE;
}

static void
YieldControlNoInput(ClientPtr client)
{
    YieldControl();
    mark_client_not_ready(client);
}

static void
YieldControlDeath(void)
{
    /* the caller returns -1 and Dispatch closes the client */
}

/* ---- lifecycle ---------------------------------------------------------- */
void
CreateWellKnownSockets(void)
{
    /* no sockets: the control pipe is the well-known "socket", and the
     * Windows side already owns it by the time the server starts */
    memset(channel_map, 0, sizeof channel_map);
    connections_ready = TRUE;
}

void
ResetWellKnownSockets(void)
{
    /* server reset (last client gone with -noreset off): nothing to reopen */
}

void
CloseWellKnownConnections(void)
{
}

void
ListenToAllClients(void)
{
}

int
OnlyListenToOneClient(ClientPtr client)
{
    (void) client;
    return Success;
    /* GrabServer with -noreset semantics on sockets; readiness of other
     * clients is already gated by the grab in dix */
}

/* The controller opened a channel: a forwarded X client connected on the
 * remote side and its SSH channel is up. Give it a ClientRec. */
void
amber_os_channel_opened(CARD32 channel)
{
    AmberCommPtr oc;
    ClientPtr client;

    if (!connections_ready || channel == 0 || client_for_channel(channel)) {
        amberwin_channel_close(channel);
        return;
    }
    oc = calloc(1, sizeof(AmberComm));
    if (!oc) {
        amberwin_channel_close(channel);
        return;
    }
    oc->channel = channel;
    oc->auth_id = None;
    oc->conn_time = GetTimeInMillis();
    if (!map_channel(channel, (ClientPtr) 1)) {    /* reserve the slot */
        free(oc);
        amberwin_channel_close(channel);
        return;
    }
    client = NextAvailableClient((void *) oc);
    if (!client) {
        unmap_channel(channel);
        free(oc);
        amberwin_channel_close(channel);
        return;
    }
    unmap_channel(channel);
    map_channel(channel, client);
    /* every AmberX client is a remote one: "local" grants nothing here and
     * would only mislead extensions that treat it as a trust hint */
    client->local = FALSE;
    report();
}

void
amber_os_channel_closed(CARD32 channel)
{
    ClientPtr client = client_for_channel(channel);
    if (!client)
        return;
    AMBER_COMM(client)->gone = TRUE;
    CloseDownClient(client);
}

void
amber_os_channel_readable(CARD32 channel)
{
    ClientPtr client = client_for_channel(channel);
    if (!client || client->clientGone)
        return;
    if (AMBER_COMM(client)->flags & AMBER_COMM_IGNORED)
        return;
    mark_client_ready(client);
}

void
CloseDownConnection(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    if (!oc)
        return;
    if (FlushCallback)
        CallCallbacks(&FlushCallback, client);
    if (oc->ocnt && !oc->gone)
        FlushClient(client, (OsCommPtr) oc, NULL, 0);
    unmap_channel(oc->channel);
    if (!oc->gone)
        amberwin_channel_close(oc->channel);
    free(oc->ibuf);
    free(oc->obuf);
    free(oc);
    client->osPrivate = NULL;
    if (auditTrailLevel > 1)
        AuditF("client %d disconnected\n", client->index);
    report();
}

/* ---- authorization glue -------------------------------------------------- */
const char *
ClientAuthorized(ClientPtr client, unsigned int proto_n, char *auth_proto,
                 unsigned int string_n, char *auth_string)
{
    AmberCommPtr oc = AMBER_COMM(client);
    const char *reason = NULL;
    XID auth_id;

    auth_id = CheckAuthorization(proto_n, auth_proto, string_n, auth_string,
                                 client, &reason);
    if (auth_id == (XID) ~0L) {
        /* upstream consults the host list here; AmberX has none, so a bad
         * cookie is the end of the conversation */
        return reason ? reason : "Client is not authorized to connect to Server";
    }
    oc->auth_id = auth_id;
    oc->conn_time = 0;
    if (auditTrailLevel > 1)
        AuditF("client %d connected\n", client->index);
    return NULL;
}

/* ---- host access: there is no list ------------------------------------- */
int
AddHost(ClientPtr client, int family, unsigned length, const void *pAddr)
{
    (void) client; (void) family; (void) length; (void) pAddr;
    return BadAccess;
}

int
RemoveHost(ClientPtr client, int family, unsigned length, void *pAddr)
{
    (void) client; (void) family; (void) length; (void) pAddr;
    return BadAccess;
}

int
GetHosts(void **data, int *pnHosts, int *pLen, BOOL *pEnabled)
{
    *data = NULL;
    *pnHosts = 0;
    *pLen = 0;
    *pEnabled = FALSE;
    return Success;
}

int
ChangeAccessControl(ClientPtr client, int fEnabled)
{
    (void) client;
    (void) fEnabled;
    return BadAccess;
}

Bool
ComputeLocalClient(ClientPtr client)
{
    (void) client;
    return FALSE;
}

Bool
ClientIsLocal(ClientPtr client)
{
    (void) client;
    return FALSE;
}

/* ---- identity of the peer: unknowable, so say so ----------------------- */
int
GetClientFd(ClientPtr client)
{
    (void) client;
    return -1;
}

pid_t
GetClientPid(ClientPtr client)
{
    (void) client;
    return -1;
}

const char *
GetClientCmdName(ClientPtr client)
{
    (void) client;
    return NULL;
}

const char *
GetClientCmdArgs(ClientPtr client)
{
    (void) client;
    return NULL;
}

int
GetLocalClientCreds(ClientPtr client, LocalClientCredRec **lccp)
{
    (void) client;
    *lccp = NULL;
    return -1;
}

void
FreeLocalClientCreds(LocalClientCredRec *lcc)
{
    (void) lcc;
}

void
ReserveClientIds(ClientPtr client)
{
    (void) client;
}

void
ReleaseClientIds(ClientPtr client)
{
    (void) client;
}

/* ---- attend / ignore, grab imperviousness ------------------------------- */
static Bool
has_whole_request(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    int gotnow;
    xReq *req;
    if (!oc || !oc->ibuf)
        return FALSE;
    gotnow = oc->icnt + (int) (oc->ibuf - oc->iptr) - oc->lenLastReq;
    if (gotnow < (int) sizeof(xReq))
        return FALSE;
    req = (xReq *) (oc->iptr + oc->lenLastReq);
    return gotnow >= (int) (get_req_len(req, client) << 2);
}

void
IgnoreClient(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    client->ignoreCount++;
    if (client->ignoreCount > 1)
        return;
    if (oc) {
        oc->flags |= AMBER_COMM_IGNORED;
        mark_client_not_ready(client);
    }
}

void
AttendClient(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    client->ignoreCount--;
    if (client->ignoreCount)
        return;
    if (oc) {
        oc->flags &= ~AMBER_COMM_IGNORED;
        if (has_whole_request(client))
            mark_client_ready(client);
        else {
            /* bytes may have arrived while ignored; let the next wait see */
            uint32_t ids[MAX_CHANNELS];
            int n = amberwin_readable_channels(ids, MAX_CHANNELS), i;
            for (i = 0; i < n; i++)
                if (ids[i] == oc->channel) {
                    mark_client_ready(client);
                    break;
                }
        }
    }
}

void
MakeClientGrabImpervious(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    if (oc)
        oc->flags |= AMBER_COMM_GRAB_IMPERVIOUS;
    if (ServerGrabCallback) {
        ServerGrabInfoRec grabinfo;
        grabinfo.client = client;
        grabinfo.grabstate = CLIENT_IMPERVIOUS;
        CallCallbacks(&ServerGrabCallback, &grabinfo);
    }
}

void
MakeClientGrabPervious(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    if (oc)
        oc->flags &= ~AMBER_COMM_GRAB_IMPERVIOUS;
    if (ServerGrabCallback) {
        ServerGrabInfoRec grabinfo;
        grabinfo.client = client;
        grabinfo.grabstate = CLIENT_PERVIOUS;
        CallCallbacks(&ServerGrabCallback, &grabinfo);
    }
}

/* ---- input: ReadRequestFromClient -------------------------------------- */
static Bool
ensure_input_buffer(AmberCommPtr oc)
{
    if (oc->ibuf)
        return TRUE;
    oc->ibuf = malloc(BUFSIZE);
    if (!oc->ibuf)
        return FALSE;
    oc->isize = BUFSIZE;
    oc->icnt = 0;
    oc->iptr = oc->ibuf;
    oc->lenLastReq = 0;
    oc->ignoreBytes = 0;
    return TRUE;
}

int
ReadRequestFromClient(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    unsigned int gotnow, needed;
    int result;
    xReq *request;
    Bool need_header;
    Bool move_header;

    if (!oc || oc->gone) {
        YieldControlDeath();
        return -1;
    }
    if (!ensure_input_buffer(oc)) {
        YieldControlDeath();
        return -1;
    }

    /* advance to start of next request */
    oc->iptr += oc->lenLastReq;

    need_header = FALSE;
    move_header = FALSE;
    gotnow = oc->icnt + (unsigned int) (oc->ibuf - oc->iptr);

    if (oc->ignoreBytes > 0) {
        if (oc->ignoreBytes > oc->isize)
            needed = oc->isize;
        else
            needed = oc->ignoreBytes;
    }
    else if (gotnow < sizeof(xReq)) {
        needed = sizeof(xReq);
        need_header = TRUE;
    }
    else {
        request = (xReq *) oc->iptr;
        needed = get_req_len(request, client);
        if (!needed && client->big_requests) {
            move_header = TRUE;
            if (gotnow < sizeof(xBigReq)) {
                needed = bytes_to_int32(sizeof(xBigReq));
                need_header = TRUE;
            }
            else
                needed = get_big_req_len(request, client);
        }
        client->req_len = needed;
        if (needed > MAXINT >> 2)
            return -(BadLength);
        needed <<= 2;
    }
    if (gotnow < needed) {
        oc->lenLastReq = 0;
        if (needed > (unsigned) (maxBigRequestSize << 2)) {
            /* too big to handle: skip it and let Dispatch answer BadLength */
            oc->ignoreBytes = needed - gotnow;
            oc->lenLastReq = gotnow;
            return needed;
        }
        if ((gotnow == 0) || ((oc->iptr - oc->ibuf + needed) > (unsigned) oc->isize)) {
            if ((gotnow > 0) && (oc->iptr != oc->ibuf))
                memmove(oc->ibuf, oc->iptr, gotnow);
            if (needed > (unsigned) oc->isize) {
                char *ibuf = realloc(oc->ibuf, needed);
                if (!ibuf) {
                    YieldControlDeath();
                    return -1;
                }
                oc->isize = needed;
                oc->ibuf = ibuf;
            }
            oc->iptr = oc->ibuf;
            oc->icnt = gotnow;
        }
        result = amberwin_channel_read(oc->channel, oc->ibuf + oc->icnt,
                                       (size_t) (oc->isize - oc->icnt));
        if (result == 0) {
            /* nothing pending yet: the wait loop will mark us ready again */
            mark_client_not_ready(client);
            YieldControlNoInput(client);
            return 0;
        }
        if (result < 0) {
            oc->gone = TRUE;
            YieldControlDeath();
            return -1;
        }
        bytes_in_total += (uint64_t) result;
        oc->icnt += result;
        gotnow += result;
        /* free up some space after huge requests */
        if ((oc->isize > BUFWATERMARK) &&
            (oc->icnt < BUFSIZE) && (needed < BUFSIZE)) {
            char *ibuf = realloc(oc->ibuf, BUFSIZE);
            if (ibuf) {
                oc->isize = BUFSIZE;
                oc->ibuf = ibuf;
                oc->iptr = ibuf + oc->icnt - gotnow;
            }
        }
        if (need_header && gotnow >= needed) {
            request = (xReq *) oc->iptr;
            needed = get_req_len(request, client);
            if (!needed && client->big_requests) {
                move_header = TRUE;
                if (gotnow < sizeof(xBigReq))
                    needed = bytes_to_int32(sizeof(xBigReq));
                else
                    needed = get_big_req_len(request, client);
            }
            client->req_len = needed;
            if (needed > MAXINT >> 2)
                return -(BadLength);
            needed <<= 2;
        }
        if (gotnow < needed) {
            YieldControlNoInput(client);
            return 0;
        }
    }
    if (needed == 0) {
        if (client->big_requests)
            needed = sizeof(xBigReq);
        else
            needed = sizeof(xReq);
    }

    if (oc->ignoreBytes > 0) {
        if (gotnow < needed) {
            oc->ignoreBytes -= gotnow;
            oc->iptr += gotnow;
            gotnow = 0;
        }
        else {
            oc->ignoreBytes -= needed;
            oc->iptr += needed;
            gotnow -= needed;
        }
        needed = 0;
    }

    oc->lenLastReq = needed;

    /* If only a partial request remains, yield so other clients get a turn
     * and the wait loop re-arms readiness when more bytes arrive. */
    gotnow -= needed;
    if (!gotnow && !oc->ignoreBytes) {
        /* buffer fully consumed: the next ready mark comes from the channel */
        mark_client_not_ready(client);
    }
    if (move_header) {
        if (client->req_len < bytes_to_int32(sizeof(xBigReq) - sizeof(xReq))) {
            YieldControlDeath();
            return -1;
        }
        request = (xReq *) oc->iptr;
        oc->iptr += (sizeof(xBigReq) - sizeof(xReq));
        *(xReq *) oc->iptr = *request;
        oc->lenLastReq -= (sizeof(xBigReq) - sizeof(xReq));
        client->req_len -= bytes_to_int32(sizeof(xBigReq) - sizeof(xReq));
    }
    client->requestBuffer = (void *) oc->iptr;
    return needed;
}

Bool
InsertFakeRequest(ClientPtr client, char *data, int count)
{
    AmberCommPtr oc = AMBER_COMM(client);
    int gotnow, moveup;

    if (!oc || !ensure_input_buffer(oc))
        return FALSE;
    oc->iptr += oc->lenLastReq;
    oc->lenLastReq = 0;
    gotnow = oc->icnt + (int) (oc->ibuf - oc->iptr);
    if ((gotnow + count) > oc->isize) {
        char *ibuf = realloc(oc->ibuf, gotnow + count);
        if (!ibuf)
            return FALSE;
        oc->isize = gotnow + count;
        oc->ibuf = ibuf;
        oc->iptr = ibuf + oc->icnt - gotnow;
    }
    moveup = count - (int) (oc->iptr - oc->ibuf);
    if (moveup > 0) {
        if (gotnow > 0)
            memmove(oc->iptr + moveup, oc->iptr, gotnow);
        oc->iptr += moveup;
        oc->icnt += moveup;
    }
    memmove(oc->iptr - count, data, count);
    oc->iptr -= count;
    gotnow += count;
    if ((gotnow >= (int) sizeof(xReq)) &&
        (gotnow >= (int) (get_req_len((xReq *) oc->iptr, client) << 2)))
        mark_client_ready(client);
    else
        YieldControlNoInput(client);
    return TRUE;
}

void
ResetCurrentRequest(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    xReq *request;
    int gotnow, needed;

    if (!oc || !oc->ibuf)
        return;
    oc->lenLastReq = 0;
    gotnow = oc->icnt + (int) (oc->ibuf - oc->iptr);
    if (gotnow < (int) sizeof(xReq)) {
        YieldControlNoInput(client);
    }
    else {
        request = (xReq *) oc->iptr;
        needed = get_req_len(request, client);
        if (!needed && client->big_requests) {
            oc->iptr -= sizeof(xBigReq) - sizeof(xReq);
            *(xReq *) oc->iptr = *request;
            ((xBigReq *) oc->iptr)->length = client->req_len;
            if (client->swapped) {
                swapl(&((xBigReq *) oc->iptr)->length);
            }
        }
        if (gotnow >= (needed << 2)) {
            if (!(oc->flags & AMBER_COMM_IGNORED))
                mark_client_ready(client);
            YieldControl();
        }
        else
            YieldControlNoInput(client);
    }
}

/* ---- output: WriteToClient / FlushClient -------------------------------- */
static Bool
ensure_output_buffer(AmberCommPtr oc)
{
    if (oc->obuf)
        return TRUE;
    oc->obuf = malloc(BUFSIZE);
    if (!oc->obuf)
        return FALSE;
    oc->osize = BUFSIZE;
    oc->ocnt = 0;
    return TRUE;
}

static void
AbortClient(ClientPtr client)
{
    AmberCommPtr oc = AMBER_COMM(client);
    if (oc && !oc->gone) {
        oc->gone = TRUE;
        amberwin_channel_close(oc->channel);
    }
}

int
FlushClient(ClientPtr who, OsCommPtr oc_, const void *__extraBuf, int extraCount)
{
    AmberCommPtr oc = (AmberCommPtr) oc_;
    const char *extraBuf = __extraBuf;
    static const char padBuffer[3] = { 0, 0, 0 };
    int padsize;
    long notWritten;

    if (!oc)
        return 0;
    padsize = padding_for_int32(extraCount);
    notWritten = (long) oc->ocnt + extraCount + padsize;
    if (!notWritten)
        return 0;
    if (FlushCallback)
        CallCallbacks(&FlushCallback, who);
    if (oc->gone) {
        oc->ocnt = 0;
        return -1;
    }
    /* the channel write is whole or nothing: no partial-write bookkeeping */
    if ((oc->ocnt && amberwin_channel_write(oc->channel, oc->obuf, (size_t) oc->ocnt) < 0) ||
        (extraCount && amberwin_channel_write(oc->channel, extraBuf, (size_t) extraCount) < 0) ||
        (padsize && amberwin_channel_write(oc->channel, padBuffer, (size_t) padsize) < 0)) {
        AbortClient(who);
        MarkClientException(who);
        oc->ocnt = 0;
        return -1;
    }
    oc->ocnt = 0;
    return extraCount;
}

int
WriteToClient(ClientPtr who, int count, const void *__buf)
{
    AmberCommPtr oc;
    int padBytes;
    const char *buf = __buf;

    if (!count || !who || who == serverClient || who->clientGone)
        return 0;
    oc = AMBER_COMM(who);
    if (!oc)
        return 0;
    if (!ensure_output_buffer(oc)) {
        AbortClient(who);
        MarkClientException(who);
        return -1;
    }

    padBytes = padding_for_int32(count);

    if (ReplyCallback) {
        ReplyInfoRec replyinfo;
        replyinfo.client = who;
        replyinfo.replyData = buf;
        replyinfo.dataLenBytes = count + padBytes;
        replyinfo.padBytes = padBytes;
        if (who->replyBytesRemaining) {
            who->replyBytesRemaining -= count + padBytes;
            replyinfo.startOfReply = FALSE;
            replyinfo.bytesRemaining = who->replyBytesRemaining;
            CallCallbacks((&ReplyCallback), (void *) &replyinfo);
        }
        else if (who->clientState == ClientStateRunning && buf[0] == X_Reply) {
            CARD32 replylen;
            unsigned long bytesleft;
            replylen = ((const xGenericReply *) buf)->length;
            if (who->swapped)
                swapl(&replylen);
            bytesleft = (replylen * 4) + SIZEOF(xReply) - count - padBytes;
            replyinfo.startOfReply = TRUE;
            replyinfo.bytesRemaining = who->replyBytesRemaining = bytesleft;
            CallCallbacks((&ReplyCallback), (void *) &replyinfo);
        }
    }
    if (oc->ocnt == 0 || oc->ocnt + count + padBytes > oc->osize) {
        output_pending_clear(who);
        if (!any_output_pending()) {
            CriticalOutputPending = FALSE;
            NewOutputPending = FALSE;
        }
        return FlushClient(who, (OsCommPtr) oc, buf, count);
    }

    NewOutputPending = TRUE;
    output_pending_mark(who);
    memmove((char *) oc->obuf + oc->ocnt, buf, count);
    oc->ocnt += count;
    if (padBytes) {
        memset(oc->obuf + oc->ocnt, '\0', padBytes);
        oc->ocnt += padBytes;
    }
    return count;
}

int
WriteFdToClient(ClientPtr client, int fd, Bool do_close)
{
    (void) client;
    (void) fd;
    (void) do_close;
    return -1;      /* no descriptor passing over a channel */
}

void
FlushAllOutput(void)
{
    ClientPtr client, tmp;

    if (!NewOutputPending)
        return;
    NewOutputPending = FALSE;
    CriticalOutputPending = FALSE;

    xorg_list_for_each_entry_safe(client, tmp, &output_pending_clients, output_pending) {
        if (client->clientGone)
            continue;
        if (!client_is_ready(client)) {
            output_pending_clear(client);
            (void) FlushClient(client, (OsCommPtr) AMBER_COMM(client), NULL, 0);
        }
        else
            NewOutputPending = TRUE;
    }
}

void
FlushIfCriticalOutputPending(void)
{
    if (CriticalOutputPending)
        FlushAllOutput();
}

void
SetCriticalOutputPending(void)
{
    CriticalOutputPending = TRUE;
}

void
ResetOsBuffers(void)
{
    /* buffers are per-client and freed with the client */
}

/* ---- fd notification: there are no fds --------------------------------- */
Bool
SetNotifyFd(int fd, NotifyFdProcPtr notify, int mask, void *data)
{
    (void) fd; (void) notify; (void) mask; (void) data;
    return FALSE;
}

/* ---- the wait loop ------------------------------------------------------- */
extern int amber_os_check_timers(void);

static void
drain_backend(void)
{
    amberwin_event ev;
    uint32_t ids[MAX_CHANNELS];
    int n, i;

    while (amberwin_next_event(&ev)) {
        switch (ev.type) {
        case AMBERWIN_EV_CHANNEL_OPEN:
            amber_os_channel_opened(ev.channel);
            break;
        case AMBERWIN_EV_CHANNEL_CLOSE:
            amber_os_channel_closed(ev.channel);
            break;
        case AMBERWIN_EV_COOKIE:
            amber_os_set_cookie(ev.cookie);
            cookie_seen = TRUE;
            report();
            break;
        case AMBERWIN_EV_SHUTDOWN:
            dispatchException |= DE_TERMINATE;
            isItTimeToYield = TRUE;
            break;
        case AMBERWIN_EV_FRAME_CLOSE:
        case AMBERWIN_EV_FRAME_CONFIGURE:
        case AMBERWIN_EV_FRAME_ACTIVATE:
        case AMBERWIN_EV_FRAME_STATE:
            amber_wm_event(&ev);
            break;
        default:
            amber_ddx_input_event(&ev);
            break;
        }
    }
    n = amberwin_readable_channels(ids, MAX_CHANNELS);
    for (i = 0; i < n; i++)
        amber_os_channel_readable(ids[i]);
}

Bool
WaitForSomething(Bool are_ready)
{
    int timeout;
    int woke;

    while (1) {
        if (workQueue)
            ProcessWorkQueue();
        timeout = amber_os_check_timers();
        are_ready = clients_are_ready();
        if (are_ready)
            timeout = 0;
        BlockHandler(&timeout);
        if (NewOutputPending)
            FlushAllOutput();
        if (dispatchException)
            return FALSE;
        woke = amberwin_wait(timeout);
        drain_backend();
        WakeupHandler(woke);
        if (dispatchException)
            return FALSE;
        if (InputCheckPending())
            return FALSE;
        if (clients_are_ready())
            return TRUE;
        /* a timeout with nothing ready: loop, timers may have fired */
    }
}
