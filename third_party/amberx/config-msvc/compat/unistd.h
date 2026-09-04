/* unistd.h — PROBE SHIM. Declarations only, so the few core files that
 * include it (dix/main.c chief among them, which AmberXHost replaces) get
 * past the include and the probe measures the rest. Functions that MSVC has
 * under an underscore name map to it; the others are declared and not
 * defined, so nothing can link against a pretend POSIX. Original AmberSSH
 * file. */
#ifndef AMBERX_PROBE_UNISTD_H
#define AMBERX_PROBE_UNISTD_H
#include <io.h>
#include <process.h>
#include <direct.h>
#include <stdlib.h>
#define getpid   _getpid
#define isatty   _isatty
#define access   _access
#define dup2     _dup2
#define getcwd   _getcwd
#define chdir    _chdir
#define unlink   _unlink
/* access() modes: MSVC's _access takes the same numeric values but never
 * names them. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
typedef int uid_t;
typedef int gid_t;
typedef long ssize_t;
typedef long off_t;
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int   setuid(uid_t);
int   setgid(gid_t);
int   seteuid(uid_t);
int   setegid(gid_t);
unsigned sleep(unsigned);
int   usleep(unsigned);
long  sysconf(int);
#endif
