/* amberwin.h — the one header shared between the two halves of AmberXHost.
 *
 * One half is the X.Org core plus AmberX's os and DDX layers: C, compiled
 * against X headers, never sees <windows.h>. The other half is the Windows
 * backend: C++, compiled against <windows.h>, never sees an X header. The
 * two cannot share a translation unit (X's BOOL and Windows' BOOL collide,
 * and that is only the first collision), so everything they say to each
 * other goes through this file, which uses fixed-width C types and nothing
 * else.
 *
 * Direction of every call is fixed and written on it. The X side calls the
 * Windows side for time, randomness, logging, frames, waiting, and channel
 * bytes. The Windows side never calls the X side except to start it.
 *
 * Original AmberSSH file.
 */
#ifndef AMBERWIN_H
#define AMBERWIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- configuration handed to the X side before it starts -------------- */
typedef struct amberwin_config {
    int rootless;           /* 1: every top-level X window is a native window
                               (Phase 3); 0: one native window is the root */
    int width;              /* root size. Rootless: the virtual desktop */
    int height;
    int desktop_x;          /* rootless: where X (0,0) sits on the Windows
                               desktop, which may be negative with several
                               monitors. Root coords = desktop - origin. */
    int desktop_y;
    int depth;              /* 24; framebuffers are 32 bpp BGRX */
    uint32_t display;       /* display number, reported in status only */
    const char *keymap_path;/* an .xkm compiled by xkbcomp, or NULL for the
                               built-in US map (see ddx_keymap.c) */
    const char *identity;   /* the session badge text, e.g. "host · user" */
    const char *mode;       /* "RESTRICTED" or "TRUSTED" */
    int skin;               /* AmberSSH chrome style index for the badge */
} amberwin_config;
const amberwin_config *amberwin_get_config(void);

/* ---- services (X side → Windows side) --------------------------------- */
uint32_t amberwin_now_ms(void);
uint64_t amberwin_now_us(void);

/* OS CSPRNG. Returns 1 on success. A 0 must be treated as "stop", never as
 * "use what is in the buffer". */
int amberwin_random(void *buf, size_t len);

/* The bounded, non-sensitive log sink. `text` is a complete line, already
 * formatted, at most AMBERWIN_LOG_MAX bytes. Nothing that has passed
 * through this function may be a cookie, a nonce, a request body, a window's
 * contents or a property value: the X side formats messages, so the X side
 * is where that rule is kept. */
enum { AMBERWIN_LOG_INFO = 0, AMBERWIN_LOG_WARN = 1,
       AMBERWIN_LOG_ERROR = 2, AMBERWIN_LOG_FATAL = 3 };
#define AMBERWIN_LOG_MAX 512
void amberwin_log(int level, const char *text);

/* Ends the process. Does not return. */
void amberwin_exit(int code);

/* ---- the rootful screen ----------------------------------------------- */
/* Allocates the framebuffer the X side draws into: 32 bpp, BGRX, top-down,
 * `*stride_bytes` per row. The Windows side owns the memory and the window
 * that shows it. Returns 1 on success. Rootful mode only. */
int  amberwin_screen_create(int width, int height, void **bits, int *stride_bytes);
/* The X side has finished drawing this rectangle; show it. */
void amberwin_screen_present(int x, int y, int w, int h);
void amberwin_bell(int percent);

/* ---- rootless frames: one native window per top-level X window --------- */
/* A frame is an owner window carrying the AmberX identity strip and a view
 * child that shows the frame's pixels. The X side draws straight into the
 * frame's buffer (amberwin_frame_bits) and says what changed
 * (amberwin_frame_present). Every call here may come from the server
 * thread; the Windows side marshals to its UI thread as it must.
 * Coordinates are X screen coordinates (see desktop_x/desktop_y). */
typedef struct amberwin_frame amberwin_frame;

amberwin_frame *amberwin_frame_create(uint32_t xid, int x, int y, int w, int h,
                                      int override_redirect);
void  amberwin_frame_destroy(amberwin_frame *f);
void  amberwin_frame_move(amberwin_frame *f, int x, int y);
void  amberwin_frame_resize(amberwin_frame *f, int x, int y, int w, int h);
/* Places the frame directly beneath `above` (NULL = top of the stack) and
 * shows it if it was hidden. This is how a frame becomes visible. */
