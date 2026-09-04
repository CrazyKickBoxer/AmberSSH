/* sys/param.h — PROBE SHIM. include/misc.h includes it for MIN/MAX and
 * MAXPATHLEN. MSVC has no such header; these are the definitions the core
 * expects. Original AmberSSH file. */
#ifndef AMBERX_PROBE_SYS_PARAM_H
#define AMBERX_PROBE_SYS_PARAM_H
#include <stdlib.h>
#include <limits.h>
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN 260
#endif
#endif
