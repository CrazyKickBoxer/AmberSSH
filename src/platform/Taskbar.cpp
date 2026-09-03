#include "Taskbar.h"

#include <shobjidl_core.h>

namespace amber
{
namespace
{

ITaskbarList3* g_taskbar = nullptr;
bool g_tried = false;

ITaskbarList3* Get()
{
    if (g_taskbar || g_tried)
        return g_taskbar;
    g_tried = true;   // one attempt only: a failure here is permanent
    ITaskbarList3* p = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&p));
    if (FAILED(hr) || !p)
        return nullptr;
    if (FAILED(p->HrInit()))
    {
        p->Release();
        return nullptr;
    }
    g_taskbar = p;
    return g_taskbar;
}

} // namespace

void TaskbarProgress(HWND hwnd, TaskbarState state, uint64_t done, uint64_t total)
{
    if (!hwnd)
        return;
    ITaskbarList3* tb = Get();
    if (!tb)
        return;
    TBPFLAG flag = TBPF_NOPROGRESS;
    switch (state)
    {
    case TaskbarState::Indeterminate: flag = TBPF_INDETERMINATE; break;
    case TaskbarState::Normal:        flag = TBPF_NORMAL;        break;
    case TaskbarState::Error:         flag = TBPF_ERROR;         break;
    case TaskbarState::Paused:        flag = TBPF_PAUSED;        break;
    case TaskbarState::None:          flag = TBPF_NOPROGRESS;    break;
    }
    // The value has to be set before the state for a determinate bar, or the
    // shell briefly paints a full bar in the new colour.
    if (state == TaskbarState::Normal || state == TaskbarState::Error ||
        state == TaskbarState::Paused)
        tb->SetProgressValue(hwnd, done, total ? total : 100);
    tb->SetProgressState(hwnd, flag);
}

void TaskbarShutdown()
{
    if (g_taskbar)
    {
        g_taskbar->Release();
        g_taskbar = nullptr;
    }
}

} // namespace amber