void  amberwin_frame_restack(amberwin_frame *f, amberwin_frame *above);
void  amberwin_frame_unmap(amberwin_frame *f);
/* nboxes 0 = unshaped; boxes are frame-local x1,y1,x2,y2 quads */
void  amberwin_frame_set_shape(amberwin_frame *f, int nboxes, const int16_t *boxes);
void *amberwin_frame_bits(amberwin_frame *f, int *stride_bytes);
/* frame-local rectangle; w < 0 means the whole frame */
void  amberwin_frame_present(amberwin_frame *f, int x, int y, int w, int h);
/* the remote-supplied title, already sanitised and bounded by the X side */
void  amberwin_frame_set_title(amberwin_frame *f, const char *utf8);
/* ARGB rows, top-down; already validated (bounded) by the X side; w 0 clears */
void  amberwin_frame_set_icon(amberwin_frame *f, int w, int h, const uint32_t *argb);
/* 0 for "no constraint"; resizable 0 removes the sizing border */
void  amberwin_frame_set_hints(amberwin_frame *f, int min_w, int min_h,
                               int max_w, int max_h, int resizable);
void  amberwin_frame_set_transient(amberwin_frame *f, amberwin_frame *owner, int modal);
void  amberwin_frame_set_state(amberwin_frame *f, int minimized, int maximized,
                               int fullscreen, int urgent);
void  amberwin_frame_activate(amberwin_frame *f);
/* the frame's X window changed (reparenting) */
void  amberwin_frame_set_xid(amberwin_frame *f, uint32_t xid);

/* ---- events (Windows side → X side, pulled by the X side) ------------- */
enum {
    AMBERWIN_EV_NONE = 0,
    AMBERWIN_EV_CHANNEL_OPEN,   /* channel */
    AMBERWIN_EV_CHANNEL_CLOSE,  /* channel */
    AMBERWIN_EV_COOKIE,         /* cookie[16], display */
    AMBERWIN_EV_SHUTDOWN,
    AMBERWIN_EV_POINTER_MOVE,   /* x, y (X screen coordinates) */
    AMBERWIN_EV_BUTTON,         /* button 1..7, pressed */
    AMBERWIN_EV_KEY,            /* keycode (evdev), pressed */
    AMBERWIN_EV_FOCUS,          /* rootful: pressed = 1 gained, 0 lost */
    /* rootless frame events, all carrying xid */
    AMBERWIN_EV_FRAME_CLOSE,    /* the user closed the native window */
    AMBERWIN_EV_FRAME_CONFIGURE,/* the user moved/resized it: x,y,w,h of the view */
    AMBERWIN_EV_FRAME_ACTIVATE, /* pressed = 1 activated, 0 deactivated */
    AMBERWIN_EV_FRAME_STATE,    /* x = minimized, y = maximized */
};
typedef struct amberwin_event {
    int type;
    uint32_t channel;
    uint32_t xid;
    int32_t x, y, w, h;
    int32_t button;
    int32_t pressed;
    uint32_t keycode;
    uint8_t cookie[16];
    uint32_t display;
} amberwin_event;

/* Blocks until an event or channel data arrives, or `timeout_ms` passes
 * (-1 = no timeout). Returns 1 if woken by activity, 0 on timeout. */
int  amberwin_wait(int timeout_ms);
/* Dequeues one event. Returns 1 if `ev` was filled. */
int  amberwin_next_event(amberwin_event *ev);

/* ---- channels: one per forwarded X client ----------------------------- */
/* Channel ids with unread bytes; returns how many ids were written. */
int  amberwin_readable_channels(uint32_t *ids, int cap);
/* Non-blocking. >0 bytes copied, 0 nothing pending, -1 the channel is gone. */
int  amberwin_channel_read(uint32_t ch, void *buf, size_t cap);
/* Blocking, whole. 0 on success, -1 if the channel is gone. */
int  amberwin_channel_write(uint32_t ch, const void *buf, size_t len);
/* The X side is done with this client (it disconnected or was killed). */
void amberwin_channel_close(uint32_t ch);

/* Host → controller status. Counts only, by construction. */
void amberwin_report(uint32_t open_clients, uint64_t bytes_in, int cookie_set);

/* ---- entry (Windows side → X side, once, on the server thread) -------- */
int  amberx_server_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
