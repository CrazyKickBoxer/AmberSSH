// Notify.h — Windows-native notifications: a tray icon that can raise
// balloon/toast notifications (Shell_NotifyIcon NIF_INFO), plus taskbar
// progress (ITaskbarList3) for long transfers.
#pragma once

#include <Windows.h>

#include <string>

namespace amber
{

class TrayNotifier
{
public:
    // callbackMsg: WM_APP+n the tray sends interaction events to (lParam holds
    // NIN_* / mouse messages).
    void Init(HWND hwnd, UINT callbackMsg);
    void Shutdown();
    // Balloon/toast in the notification area. Title is bold in the toast.
    void Toast(const std::wstring& title, const std::wstring& text);

private:
    HWND m_hwnd = nullptr;
    UINT m_msg = 0;
    bool m_added = false;
};

// Taskbar button progress bar (green fill), 0..1; -1 clears it.
void SetTaskbarProgress(HWND hwnd, double fraction);

} // namespace amber
