// SafetyDialog.h — the two modals that stand between the user and something
// they cannot undo.
//
//   * the host-key box, which draws the host's sigil so a changed key is seen
//     rather than read;
//   * the blast-radius box, which shows what a command is about to do before
//     it is sent.
//
// Both are painted, not composed from system controls, so they take the
// active interface skin like every other native surface in AmberSSH. Both are
// modal and neither has a default that says yes.
#pragma once

#include <Windows.h>
#include <windowsx.h>

#include <string>

#include "../security/BlastRadius.h"
#include "../security/HostSigil.h"

namespace amber
{

// What the user decided about a host key. There is no "remember and skip":
// a key decision is always explicit.
enum class HostKeyChoice
{
    Reject = 0,
    Accept,
};

// `fingerprint` is the exact text the transport reported. `changed` is true
// when a key was already known for this host and does not match — the case
// the whole feature exists for, and the one that gets the alarming treatment.
HostKeyChoice ShowHostKeyDialog(HWND owner, const std::string& label,
                                const std::string& fingerprint, bool changed);

// The confirmation for a command the risk analyser flagged. Returns true only
// when the user positively confirmed; every other outcome — Esc, the close
// button, the wrong hostname typed — is false, because the safe answer to an
// unanswered question about a destructive command is "do not run it".
//
// `style` decides the friction. `hostname` is what must be typed back for
// ConfirmStyle::TypeHostname.
bool ShowRiskDialog(HWND owner, const RiskReport& report, ConfirmStyle style,
                    const std::string& hostname);

// The high-friction opt-in for trusted X11 (Phase 5 of AmberX). Trusted mode
// lets every program forwarded from `hostname` see the input and windows of
// every other, which is what X11 trusted means; restricted mode is the
// default and needs no dialog. The user must type the hostname. Returns true
// only on that positive confirmation.
bool ShowTrustedX11Dialog(HWND owner, const std::string& hostname, bool sessionOnly);

// Draws a sigil into `rc` on a GDI DC, in the colours of the active skin.
// Exposed so the tab strip and the status bar can draw the same figure at a
// smaller size — recognition only works if it is the same drawing everywhere.
void DrawSigil(HDC dc, const RECT& rc, const Sigil& s, UINT dpi, bool alarm);

} // namespace amber
