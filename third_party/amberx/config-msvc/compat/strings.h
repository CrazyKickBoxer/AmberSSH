/* strings.h — PROBE SHIM. The BSD string header; MSVC keeps the same
 * functions under different names in <string.h>. This mapping is the
 * standard one every Windows port makes and is not measurement-only: it can
 * stay. Original AmberSSH file. */
#ifndef AMBERX_PROBE_STRINGS_H
#define AMBERX_PROBE_STRINGS_H
#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif
