// CrashReport.h — one file saying where AmberSSH died, and nothing else.
//
// The AmberX host has said this since it was written; the process holding the
// credentials has not. A GUI application has no stderr, so a crash has until
// now produced a message box with an exception code and nothing to act on.
//
// WHAT IT RECORDS
//   The exception code, the faulting address as module+offset, the read or
//   write address for an access violation, the call stack as module+offset,
//   and the load address of every module so those offsets can be resolved
//   later against the .pdb or the linker map.
//
// WHAT IT DELIBERATELY DOES NOT RECORD
//   Any memory contents. No minidump, no register dump, no stack bytes, no
//   heap. This process holds passwords, key passphrases and terminal
//   scrollback, and a crash file is a file that gets attached to a bug
//   report and emailed. Module offsets identify the code; memory would
//   identify the session. The project's rule is that secrets are not written
//   to disk, and a crash is not an exception to it.
//
//   The consequence, stated so nobody expects otherwise: these reports say
//   which code path died and not what it was holding. Diagnosing a data-
//   dependent crash from one of these needs the path plus a reproduction,
//   not the file alone.
//
// The handler allocates nothing and takes no lock. The heap may already be
// the thing that is broken.
#pragma once

#include <string>

namespace amber
{

// Installs the unhandled-exception filter. Call once, early, before anything
// that can fault. Safe to call when the directory cannot be created: the
// handler then writes nothing rather than failing at crash time.
void InstallCrashHandler();

// Where reports go: %LOCALAPPDATA%\AmberSSH\crashes. Exposed so the About
// box and the docs can name it rather than describing it.
std::wstring CrashReportDirectory();

} // namespace amber
