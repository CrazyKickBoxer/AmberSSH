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
 * The cookie table itself is upstream's os/mitauth.c, unmodified. */
#include <dix-config.h>
#include <string.h>
#include <X11/X.h>
#include "misc.h"
#include "os.h"
#include "osdep.h"
#include "dixstruct.h"
#include "amberos.h"
#include "amberwin.h"

#define MIT_NAME "MIT-MAGIC-COOKIE-1"
#define MIT_NAME_LEN 18

static Bool cookie_installed;

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

/* Installs the session cookie, replacing any earlier one. The value is
 * copied into mitauth's table and nowhere else; it is never logged. */
void
amber_os_set_cookie(const unsigned char *cookie16)
{
    MitResetCookie();
    if (MitAddCookie(16, (const char *) cookie16, FakeClientID(0)))
        cookie_installed = TRUE;
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
    if (!is_mit(name_length, name))
        return 0;
    return MitRemoveCookie(data_length, data);
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
