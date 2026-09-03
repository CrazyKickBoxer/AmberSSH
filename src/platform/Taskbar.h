// Taskbar.h — progress and state on the Windows taskbar button.
//
// While a command runs the button shows an indeterminate shimmer; when the
// output carries a percentage (apt, pip, rsync, curl, docker) it becomes a
// real bar; a failed command turns it red. The point is that a long-running
// command stays legible from Alt-Tab and from another application, without
// the terminal window being visible at all.
#pragma once

#include <Windows.h>

#include <cstdint>

namespace amber
{

enum class TaskbarState
{
    None = 0,          // clear the indicator
    Indeterminate,     // running, no measurable progress
    Normal,            // running with a percentage
    Error,             // finished badly (red)
    Paused,            // amber
};

// Safe to call every frame and on any thread that owns the window: redundant
// updates are cheap, and the whole thing silently does nothing if the shell
// taskbar interface is unavailable.
void TaskbarProgress(HWND hwnd, TaskbarState state, uint64_t done = 0,
                     uint64_t total = 100);

// Releases the cached shell interface. Call once at shutdown.
void TaskbarShutdown();

} // namespace amber
