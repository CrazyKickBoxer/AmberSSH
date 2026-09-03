#include "Notify.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstring>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace amber
{

void TrayNotifier::Init(HWND hwnd, UINT callbackMsg)
{
    m_hwnd = hwnd;
    m_msg = callbackMsg;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = callbackMsg;
    // The app's own icon (first icon resource), else the stock application icon.
    nid.hIcon = reinterpret_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                   0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!nid.hIcon)
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"AmberSSH");
    m_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (m_added)
    {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayNotifier::Shutdown()
{
    if (!m_added)
        return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_added = false;
}

void TrayNotifier::Toast(const std::wstring& title, const std::wstring& text)
{
    if (!m_added)
        return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void SetTaskbarProgress(HWND hwnd, double fraction)
{
    Microsoft::WRL::ComPtr<ITaskbarList3> tb;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&tb))) ||
        FAILED(tb->HrInit()))
        return;
    if (fraction < 0.0)
    {
        tb->SetProgressState(hwnd, TBPF_NOPROGRESS);
        return;
    }
    tb->SetProgressState(hwnd, TBPF_NORMAL);
    ULONGLONG v = static_cast<ULONGLONG>(fraction * 1000.0);
    tb->SetProgressValue(hwnd, v > 1000 ? 1000 : v, 1000);
}

} // namespace amber
