/* amberlimits.h — AmberX resource limits, in one place (named so as never to
 * shadow the C library's <limits.h> on the include path). Original AmberSSH file.
 *
 * Every limit a forwarded client can run into is declared here with its
 * default, so the question "what stops a client from doing X to excess" has
 * one answer. Enforcement lives in ddx_limits.c (request-vector wrappers for
 * ChangeProperty and InternAtom, the XACE resource hook, the pixmap
 * wrapper), os_connection.c (clients, request size) and
 * the Windows side (queue bytes, CPU, memory, via the Job Object).
 *
 * A limit violation rejects the request (BadAlloc / BadAccess) or closes
 * the offending client. It never ends the server, and it can never reach
 * AmberSSH, which is another process. */
#ifndef AMBERX_LIMITS_H
#define AMBERX_LIMITS_H

typedef struct amber_limits {
    /* clients per session: LimitClients in os_connection.c (64 + server) */
    int windows_per_client;         /* RT_WINDOW resources one client may hold */
    int windows_per_session;        /* across all clients */
    int pixmap_max_dim;             /* width or height of any pixmap */
    long long pixmap_max_bytes;     /* one pixmap's storage */
    long long pixmap_total_bytes;   /* all live pixmaps, all clients */
    long long property_max_bytes;   /* one property's value */
    int icon_max_dim;               /* _NET_WM_ICON side (ddx_wm.c) */
    long long request_max_bytes;    /* including BIG-REQUESTS */
    int atoms_per_window;           /* InternAtom (creating) per client per window */
    int atoms_window_ms;            /* the window for the atom rate */
} amber_limits;

/* the defaults; the Windows side may lower them from the command line */
extern amber_limits amberLimits;

/* installs the hooks and the pixmap wrapper on the screen */
struct _Screen;
int amber_limits_screen_init(struct _Screen *pScreen);
/* called from OsInit: the request-size limit is a dix global */
void amber_limits_os_init(void);

#endif
