/* os_log.c — the X.Org logging entry points, on a bounded sink. Original
 * AmberSSH file; replaces xserver/os/log.c.
 *
 * Rule 9 of the AmberX prompt lives here: the server never logs cookies,
 * clipboard contents, window contents or raw X11 bytes. Upstream's log.c
 * does not do that either, but it writes to a file whose name and size are
 * open-ended. This one formats into a fixed buffer, truncates, and hands the
 * line to the Windows side, which decides where lines go (today: stderr,
 * and a HostError frame for X_ERROR and above, capped by the protocol).
 *
 * Nothing here is signal-safe because nothing here is called from a signal:
 * AmberX has no signal handlers. ErrorFSigSafe is therefore ordinary ErrorF. */
#include <dix-config.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "opaque.h"
#include "amberwin.h"

int auditTrailLevel = 1;

static void
emit(int level, const char *prefix, const char *f, va_list args)
{
    char line[AMBERWIN_LOG_MAX];
    size_t n = 0;
    if (prefix && *prefix) {
        n = strlen(prefix);
        if (n >= sizeof line - 1)
            n = sizeof line - 2;
        memcpy(line, prefix, n);
        line[n++] = ' ';
    }
    vsnprintf(line + n, sizeof line - n, f, args);
    /* upstream messages end in '\n'; the sink adds its own line ending */
    n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    amberwin_log(level, line);
}

static const char *
type_prefix(MessageType type, int *level)
{
    switch (type) {
    case X_PROBED:   *level = AMBERWIN_LOG_INFO;  return "(--)";
    case X_CONFIG:   *level = AMBERWIN_LOG_INFO;  return "(**)";
    case X_DEFAULT:  *level = AMBERWIN_LOG_INFO;  return "(==)";
    case X_CMDLINE:  *level = AMBERWIN_LOG_INFO;  return "(++)";
    case X_NOTICE:   *level = AMBERWIN_LOG_INFO;  return "(!!)";
    case X_ERROR:    *level = AMBERWIN_LOG_ERROR; return "(EE)";
    case X_WARNING:  *level = AMBERWIN_LOG_WARN;  return "(WW)";
    case X_INFO:     *level = AMBERWIN_LOG_INFO;  return "(II)";
    case X_NOT_IMPLEMENTED: *level = AMBERWIN_LOG_WARN; return "(NI)";
    case X_DEBUG:    *level = AMBERWIN_LOG_INFO;  return "(DB)";
    case X_NONE:
    default:         *level = AMBERWIN_LOG_INFO;  return "";
    }
}

void
LogVMessageVerb(MessageType type, int verb, const char *format, va_list args)
{
    int level;
    const char *prefix = type_prefix(type, &level);
    /* verbosity above 1 is diagnostic chatter; the sink is bounded so it is
     * dropped rather than rate-limited */
    if (verb > 1 && level == AMBERWIN_LOG_INFO)
        return;
    emit(level, prefix, format, args);
}

void
LogMessageVerb(MessageType type, int verb, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    LogVMessageVerb(type, verb, format, ap);
    va_end(ap);
}

void
LogMessage(MessageType type, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    LogVMessageVerb(type, 1, format, ap);
    va_end(ap);
}

void
LogVWrite(int verb, const char *f, va_list args)
{
    if (verb > 1)
        return;
    emit(AMBERWIN_LOG_INFO, NULL, f, args);
}

void
LogWrite(int verb, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    LogVWrite(verb, f, ap);
    va_end(ap);
}

void
VErrorF(const char *f, va_list args)
{
    emit(AMBERWIN_LOG_ERROR, NULL, f, args);
}

void
ErrorF(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    VErrorF(f, ap);
    va_end(ap);
}

void
ErrorFSigSafe(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    VErrorF(f, ap);
    va_end(ap);
}

void
VAuditF(const char *f, va_list args)
{
    emit(AMBERWIN_LOG_INFO, "AUDIT:", f, args);
}

void
AuditF(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    VAuditF(f, ap);
    va_end(ap);
}

void
FreeAuditTimer(void)
{
    /* upstream batches repeated audit lines on a timer; this sink does not */
}

void
FatalError(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    emit(AMBERWIN_LOG_FATAL, "Fatal server error:", f, ap);
    va_end(ap);
    ddxGiveUp(EXIT_ERR_ABORT);
    amberwin_exit(1);
}

void
LogPrintMarkers(void)
{
}

Bool
LogSetParameter(LogParameter param, int value)
{
    (void) param;
    (void) value;
    return FALSE;
}
