/* amberos.h — private to AmberX's os and DDX layers (the X side). Original
 * AmberSSH file.
 *
 * What upstream keeps in os/osdep.h as OsCommRec, with the socket removed
 * and a channel id in its place. One of these hangs off every
 * ClientRec.osPrivate. Plus the handful of calls the DDX files make to each
 * other. */
#ifndef AMBEROS_H
#define AMBEROS_H

#include <dix-config.h>
#include <X11/X.h>
#include <X11/Xmd.h>
#include "misc.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "windowstr.h"

typedef struct amber_comm {
    CARD32 channel;         /* AmberXControl channel id; never 0 */
    /* input: whole requests are presented from this buffer */
    char *ibuf;
    int isize;              /* allocated */
    int icnt;               /* bytes valid from ibuf */
    char *iptr;             /* start of the current request */
    int lenLastReq;         /* bytes of the request last returned */
    int ignoreBytes;        /* remainder of an oversized request to skip */
    /* output: replies and events coalesce here until a flush */
    unsigned char *obuf;
    int osize;
    int ocnt;
    XID auth_id;
    CARD32 conn_time;       /* non-zero until the setup request is accepted */
    int flags;
    Bool gone;              /* the channel closed under us */
} AmberComm, *AmberCommPtr;

#define AMBER_COMM_GRAB_IMPERVIOUS 1
#define AMBER_COMM_IGNORED         2

#define AMBER_COMM(client) ((AmberCommPtr) (client)->osPrivate)

/* os_connection.c */
void amber_os_channel_opened(CARD32 channel);
void amber_os_channel_closed(CARD32 channel);
void amber_os_channel_readable(CARD32 channel);

/* os_auth.c */
void amber_os_set_cookie(const unsigned char *cookie16);

/* ddx_input.c: called from WaitForSomething when the backend has input */
struct amberwin_event;
void amber_ddx_input_event(const struct amberwin_event *ev);

/* ddx_frames.c: the rootless frame procs, installed by ddx_screen.c */
struct _RootlessFrameProcs;
struct _RootlessFrameProcs *amber_frame_procs(void);
struct amberwin_frame;
/* the frame a top-level window currently has, or NULL */
struct amberwin_frame *amber_frame_of(WindowPtr pWin);

/* ddx_wm.c: the window manager inside the server */
Bool amber_wm_screen_init(ScreenPtr pScreen);
void amber_wm_frame_created(WindowPtr pWin, struct amberwin_frame *f);
void amber_wm_frame_destroyed(WindowPtr pWin, struct amberwin_frame *f);
void amber_wm_frame_switched(WindowPtr pNew, WindowPtr pOld, struct amberwin_frame *f);
void amber_wm_event(const struct amberwin_event *ev);

/* ---- ddx_clipboard.c: the X half of the clipboard bridge -------------- */
Bool amber_clipboard_screen_init(ScreenPtr pScreen);
void amber_clipboard_from_windows(void);
void amber_clipboard_close(void);

/* ---- ddx_policy.c: which extensions a restricted client may use ------ */
Bool amber_policy_init(void);

/* ---- ddx_randr.c: the Windows monitors as RANDR outputs ---------------- */
Bool amber_randr_screen_init(ScreenPtr pScreen);
void amber_randr_monitors_changed(void);

#endif
