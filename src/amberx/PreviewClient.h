// PreviewClient.h — the hand-written X client that --preview-amberx drives
// through a started controller. Returns the number of failed checks; every
// check is reported through `line(step, ok, detail)`.
#pragma once

#include <functional>
#include <string>

#include "AmberXController.h"

namespace amber::amberx
{

int RunPreviewClient(AmberXController& c,
                     const std::function<void(const char*, bool, const std::string&)>& line);

// Phase 5: on a started host with its cookie set — what restricted and
// trusted enforce differently, and what the resource limits refuse.
int RunPreviewTrustChecks(AmberXController& c, bool trusted,
                          const std::function<void(const char*, bool, const std::string&)>& line);
// On a host launched with a 2-second authorization timeout.
int RunPreviewTimeoutCheck(AmberXController& c,
                           const std::function<void(const char*, bool, const std::string&)>& line);

// Phase 6: the clipboard bridge, on a host launched with `mode`
// (AMBERWIN_CLIP_* in server/amberwin.h). Checks both directions against
// what that mode is supposed to allow, so a mode that lets text through in a
// direction the user disabled fails the gate.
int RunPreviewClipboardChecks(AmberXController& c, int mode,
                              const std::function<void(const char*, bool, const std::string&)>& line);

} // namespace amber::amberx
