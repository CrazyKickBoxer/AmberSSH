/* os_auth.c — authorization: MIT-MAGIC-COOKIE-1, and nothing else.
 * Original AmberSSH file; replaces xserver/os/auth.c.
 *
 * Upstream's auth.c reads an -auth file, knows several protocols, and — the
 * part that matters — lets a client with no valid cookie in anyway if the
 * host access list allows it. AmberX does none of that:
 *
 *   * the one cookie arrives over the control pipe (SetCookie) and is
 *     installed with amber_os_set_cookie(); there is no file;
 *   * there is no host list, so there is no "no cookie but allowed host";
 *   * a client whose cookie does not match is refused. Always.
 *
 * Restricted mode (the default) is where the cookie meets the SECURITY
 * extension: the installed cookie is registered as an UNTRUSTED
 * authorization with a timeout, exactly as `xauth generate untrusted`
 * would create one through the extension. From then on every client that
 * presents it is untrusted, and Xext/security.c's XACE hooks enforce what
 * that means — no access to trusted clients' resources, no keyboard grabs,
 * no writing trusted windows' properties, no reading the root's pixels.
 * Trusted mode registers nothing, and the cookie's clients are trusted.
 * The mode is fixed when the host starts; it cannot be promoted.
 *
 * The cookie table itself is upstream's os/mitauth.c, unmodified. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/extensions/secur.h>
#include "misc.h"
#include "os.h"
#include "osdep.h"
#include "dixstruct.h"
#include "resource.h"
#include "securitysrv.h"
#include "amberos.h"
#include "amberwin.h"

#define MIT_NAME "MIT-MAGIC-COOKIE-1"
#define MIT_NAME_LEN 18

static Bool cookie_installed;
static XID cookie_auth_id = None;

void
GenerateRandomData(int len, char *buf)
{
    if (len <= 0)
        return;
    if (!amberwin_random(buf, (size_t) len))
        FatalError("the system random source failed; refusing to generate a weak authorization");
}

/* ---- the one protocol --------------------------------------------------- */
static Bool
is_mit(unsigned short name_length, const char *name)
{
    return name_length == MIT_NAME_LEN && memcmp(name, MIT_NAME, MIT_NAME_LEN) == 0;
}

void
InitAuthorization(const char *file_name)
{
    /* no -auth file: the cookie comes from the controller */
    (void) file_name;
}

int
AddAuthorization(unsigned name_length, const char *name,
                 unsigned data_length, char *data)
{
    if (!is_mit((unsigned short) name_length, name))
        return 0;
    return MitAddCookie((unsigned short) data_length, data, FakeClientID(0));
}

/* The timer the extension itself would have armed on a freshly generated
 * authorization: while no client uses the cookie, it expires after the
 * timeout and new clients are refused. Once a client connects, security.c
 * cancels the timer (through pAuth->timer) and re-arms it with its own
 * callback when the last client leaves. */
static CARD32
untrusted_cookie_expired(OsTimerPtr timer, CARD32 now, void *arg)
{
    SecurityAuthorizationPtr pAuth = arg;
    (void) timer; (void) now;
    if (pAuth->refcnt == 0) {
        LogMessage(X_INFO, "AmberX: the session's untrusted authorization expired; new clients are refused\n");
        FreeResource(pAuth->id, RT_NONE);   /* SecurityDeleteAuthorization removes the cookie */
        cookie_installed = FALSE;
        cookie_auth_id = None;
    }
    return 0;
}

