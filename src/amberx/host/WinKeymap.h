// WinKeymap.h — the Windows half of keyboard layout support.
// See WinKeymap.cpp for what "reading a layout" means here.
#pragma once

#include "../server/amberwin.h"

namespace amber::amberx
{

// Reads the calling thread's current keyboard layout and caches it.
// Returns the cached description, or nullptr if the layout produced nothing
// usable (in which case the X side keeps its built-in US map). Call again
// after WM_INPUTLANGCHANGE; the returned pointer stays valid until then.
const amberwin_keymap* ReadCurrentLayout();

// The description from the last successful ReadCurrentLayout, or nullptr.
const amberwin_keymap* CurrentLayout();

} // namespace amber::amberx
