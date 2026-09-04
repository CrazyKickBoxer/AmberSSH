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

} // namespace amber::amberx
