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

} // namespace amber::amberx