static void
register_untrusted(XID authId)
{
    const amberwin_config *cfg = amberwin_get_config();
    SecurityAuthorizationPtr pAuth;
    CARD32 maxSecs = (CARD32) (~0) / 1000;
    unsigned int seconds = cfg->auth_timeout_seconds > 0 ? (unsigned) cfg->auth_timeout_seconds : 0;

    if (!SecurityAuthorizationResType) {
        /* the extension is not initialised: refuse to run "restricted" as a
         * label without the mechanism behind it */
        FatalError("AmberX: restricted mode requested but the SECURITY extension is not available");
    }
    pAuth = calloc(1, sizeof(SecurityAuthorizationRec));
    if (!pAuth)
        FatalError("AmberX: out of memory registering the authorization");
    pAuth->id = authId;
    pAuth->timeout = seconds;
    pAuth->group = None;
    pAuth->trustLevel = XSecurityClientUntrusted;
    pAuth->refcnt = 0;
    pAuth->secondsRemaining = 0;
    pAuth->timer = NULL;
    pAuth->eventClients = NULL;
    if (!AddResource(authId, SecurityAuthorizationResType, pAuth))
        FatalError("AmberX: could not register the authorization with the SECURITY extension");
    if (seconds > 0) {
        if (seconds > maxSecs)
            seconds = maxSecs;
        pAuth->timer = TimerSet(NULL, 0, seconds * 1000, untrusted_cookie_expired, pAuth);
    }
    LogMessage(X_INFO, "AmberX: session authorization registered UNTRUSTED (timeout %u s)\n", seconds);
}

/* Installs the session cookie, replacing any earlier one. The value is
 * copied into mitauth's table and nowhere else; it is never logged. */
void
amber_os_set_cookie(const unsigned char *cookie16)
{
    const amberwin_config *cfg = amberwin_get_config();
    XID id;

    if (cookie_auth_id != None) {
        FreeResource(cookie_auth_id, RT_NONE);   /* a registered untrusted auth, if any */
        cookie_auth_id = None;
    }
    MitResetCookie();
    cookie_installed = FALSE;
    id = FakeClientID(0);
    if (!MitAddCookie(16, (const char *) cookie16, id))
        return;
    cookie_installed = TRUE;
    if (!cfg->trusted) {
        register_untrusted(id);
        cookie_auth_id = id;
    }
    else
        LogMessage(X_WARNING, "AmberX: session authorization registered TRUSTED: clients have full access\n");
}

XID
CheckAuthorization(unsigned int name_length, const char *name,
                   unsigned int data_length, const char *data,
                   ClientPtr client, const char **reason)
{
    if (!cookie_installed) {
        *reason = "No authorization is installed for this display";
        return (XID) ~0L;
    }
    if (!is_mit((unsigned short) name_length, name)) {
        *reason = "Authorization protocol not supported";
        return (XID) ~0L;
    }
    return MitCheckCookie((unsigned short) data_length, data, client, reason);
}

void
ResetAuthorization(void)
{
    MitResetCookie();
    cookie_installed = FALSE;
    cookie_auth_id = None;
}

int
AuthorizationFromID(XID id, unsigned short *name_lenp, const char **namep,
                    unsigned short *data_lenp, char **datap)
{
    if (MitFromID(id, data_lenp, datap)) {
        *name_lenp = MIT_NAME_LEN;
        *namep = MIT_NAME;
        return 1;
    }
    return 0;
}

int
RemoveAuthorization(unsigned short name_length, const char *name,
                    unsigned short data_length, const char *data)
{
    int r;
    if (!is_mit(name_length, name))
        return 0;
    r = MitRemoveCookie(data_length, data);
    if (r) {
        /* the extension's timeout removed the session cookie */
        cookie_installed = FALSE;
        cookie_auth_id = None;
    }
    return r;
}

XID
GenerateAuthorization(unsigned name_length, const char *name,
                      unsigned data_length, const char *data,
                      unsigned *data_length_return, char **data_return)
{
    if (!is_mit((unsigned short) name_length, name))
        return (XID) ~0L;
    return MitGenerateCookie(data_length, data, FakeClientID(0),
                             data_length_return, data_return);
}

XID
AuthorizationIDOfClient(ClientPtr client)
{
    if (client->osPrivate)
        return AMBER_COMM(client)->auth_id;
    return None;
}

void
CheckUserAuthorization(void)
{
    /* upstream: refuse to run setuid for a user who cannot read the auth
     * file. There is no file and no setuid. */
}

/* SECURITY extension hooks for enabling/disabling local (host-list) access
 * around a generated authorization. There is no host list to toggle. */
void
EnableLocalAccess(void)
{
}

void
DisableLocalAccess(void)
{
}
