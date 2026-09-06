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
    const char *mode;       /* the strip's mode word, as given */
    int trusted;            /* 0: the session cookie is registered UNTRUSTED with
                               the SECURITY extension (Phase 5); 1: trusted */
    int auth_timeout_seconds; /* untrusted authorization timeout; 0 = none */
    int skin;               /* AmberSSH chrome style index for the badge */
    int clipboard_mode;     /* one of the AMBERWIN_CLIP_* values below */
} amberwin_config;

/* Clipboard policy. The authority is AmberSSH — it is the process that can
 * reach the Windows clipboard at all — but the mode is passed down and the
 * X side enforces its own copy, so a host that has been subverted still
 * cannot move text in a direction the user did not enable. There is no
 * "advanced/full integration" value: images, file lists and markup each
 * need their own threat analysis and their own bounded parser, and until
 * they have one the bridge is text. */
enum {
    AMBERWIN_CLIP_OFF = 0,       /* the default: no bridge at all */
    AMBERWIN_CLIP_ASK = 1,       /* every transfer is confirmed by the user */
    AMBERWIN_CLIP_TO_LOCAL = 2,  /* remote → local text only */
    AMBERWIN_CLIP_TO_REMOTE = 3, /* local → remote text only */
    AMBERWIN_CLIP_BOTH = 4       /* both directions, no confirmation */
};
const amberwin_config *amberwin_get_config(void);

/* ---- the keyboard layout (X side pulls, once and on change) ------------
 *
 * The Windows side reads the live keyboard layout and reports what each key
 * produces as Unicode code points; the X side turns those into keysyms and
 * builds the XKB map (ddx_keymap.c). The split is deliberate: reading the
 * layout needs Win32, and keysym values are an X constant, so neither half
 * has to know the other's headers.
 *
 * Four levels, in XKB order: plain, Shift, AltGr, Shift+AltGr. A level with
 * ucs 0 does not exist on that key. `dead` marks a level that composes with
 * the next keystroke rather than producing its character; the X side maps
 * the spacing character to the matching dead keysym.
 *
 * Only keys that produce characters are reported. Everything structural —
 * function keys, navigation, modifiers, the keypad — comes from the X
 * side's own table, which is what makes a partial or unusual layout safe:
 * the worst case is a key with no symbol, never a keyboard with no Escape. */
enum { AMBERWIN_KEY_LEVELS = 4 };
typedef struct amberwin_key {
    uint16_t evdev;
    uint32_t ucs[AMBERWIN_KEY_LEVELS];
    uint8_t dead[AMBERWIN_KEY_LEVELS];
} amberwin_key;

typedef struct amberwin_keymap {
    const amberwin_key *keys;
    int nkeys;
    int has_level3;         /* the layout puts anything on AltGr */
    int repeat_delay_ms;    /* Windows' repeat delay and rate, for XKB's */
    int repeat_interval_ms;
    char name[96];          /* the layout's own name, for the log only */
} amberwin_keymap;
/* NULL when the Windows side could not read a layout: the X side then uses
 * its built-in US map unchanged. */
const amberwin_keymap *amberwin_get_keymap(void);

/* ---- monitors (X side pulls, once and on AMBERWIN_EV_MONITORS) --------
 * Rectangles are in X screen coordinates (desktop minus the origin), so a
 * monitor left of the primary one has a non-negative x here even though its
 * Windows coordinate is negative. mm_w/mm_h are derived from the monitor's
 * own DPI, which is how a client learns that two monitors scale
 * differently. */
typedef struct amberwin_monitor {
    int32_t x, y, w, h;
    int32_t mm_w, mm_h;
    int32_t dpi;
    int32_t primary;
    char name[32];
} amberwin_monitor;
/* Writes at most `cap` monitors, returns how many exist (which may exceed
 * `cap`). Always at least 1. */
int amberwin_monitors(amberwin_monitor *out, int cap);

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
/* diagnostics only: the frame and its buffer as the Windows side has them */
void  amberwin_frame_geometry(amberwin_frame *f, int *x, int *y, int *w, int *h,
                              int *buf_w, int *buf_h);
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
    AMBERWIN_EV_LOCKS,          /* x = caps, y = num, w = scroll (Windows') */
    AMBERWIN_EV_KEYMAP,         /* the Windows layout changed; re-read it */
    AMBERWIN_EV_MONITORS,       /* the monitor topology changed */
    AMBERWIN_EV_CLIPBOARD,      /* AmberSSH sent text; pull it */
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

/* The numbers behind the Remote Apps shelf and the diagnostics overlay
 * (Phase 7). Pushed by the X side because only it can count X things; the
 * Windows side adds what only it can count (frames, presents, queues) and
 * sends the two together. Counts only: no titles, no contents, no bytes. */
void amberwin_report_counts(uint32_t clients, uint32_t windows,
                            uint64_t pixmap_bytes, uint64_t x11_in,
                            uint64_t x11_out);

/* ---- clipboard: UTF-8 text, both directions ---------------------------
 * Neither side keeps a copy longer than the transfer needs, and neither
 * logs one. The X side pulls after AMBERWIN_EV_CLIPBOARD rather than being
 * handed a pointer, so there is no question of who frees what.
 *
 * pull: copies the pending text into `buf`, at most `cap` bytes, and
 *       returns how many were copied; 0 when there is nothing pending or it
 *       does not fit. The pending text is consumed either way.
 *
 *       `buf` MUST have room for cap + 1 bytes. The copy is bounded by cap
 *       but a NUL terminator is written after it, so a buffer of exactly cap
 *       bytes is overflowed by one when the text is exactly cap long. The
 *       only caller allocates cap + 1 already; this says so, because nothing
 *       did and the next caller would have had no way to know.
 * push: hands an X client's selection text to the Windows side, which
 *       applies the session's clipboard policy to it. */
uint32_t amberwin_clipboard_pull(char *buf, uint32_t cap);
void amberwin_clipboard_push(const char *utf8, uint32_t len);

/* ---- entry (Windows side → X side, once, on the server thread) -------- */
int  amberx_server_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
