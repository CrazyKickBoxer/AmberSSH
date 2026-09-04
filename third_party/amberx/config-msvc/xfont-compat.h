/* xfont-compat.h — force-included into every libXfont2 translation unit
 * (/FI). One declaration libXfont2 assumes from headers MSVC lacks. On a
 * 64-bit build an undeclared function is an int-returning one, and a
 * pointer returned through int is truncated and sign-extended: that is a
 * crash, not a warning, so nothing here may be left implicit.
 * Original AmberSSH file. */
#ifndef AMBERX_XFONT_COMPAT_H
#define AMBERX_XFONT_COMPAT_H
#include <string.h>
int strcasecmp(const char *a, const char *b);
#endif
