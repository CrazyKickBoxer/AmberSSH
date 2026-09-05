// main.cpp — WinMain, window class, dark title bar, message loop.
#include "common.h"
#include "app.h"
#include "platform/JumpList.h"
#include "amberx/AmberXController.h"
#include "resource.h"

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

static App* g_app = nullptr;

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam)
{
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app)
    {
        bool handled = false;
        LRESULT r = app->WndProc(hwnd, msg, wParam, lParam, handled);
        if (handled)
            return r;
    }
    else if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --diag: start with the local color/attribute diagnostic screen instead
    // of the connection manager (no SSH). Used for visual and perf checks.
    bool diagMode = lpCmdLine && wcsstr(lpCmdLine, L"--diag") != nullptr;
    // --connect <profile-id>: taskbar jump-list launch — connect straight to
    // a saved profile.
    std::string connectId;
    if (const wchar_t* c = lpCmdLine ? wcsstr(lpCmdLine, L"--connect") : nullptr)
    {
        c += 9;
        while (*c == L' ' || *c == L'=')
            ++c;
        while (*c && *c != L' ')
            connectId.push_back(static_cast<char>(*c++));
    }
    // --play <file.cast>: open an asciinema recording in a local tab (no
    // SSH). Used to replay full-screen app captures for visual checks.
    std::wstring playPath;
    if (const wchar_t* p = lpCmdLine ? wcsstr(lpCmdLine, L"--play") : nullptr)
    {
        p += 6;
        while (*p == L' ' || *p == L'=')
            ++p;
        if (*p == L'"')
        {
            ++p;
            while (*p && *p != L'"')
                playPath.push_back(*p++);
        }
        else
            while (*p && *p != L' ')
                playPath.push_back(*p++);
    }
    // --local <shell-key>: open a local console session straight away
    // ("pwsh", "cmd", "wsl:Ubuntu"). Same path the menu and palette use.
    std::string localShell;
    if (const wchar_t* l = lpCmdLine ? wcsstr(lpCmdLine, L"--local") : nullptr)
    {
        l += 7;
        while (*l == L' ' || *l == L'=')
            ++l;
        while (*l && *l != L' ')
            localShell.push_back(static_cast<char>(*l++));
    }
    // --preview-safety: open the host-key and blast-radius boxes once with
    // sample content, then exit. Nothing connects and nothing is sent — it is
    // there so both modals can be reviewed on every interface skin without a
    // server, the same way --diag exists for the terminal itself.
    // 1 = an unrecognised key, 2 = the changed-key alarm.
    int previewSafety = 0;
    if (lpCmdLine && wcsstr(lpCmdLine, L"--preview-safety"))
        previewSafety = wcsstr(lpCmdLine, L"--changed") ? 2 : 1;
    // --preview-amberx: launch an AmberXHost, handshake, push a cookie and
    // a channel through it, and write a report. Needs no window and no GPU,
    // so it runs and exits before anything else is set up.
    if (lpCmdLine && wcsstr(lpCmdLine, L"--preview-amberx"))
        return amber::amberx::RunPreview();
    // --amberx-report: the version and capability report the release bundle
    // is required to carry, written to %TEMP%\amberx-report.txt and to the
    // console when there is one. Answers "what is this, what is it built
    // from, and what does it not do" without starting anything.
    if (lpCmdLine && wcsstr(lpCmdLine, L"--amberx-report"))
        return amber::amberx::WriteReport();
    amber::InitAppUserModelId();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"AmberSSHWindow";
    // The application mark, on the class so the taskbar, Alt-Tab and the task
    // manager all pick it up. LoadIconW gives the shell-sized frame; the small
    // one is loaded at the caption size explicitly.
    wc.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(
        LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!RegisterClassExW(&wc))
        return 1;

    // Default 1600x900, scaled for the primary monitor's DPI.
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1600, dpi, 96);
    int h = MulDiv(900, dpi, 96);
    RECT wr = { 0, 0, w, h };
    AdjustWindowRectExForDpi(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AmberSSH", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
        return 1;

    // Windows 11 dark title bar; rounded corners stay at the system default.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));

    App app;
    // --vnc-selfcheck: an RFB server in-process, a VNC tab at solidity 1,
    // the scene read back and compared with the picture served. Needs the
    // window and the GPU, so it runs inside the ordinary loop and exits
    // through the message queue with 0 (passed) or 1; the report is in
    // %TEMP%\vnc-selfcheck.txt.
    if (lpCmdLine && wcsstr(lpCmdLine, L"--vnc-selfcheck"))
        app.RequestVncSelfCheck();
    // --vnc-bench: the same harness serving 3840x2160, measuring frame times
    // over a static desktop, a dragged block and full-frame video, vsync off;
    // report in %TEMP%\vnc-bench.txt. Exits 0 — it measures, it does not judge.
    if (lpCmdLine && wcsstr(lpCmdLine, L"--vnc-bench"))
        app.RequestVncBench();
    g_app = &app;
    // Route messages to the app BEFORE Init so the custom-frame WM_NCCALCSIZE
    // that Init triggers (SWP_FRAMECHANGED) reaches App::WndProc.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    try
    {
        if (!app.Init(hwnd, diagMode, connectId, playPath, localShell, previewSafety))
        {
            MessageBoxW(hwnd, L"Failed to initialize DirectX 12 renderer.",
                        L"AmberSSH", MB_ICONERROR);
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        MessageBoxW(hwnd, WideFromUtf8(e.what()).c_str(), L"AmberSSH — init failed",
                    MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message loop: pump everything, then render one frame when idle.
    MSG msg = {};
    bool running = true;
    int exitCode = 0;
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                exitCode = static_cast<int>(msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        try
        {
            app.Tick();
        }
        catch (const std::exception& e)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            MessageBoxW(hwnd, WideFromUtf8(e.what()).c_str(),
                        L"AmberSSH — fatal error", MB_ICONERROR);
            running = false;
            exitCode = 2;
        }
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    app.Shutdown();
    g_app = nullptr;
    return exitCode;
}
