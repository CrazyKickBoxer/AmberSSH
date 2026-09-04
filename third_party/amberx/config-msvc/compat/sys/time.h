/* sys/time.h — PROBE SHIM.
 *
 * Defines struct timeval itself rather than including <winsock2.h>: the
 * Winsock route drags in <windows.h>, whose BOOL collides with X11's BOOL
 * (a CARD8 in Xmd.h). That collision is the oldest problem in porting X to
 * Windows, and the rule the port will follow is the one this file follows —
 * X headers and Windows headers are never visible to the same translation
 * unit. gettimeofday is declared only; the Windows os layer implements it
 * over GetSystemTimePreciseAsFileTime. Original AmberSSH file. */
#ifndef AMBERX_PROBE_SYS_TIME_H
#define AMBERX_PROBE_SYS_TIME_H
#include <time.h>
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval { long tv_sec; long tv_usec; };
#endif
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval*, struct timezone*);
#endif
