/* os_misc.c — the rest of what the X.Org core wants from its os layer:
 * init and teardown, the command line, process and identity stubs, the
 * input-thread locks, extension globals, and the POSIX leftovers.
 * Original AmberSSH file; replaces the non-allocation half of
 * xserver/os/utils.c plus os/osinit.c and os/client.c.
 *
 * Every function that upstream implements by talking to the OS — spawning,
 * signals, credentials, PIDs of peers — is answered here with "no" rather
 * than with a Windows imitation, because for AmberX the honest answer is no:
 * the host cannot spawn (Job Object), has no signals, and its clients are on
 * another machine. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include "misc.h"
#include "os.h"
#include "opaque.h"
#include "dixstruct.h"
#include "input.h"
#include "amberwin.h"
#include "amberlimits.h"

/* ---- extension globals (normally os/utils.c) -------------------------- */
Bool noCompositeExtension = FALSE;
Bool noDamageExtension = FALSE;
Bool noGEExtension = FALSE;
Bool noMITShmExtension = TRUE;      /* not advertised, by policy */
Bool noRRExtension = FALSE;
Bool noRenderExtension = FALSE;
Bool noSecurityExtension = FALSE;   /* Phase 5 builds restricted mode on it */
Bool noTestExtensions = FALSE;
Bool noXFixesExtension = FALSE;
Bool AllowByteSwappedClients = TRUE;  /* a remote client's endianness is not
                                         a trust signal; the cookie is */

/* MIT-SHM is not compiled; these are what Xext/shm.c would have exported.
 * The core references them unconditionally. */
void
ShmExtensionInit(void)
{
}

void
ShmRegisterFbFuncs(ScreenPtr pScreen)
{
    (void) pScreen;
}

/* ---- init / teardown ---------------------------------------------------- */
void
OsInit(void)
{
    /* SmartScheduleSignalEnable is compiled to FALSE here (no setitimer):
     * the scheduler reads the clock after every request instead of
     * being told by a signal */
    TimerInit();
    amber_limits_os_init();

    /* AmberX never resets.
     *
     * A stock X server, when its last client disconnects, tears the whole
     * server down and builds it again — a new generation, new screen, new
     * atoms, new everything — because that is how an X display gets a clean
     * slate between logins. None of that applies to a display that belongs
     * to one SSH session: there is no next login, the cookie was delivered
     * once and cannot be delivered again, and rebuilding the screen while
     * the Windows side still holds the frames is a way to lose windows, not
     * to clean up. The session ends when the session ends.
     *
     * This is -noreset, set here rather than left to a command line nobody
     * types. The symptom when it was missing: close the last client, and
     * the next one to connect got no answer at all. */
    dispatchExceptionAtReset = 0;
}

void
OsCleanup(Bool terminating)
{
    (void) terminating;
}

void
OsVendorInit(void)
{
}

void
OsVendorFatalError(const char *f, va_list args)
{
    (void) f;
    (void) args;
}

void
OsAbort(void)
{
    amberwin_exit(3);
}

void
ddxGiveUp(enum ExitCode error)
{
    (void) error;
}

void
GiveUp(int sig)
{
    (void) sig;
    dispatchException |= DE_TERMINATE;
    isItTimeToYield = TRUE;
}

void
NotifyParentProcess(void)
{
    /* the controller learns we are up from the first HostStatus frame */
    amberwin_report(0, 0, 0);
}

void
xorg_backtrace(void)
{
}

/* ---- the command line --------------------------------------------------- */
/* AmberXHost's own arguments are parsed on the Windows side. What reaches
 * here is the argv the host chooses to forward; today nothing. */
void
UseMsg(void)
{
    ErrorF("AmberX X server (X.Org core %s): not for direct use, launched by AmberSSH\n",
           XVENDORNAME);
}

void
ProcessCommandLine(int argc, char *argv[])
{
    int i;
    for (i = 1; i < argc; i++) {
        int skip = ddxProcessArgument(argc, argv, i);
        if (skip > 0) {
            i += skip - 1;
            continue;
        }
        ErrorF("Unrecognized option: %s\n", argv[i]);
        UseMsg();
        FatalError("Unrecognized option: %s\n", argv[i]);
    }
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    (void) argc;
    (void) argv;
    (void) i;
    return 0;
}

void
ddxUseMsg(void)
{
}

void
CheckUserParameters(int argc, char **argv, char **envp)
{
    (void) argc;
    (void) argv;
    (void) envp;
}

/* ---- processes ---------------------------------------------------------- */
/* Popen exists upstream for xkbcomp. The host cannot spawn and would not be
 * allowed to: the Job Object caps the process count at one. Fail cleanly. */
void *
Popen(const char *command, const char *type)
{
    (void) command;
    (void) type;
    return NULL;
}

void *
Fopen(const char *file, const char *type)
{
    (void) file;
    (void) type;
    return NULL;
}

int
Pclose(void *iop)
{
    (void) iop;
    return -1;
}

int
Fclose(void *iop)
{
    (void) iop;
    return -1;
}

int
System(const char *command)
{
    (void) command;
    return -1;
}

/* ---- identity ----------------------------------------------------------- */
/* Unix ids for a server whose peers are on another machine and whose own
 * process identity is the Windows logon. A fixed non-root id is returned so
 * the "am I root" checks come out false. */
int
getuid(void)
{
    return 1000;
}

int
geteuid(void)
{
    return 1000;
}

/* mi/mibitblt.c: find-first-set, 1-based, 0 for no bits */
int
ffs(int i)
{
    int n;
    if (i == 0)
        return 0;
    for (n = 1; !(i & 1); n++)
        i = (int) ((unsigned) i >> 1);
    return n;
}

/* ---- the input thread --------------------------------------------------- */
/* INPUTTHREAD is off: input arrives on the server thread from
 * WaitForSomething. The locks are kept as a real (non-recursive-safe)
 * counter so a future input thread has somewhere to hang, and so that a
 * mismatched lock/unlock is visible rather than silent. */
static int input_lock_depth;

void
InputThreadPreInit(void)
{
}

void
InputThreadInit(void)
{
}

void
InputThreadFini(void)
{
}

int
InputThreadRegisterDev(int fd, NotifyFdProcPtr readInputProc, void *readInputArgs)
{
    (void) fd;
    (void) readInputProc;
    (void) readInputArgs;
    return 0;
}

int
InputThreadUnregisterDev(int fd)
{
    (void) fd;
    return 0;
}

int
in_input_thread(void)
{
    return 0;
}

void
input_lock(void)
{
    input_lock_depth++;
}

void
input_unlock(void)
{
    if (input_lock_depth > 0)
        input_lock_depth--;
}

void
input_force_unlock(void)
{
    input_lock_depth = 0;
}

/* ---- fonts -------------------------------------------------------------- */
int
set_font_authorizations(char **authorizations, int *authlen, void *client)
{
    /* font-server authorization: there is no font server */
    *authorizations = NULL;
    *authlen = 0;
    (void) client;
    return 0;
}

/* ---- DDX bell ----------------------------------------------------------- */
void
DDXRingBell(int volume, int pitch, int duration)
{
    (void) pitch;
    (void) duration;
    amberwin_bell(volume);
}

/* ---- case-insensitive compare ------------------------------------------- */
/* os.h routes strcasecmp/strncasecmp here when HAVE_STRCASECMP is unset.
 * Upstream's os/strcasecmp.c is BSD-typed (u_char); the CRT has the same
 * functions under other names, so these are the mapping and nothing more. */
int
xstrcasecmp(const char *str1, const char *str2)
{
    return _stricmp(str1, str2);
}

int
xstrncasecmp(const char *str1, const char *str2, size_t n)
{
    return _strnicmp(str1, str2, n);
}
