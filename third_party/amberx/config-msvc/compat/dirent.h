/* dirent.h — POSIX directory iteration, for MSVC. Original AmberSSH file.
 *
 * libXfont2's fontfile/dirfile.c walks font directories with opendir().
 * Declaration only here; the Windows side implements these three over
 * FindFirstFileA in its own translation unit. This header carries no X
 * type and no Windows type, so both sides may include it. */
#ifndef AMBERX_COMPAT_DIRENT_H
#define AMBERX_COMPAT_DIRENT_H
#include <stddef.h>
struct dirent {
    char d_name[260];
};
typedef struct AmberWinDir DIR;
DIR *opendir(const char *name);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
