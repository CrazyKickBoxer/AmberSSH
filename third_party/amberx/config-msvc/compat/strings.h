/* strings.h — the BSD string header, for MSVC. Original AmberSSH file.
 *
 * Deliberately does NOT map strcasecmp/strncasecmp to _stricmp/_strnicmp:
 * with HAVE_STRCASECMP undefined, include/os.h routes both to upstream's
 * xstrcasecmp (os/strcasecmp.c, a retained pure-C file), and a second macro
 * here only produced a C4005 redefinition that os.h then overrode anyway.
 * One definition, upstream's, and the same one on every platform.
 *
 * ffs() is declared in dix-config.h, not here: mi/mibitblt.c calls it
 * without including <strings.h>, so this header cannot reach it. */
#ifndef AMBERX_COMPAT_STRINGS_H
#define AMBERX_COMPAT_STRINGS_H
#include <string.h>
#endif
