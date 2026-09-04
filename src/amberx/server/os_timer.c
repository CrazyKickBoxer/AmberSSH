/* os_timer.c — timers, the clock, client sleeps and the screen-saver timer.
 *
 * The timer list is the one in xserver/os/WaitFor.c (MIT; X Consortium and
 * Keith Packard), carried over with the select() half of that file removed:
 * the list itself has no platform in it. Everything that touches the OS goes
 * through amberwin.h. Original AmberSSH file with that derivation. */
#include <dix-config.h>
#include <stdlib.h>
#include <X11/X.h>
#include "misc.h"
#include "os.h"
#include "dixstruct.h"
#include "opaque.h"
#include "list.h"
#include "input.h"
#include "inputstr.h"
#include "windowstr.h"
#include "scrnintstr.h"
#include "amberwin.h"

/* ---- the clock ---------------------------------------------------------- */
CARD32
GetTimeInMillis(void)
{
    return amberwin_now_ms();
}

CARD64
GetTimeInMicros(void)
{
    return amberwin_now_us();
}

/* ---- timers -------------------------------------------------------------- */
struct _OsTimerRec {
    struct xorg_list list;
    CARD32 expires;
    CARD32 delta;
    OsTimerCallback callback;
    void *arg;
};

static struct xorg_list timers;
static Bool timers_ready;

static OsTimerPtr
first_timer(void)
{
    if (timers.next == &timers)
        return NULL;
    return xorg_list_first_entry(&timers, struct _OsTimerRec, list);
}

static Bool
timer_pending(OsTimerPtr timer)
{
    return !xorg_list_is_empty(&timer->list);
}

static void
DoTimer(OsTimerPtr timer, CARD32 now)
{
    CARD32 newTime;
    xorg_list_del(&timer->list);
    newTime = (*timer->callback) (timer, now, timer->arg);
    if (newTime)
        TimerSet(timer, 0, newTime, timer->callback, timer->arg);
}

static void
DoTimers(CARD32 now)
{
    OsTimerPtr timer;
    input_lock();
    while ((timer = first_timer())) {
        if ((int) (timer->expires - now) > 0)
            break;
        DoTimer(timer, now);
    }
    input_unlock();
}

/* If time has rewound, re-run every affected timer. Timers may drop out of
 * the list, so the walk restarts after each. */
static void
CheckAllTimers(void)
{
    OsTimerPtr timer;
    CARD32 now;
    input_lock();
 start:
    now = GetTimeInMillis();
    xorg_list_for_each_entry(timer, &timers, list) {
        if (timer->expires - now > timer->delta + 250) {
            DoTimer(timer, now);
            goto start;
        }
    }
    input_unlock();
}

/* Milliseconds until the next timer, running any that are due.
 * -1 means "no timers". Called by WaitForSomething. */
int
amber_os_check_timers(void)
{
    OsTimerPtr timer;
    if ((timer = first_timer()) != NULL) {
        CARD32 now = GetTimeInMillis();
        int timeout = timer->expires - now;
        if (timeout <= 0) {
            DoTimers(now);
        }
        else {
            if (timeout < (int) (timer->delta + 250))
                return timeout;
            CheckAllTimers();
        }
        return 0;
    }
    return -1;
}

OsTimerPtr
TimerSet(OsTimerPtr timer, int flags, CARD32 millis,
         OsTimerCallback func, void *arg)
{
    OsTimerPtr existing;
    CARD32 now = GetTimeInMillis();

    if (!timers_ready)
        TimerInit();
    if (!timer) {
        timer = calloc(1, sizeof(struct _OsTimerRec));
        if (!timer)
            return NULL;
        xorg_list_init(&timer->list);
    }
    else {
        input_lock();
        if (timer_pending(timer)) {
            xorg_list_del(&timer->list);
            if (flags & TimerForceOld)
                (void) (*timer->callback) (timer, now, timer->arg);
        }
        input_unlock();
    }
    if (!millis)
        return timer;
    if (flags & TimerAbsolute) {
        timer->delta = millis - now;
    }
    else {
        timer->delta = millis;
        millis += now;
    }
    timer->expires = millis;
    timer->callback = func;
    timer->arg = arg;
    input_lock();
    xorg_list_for_each_entry(existing, &timers, list)
        if ((int) (existing->expires - millis) > 0)
            break;
    xorg_list_append(&timer->list, &existing->list);
    if ((int) (millis - now) <= 0)
        DoTimer(timer, now);
    input_unlock();
    return timer;
}

Bool
TimerForce(OsTimerPtr timer)
{
    int pending;
    input_lock();
    pending = timer_pending(timer);
    if (pending)
        DoTimer(timer, GetTimeInMillis());
    input_unlock();
    return pending;
}

void
TimerCancel(OsTimerPtr timer)
{
    if (!timer)
        return;
    input_lock();
    xorg_list_del(&timer->list);
    input_unlock();
}

void
TimerFree(OsTimerPtr timer)
{
    if (!timer)
        return;
    TimerCancel(timer);
    free(timer);
}

void
TimerCheck(void)
{
    DoTimers(GetTimeInMillis());
}

void
TimerInit(void)
{
    OsTimerPtr timer, tmp;
    if (!timers_ready) {
        timers_ready = TRUE;
        xorg_list_init(&timers);
    }
    xorg_list_for_each_entry_safe(timer, tmp, &timers, list) {
        xorg_list_del(&timer->list);
        free(timer);
    }
}

void
AdjustWaitForDelay(void *waitTime, int newdelay)
{
    int *timeoutp = waitTime;
    int timeout = *timeoutp;
    if (timeout < 0 || newdelay < timeout)
        *timeoutp = newdelay;
}

/* ---- the screen saver timer --------------------------------------------- */
/* Upstream's version also drives DPMS. DPMS is off in AmberX (the display
 * is a window on someone's desktop; Windows owns power), so this is the
 * blanking half only. */
static OsTimerPtr ScreenSaverTimer;

static CARD32
ScreenSaverTimeoutExpire(OsTimerPtr timer, CARD32 now, void *arg)
{
    INT32 timeout = now - LastEventTime(XIAllDevices).milliseconds;
    CARD32 nextTimeout = 0;

    if (!ScreenSaverTime)
        return 0;
    if (timeout < ScreenSaverTime)
        return ScreenSaverTime - timeout;
    ResetOsBuffers();
    dixSaveScreens(serverClient, SCREEN_SAVER_ON, ScreenSaverActive);
    if (ScreenSaverInterval > 0)
        nextTimeout = ScreenSaverInterval;
    return nextTimeout;
}

void
SetScreenSaverTimer(void)
{
    CARD32 timeout = 0;
    if (ScreenSaverTime > 0)
        timeout = ScreenSaverTime;
    if (timeout)
        ScreenSaverTimer = TimerSet(ScreenSaverTimer, 0, timeout,
                                    ScreenSaverTimeoutExpire, NULL);
    else if (ScreenSaverTimer)
        TimerCancel(ScreenSaverTimer);
}

void
FreeScreenSaverTimer(void)
{
    if (ScreenSaverTimer) {
        TimerFree(ScreenSaverTimer);
        ScreenSaverTimer = NULL;
    }
}

/* ---- ClientSleepUntil ---------------------------------------------------- */
/* Put a client to sleep until an absolute time, then wake it and tell the
 * caller. Used by the sync extension's alarms and by grabs with timeouts. */
typedef struct {
    ClientPtr client;
    ClientSleepProcPtr notifyFunc;
    void *closure;
    OsTimerPtr timer;
} SleepInfoRec, *SleepInfoPtr;

static Bool
SleepWakeProc(ClientPtr client, void *closure)
{
    SleepInfoPtr info = closure;
    if (info->notifyFunc)
        (*info->notifyFunc) (client, info->closure);
    TimerFree(info->timer);
    free(info);
    return TRUE;
}

static CARD32
SleepTimerFire(OsTimerPtr timer, CARD32 now, void *arg)
{
    SleepInfoPtr info = arg;
    (void) timer;
    (void) now;
    info->timer = NULL;         /* DoTimer unlinked it; SleepWakeProc must not free it twice */
    free(timer);
    if (!info->client->clientGone)
        ClientWakeup(info->client);
    else {
        free(info);
    }
    return 0;
}

Bool
ClientSleepUntil(ClientPtr client, TimeStamp *revive,
                 ClientSleepProcPtr notifyFunc, void *closure)
{
    SleepInfoPtr info = calloc(1, sizeof(SleepInfoRec));
    if (!info)
        return FALSE;
    info->client = client;
    info->notifyFunc = notifyFunc;
    info->closure = closure;
    if (!ClientSleep(client, SleepWakeProc, info)) {
        free(info);
        return FALSE;
    }
    info->timer = TimerSet(NULL, TimerAbsolute, revive->milliseconds,
                           SleepTimerFire, info);
    if (!info->timer) {
        ClientWakeup(client);
        return FALSE;
    }
    return TRUE;
}
