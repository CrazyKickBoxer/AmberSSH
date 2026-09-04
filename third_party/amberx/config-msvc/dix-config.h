/* dix-config.h — AmberX build configuration for MSVC / Windows x64.
 *
 * Hand-written replacement for the meson-generated file. Every macro here is
 * one that include/dix-config.h.in in the pinned xserver tree can define;
 * nothing is invented. Deliberately minimal: the Phase 1 gate is "does the
 * device-independent core compile at all", and every feature not needed to
 * answer that is left off so the answer is not muddied by extension code.
 *
 * Original AmberSSH file. Not derived from any other X server's Windows
 * configuration.
 */
#ifndef AMBERX_DIX_CONFIG_H
#define AMBERX_DIX_CONFIG_H
/* servermd.h refuses to compile unless the real config's guard is present:
 * it is how upstream stops out-of-tree drivers skipping configuration. */
#define _DIX_CONFIG_H_ 1

/* ---- platform ------------------------------------------------------- */
/* Windows x64: long is 32 bits, so the X11 fixed-width types are right
 * without LONG64, but the server-side XID/Atom sizing needs _XSERVER64. */
#define _XSERVER64 1
#define X_BYTE_ORDER X_LITTLE_ENDIAN

/* POSIX types MSVC lacks that appear in core prototypes (os.h uses
 * sigset_t for xthread_sigmask, pid_t for client credentials). Measurement
 * shims: the functions that take them are os/ functions the port replaces.
 * dix-config.h.in itself reserves a slot for pid_t, so this is in keeping. */
#ifndef _SIGSET_T_DEFINED_AMBERX
#define _SIGSET_T_DEFINED_AMBERX
typedef unsigned long sigset_t;
#endif
#ifndef _PID_T_DEFINED_AMBERX
#define _PID_T_DEFINED_AMBERX
typedef int pid_t;
#endif

/* ffs(): POSIX puts it in <strings.h>, but mi/mibitblt.c calls it without
 * including that header (it expects __builtin_ffs under GCC). Declared here,
 * where every translation unit sees it, so MSVC does not fall back to an
 * implicit int-returning declaration (C4013). AmberWinOS defines it over
 * _BitScanForward. Declaration only, like every other shim. */
int ffs(int i);

#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_FCNTL_H 1
/* No unistd.h, no strings.h, no dirent.h, no dlfcn.h on MSVC. */

/* The UCRT has had cbrt() since VS2015 and MSVC treats it as an intrinsic
 * under /O2: mi/miarc.c's fallback definition then collides with math.h's
 * dllimport declaration (C2491). The probe missed this because it compiled
 * at /Od, where cbrt is an ordinary function. */
#define HAVE_CBRT 1

/* SHA-1 for authorization ids: upstream already supports the Windows
 * CryptoAPI backend in os/xsha1.c, so no third-party hash library is needed
 * and no new licence enters the tree. */
#define HAVE_SHA1_IN_CRYPTOAPI 1

/* MSVC lacks these; upstream ships os/ replacements, which the probe
 * excludes, so leaving them undefined tells the truth at link time. */
/* HAVE_STRNDUP, HAVE_STRCASECMP, HAVE_STRLCPY, HAVE_STRLCAT,
   HAVE_REALLOCARRAY, HAVE_TIMINGSAFE_MEMCMP, HAVE_VASPRINTF: undefined */

/* ---- identity -------------------------------------------------------- */
#define XVENDORNAME "AmberX"
#define XORG_VERSION_CURRENT (((21) * 10000000) + ((1) * 100000) + ((24) * 1000))
#define COMPILEDDEFAULTFONTPATH "built-ins"
#define SERVER_MISC_CONFIG_PATH ""
#define BUILDERSTRING "AmberX Phase 1 probe"

/* ---- extensions in scope for the first milestone -------------------- */
#define BIGREQS 1
#define XCMISC 1
#define RENDER 1
#define RANDR 1
#define XFIXES 1
#define DAMAGE 1
#define COMPOSITE 1
#define SHAPE 1
#define XSYNC 1
#define XTEST 1
#define XINPUT 1
#define PRESENT 1
#define XCSECURITY 1
#define XACE 1
#define XREGISTRY 1
#define MITSHM 0

/* ---- explicitly OFF ------------------------------------------------- */
/* GLXEXT, GLX_DRI, DRI2, DRI3, GLAMOR, XvExtension, XvMCExtension, XF86DRI,
   XF86VIDMODE, XFreeXDGA, PANORAMIX, XINERAMA, XDMCP, XRECORD, RES, DBE,
   SCREENSAVER, DPMSExtension, HASXDMAUTH, SECURE_RPC, XSELINUX, INPUTTHREAD,
   MONOTONIC_CLOCK, HAVE_TYPEOF, LISTEN_TCP, LISTEN_UNIX, LISTEN_LOCAL,
   TCPCONN, UNIXCONN, LOCALCONN, IPv6, XSERVER_DTRACE, BUSFAULT: undefined */

/* No listener of any kind: AmberX clients arrive over the control pipe. */
#define NO_LOCAL_CLIENT_CRED 1

#endif
