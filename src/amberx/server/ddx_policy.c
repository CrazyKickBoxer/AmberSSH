/* ddx_policy.c — which extensions a restricted client may use. Original
 * AmberSSH file.
 *
 * Upstream's SECURITY extension allows an untrusted client exactly two
 * extensions, XC-MISC and BIG-REQUESTS (Xext/security.c,
 * SecurityTrustedExtensions). That is a defensible choice for the world
 * SECURITY was written for — a single untrusted program on a trusted
 * desktop — and an impossible one here: with it, restricted mode cannot run
 * a toolkit application at all, because RENDER, XKEYBOARD and SHAPE are all
 * denied. A default mode that nothing runs in is a default mode nobody uses,
 * and the mode people actually use would then be the trusted one. So AmberX
 * widens the allowlist, deliberately and by name.
 *
 * It is still an allowlist. An extension is on it only if using it cannot,
 * by itself, give a forwarded client access to the desktop, to AmberSSH, or
 * to server state beyond what core X already gives it. What that leaves out
 * is written below, next to what it lets in, because a security decision
 * with no record of what it excluded is not a decision.
 *
 * Ordering, which is the whole trick: dix *prepends* callbacks and walks
 * the list from the head, so the callback registered first is the callback
 * called last, and only the one called last can change the answer. This
 * hook is therefore registered during screen init — before InitExtensions
 * registers the SECURITY extension's — so that it runs after it. Registered
 * afterwards, as looked obvious first, it was overwritten every time and
 * restricted clients still saw two extensions. */
#include <dix-config.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "extnsionst.h"
#include "xace.h"
#include "xacestr.h"
#include "amberos.h"
#include "amberwin.h"

/* Allowed to a restricted client, and why:
 *
 *   BIG-REQUESTS   upstream's own; a request longer than 256 KiB, bounded
 *                  by amberLimits.request_max_bytes.
 *   XC-MISC        upstream's own; resource id ranges.
 *   RENDER         anti-aliased drawing into the client's own drawables.
 *                  Every toolkit written since 2001 needs it.
 *   SHAPE          the shape of the client's own windows.
 *   SYNC           counters and alarms the client owns; the protocol a
 *                  window manager uses to know a resize finished.
 *   XKEYBOARD      the keyboard map. Without it a client cannot tell what
 *                  a key means; toolkits abort.
 *   XInputExtension  version 2 input for the client's own windows. Raw
 *                  events can only be selected on the root, and an AmberX
 *                  root only ever carries input the user aimed at a
 *                  forwarded window — the server never sees the rest of the
 *                  desktop's input — so this is not a keylogger for the
 *                  machine. It does let one forwarded client see another's
 *                  input, which untrusted clients can do anyway (see
 *                  THREAT-MODEL.md, "one forwarded client attacking
 *                  another"); it is the same boundary, not a new one.
 *   Generic Event Extension  the envelope XI2 events arrive in; no
 *                  requests of its own.
 *   RANDR          the monitor layout, read-only: AmberX refuses every
 *                  mode-setting request from any client (ddx_randr.c), and
 *                  the shape of the user's own desktop is not a secret from
 *                  a program the user is running on it.
 *   XFIXES         regions, cursor visibility and selection notifications.
 *                  Selections are not restricted by trust level in this
 *                  server at all (a documented gap), so the notification is
 *                  no new access; regions are the client's own.
 *
 * Deliberately NOT allowed, though compiled:
 *
 *   XTEST          synthesises input as if the user had typed it. That is
 *                  the input-injection threat exactly.
 *   SECURITY       an untrusted client must not create or revoke
 *                  authorizations, least of all its own.
 *   DAMAGE         reports what changed in a drawable — including one it
 *                  does not own. Screen scraping with a subscription.
 *   MIT-SHM        not advertised at all, by policy (os_misc.c).
 *   Composite      not advertised either, and not only by policy: it would
 *                  redirect other clients' windows into pixmaps it can read
 *                  (screen scraping with a rendering pipeline), and under
 *                  miext/rootless its implicit redirection of ARGB windows
 *                  corrupts the frame pixmap's origin — see os_misc.c.
 *
 * A name not in either list is denied, because the list is an allowlist. */
static const char *const restricted_extensions[] = {
    "BIG-REQUESTS",
    "XC-MISC",
    "RENDER",
    "SHAPE",
    "SYNC",
    "XKEYBOARD",
    "XInputExtension",
    "Generic Event Extension",
    "RANDR",
    "XFIXES",
    NULL
};

static Bool
on_allowlist(const char *name)
{
    int i;
    if (!name)
        return FALSE;
    for (i = 0; restricted_extensions[i]; i++)
        if (strcmp(restricted_extensions[i], name) == 0)
            return TRUE;
    return FALSE;
}

static void
ext_access_hook(CallbackListPtr *pcbl, void *data, void *calldata)
{
    XaceExtAccessRec *rec = calldata;
    (void) pcbl; (void) data;

    /* Only ever widens: a request already allowed is left alone, and a
     * denial for an extension not on the list stays a denial. */
    if (rec->status == Success)
        return;
    if (rec->ext && on_allowlist(rec->ext->name))
        rec->status = Success;
}

Bool
amber_policy_init(void)
{
    const amberwin_config *cfg = amberwin_get_config();

    /* Trusted mode registers no authorization, so SECURITY never judges a
     * client and there is nothing to widen. */
    if (cfg->trusted)
        return TRUE;
    /* Two hooks, because the SECURITY extension uses two and they are not
     * the same question: XACE_EXT_ACCESS decides whether a client may see
     * the extension in QueryExtension and ListExtensions, and
     * XACE_EXT_DISPATCH decides whether a request to it may run — a denial
     * there is turned into BadRequest, "pretend the extension does not
     * exist". Widening only the first gives a client an extension it can
     * see and cannot use. */
    if (!XaceRegisterCallback(XACE_EXT_ACCESS, ext_access_hook, NULL))
        return FALSE;
    if (!XaceRegisterCallback(XACE_EXT_DISPATCH, ext_access_hook, NULL))
        return FALSE;
    LogMessage(X_INFO, "AmberX: restricted clients may use %d extensions\n",
               (int) (sizeof restricted_extensions / sizeof restricted_extensions[0]) - 1);
    return TRUE;
}
