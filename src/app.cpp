#include "app.h"
#include "platform/CredentialStore.h"
#include "platform/Hello.h"
#include "platform/JumpList.h"
#include "ssh/SftpClient.h"
#include "ui/ForwardsDialog.h"
#include "ui/SftpBrowser.h"

#include <ctime>
#include <filesystem>

#include "platform/Taskbar.h"
#include "utility/PasteGuard.h"
#include "ui/AboutDialog.h"
#include "ui/SafetyDialog.h"
#include "security/PrivacyCloak.h"
#include "amberx/control/Protocol.h"
#include "remote/XAuth.h"
#include "remote/RemoteDisplay.h"
#include "ui/SkinDraw.h"
#include "ui/SkinFinish.h"
#include "platform/ConPty.h"
#include "ui/ConnectionDialog.h"
#include "ui/SftpPanel.h"
#include "ui/Theme.h"
#include "platform/CredentialStore.h"
#include "platform/Paths.h"
#include "term/Graphemes.h"

#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <urlmon.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <thread>
#include <map>
#include <unordered_map>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "urlmon.lib")

// Posted by the font-download worker when a fetch finishes: wParam = face
// index, lParam != 0 on success.
static constexpr UINT WM_APP_FONT_READY = WM_APP + 7;
static constexpr UINT WM_APP_TRAY = WM_APP + 8;   // notification-area events

// ------------------------------------------------------------------ menu ids
namespace
{
enum MenuId : int
{
    IdmNewConnection = 40001,
    IdmCloseTab,
    IdmDisconnect,
    IdmExit,

    // The motion range GROWS with the style table, so it lives above every
    // other id. It used to sit at 40100 and, at 23 styles, had grown over
    // Reduced Motion / About / Journal and swallowed their commands.
    // Local shells discovered on this machine. Like the motion range this
    // GROWS at runtime (a new WSL distro adds an entry), so it lives above
    // every fixed id — see the note on IdmMotionFirst.
    IdmLocalFirst = 41500,         // +0..31 → DiscoverLocalShells()
    IdmLocalLast = IdmLocalFirst + 31,

    // Session Guardian. Fixed ids, parked above every growing range so the
    // 40122 accident (the motion range eating About and the Journal) cannot
    // repeat here; the static_assert below is what actually enforces it.
    IdmGuardianStop = 41600,       // stop reconnecting this session
    IdmGuardianRetry,              // reconnect now, skipping the wait

    // Command blocks. Every one of these acts on the block at the cursor.
    IdmBlockCopyCommand = 41620,
    IdmBlockCopyOutput,
    IdmBlockCopyBoth,
    IdmBlockFold,                  // fold / expand this one
    IdmBlockSnippet,               // save the command to snippets.txt
    IdmBlockBookmark,              // mark it, session-local
    IdmBlockSearch,                // search inside this block's output
    IdmBlockRerun,                 // TYPE the command, never run it
    IdmBlockRerunNow,              // the deliberate run-now gesture
    IdmBlockNotify,                // tell me when this one finishes
    IdmBlockPrevBookmark,
    IdmBlockNextBookmark,
    IdmBlockSummaryCwd,            // summaries carry the working directory
    IdmBlockSummaryFirstLine,      // ...and the first line of output
    IdmBlockGutter,                // the gutter bars and hover metadata
    IdmBlockNotifyFirst = 41650,   // +0..3 → amber::NotifyOn (global default)
    IdmBlockNotifyLast = IdmBlockNotifyFirst + 3,
    IdmBlockNotifyAfter = 41660,   // set the global threshold, in seconds

    // Panes. Every one acts on the active tab's focused pane.
    IdmPaneFocusLeft = 41700,
    IdmPaneFocusRight,
    IdmPaneFocusUp,
    IdmPaneFocusDown,
    IdmPaneFocusNext,
    IdmPaneFocusPrev,
    IdmPaneMoveLeft,
    IdmPaneMoveRight,
    IdmPaneMoveUp,
    IdmPaneMoveDown,
    IdmPaneSwapLeft,
    IdmPaneSwapRight,
    IdmPaneSwapUp,
    IdmPaneSwapDown,
    IdmPaneGrow,
    IdmPaneShrink,
    IdmPaneGrowV,
    IdmPaneShrinkV,
    IdmPaneRotate,
    IdmPaneZoom,
    IdmPaneReadOnly,
    IdmPaneClose,
    // Broadcast. The picker is a modal list of the tab's panes; the rest are
    // the emergency stop and the "all/none" shortcuts.
    IdmBroadcastPick = 41740,
    IdmBroadcastStop,
    IdmBroadcastAll,

    IdmMotionFirst = 41000,        // +0..N → index into kMotionStyles
    IdmMotionLast  = IdmMotionFirst + kMotionStyleCount - 1,
    IdmReducedMotion = 40120,      // accessibility: short Direct morph only
    IdmAbout = 40121,              // About Amber SSH
    IdmJournal = 40122,            // Command Journal overlay (Ctrl+Shift+J)
    IdmJournalCapture = 40123,     // record commands on/off
    IdmJournalClear = 40124,       // forget everything recorded
    IdmStatusBar = 40125,          // the thin bar along the foot
    IdmPasteGuard = 40126,         // confirm multi-line pastes
    IdmPrevCommand = 40127,        // Ctrl+Up  — older command mark
    IdmNextCommand = 40128,        // Ctrl+Down — newer command mark
    IdmFoldToggle = 40129,         // fold / expand the command at the cursor
    IdmFoldAll = 40130,            // collapse every command's output
    IdmFoldNone = 40131,           // expand everything
    IdmPaste = 40134,              // paste (also reachable from the palette)
    IdmDepartFirst = 40140,        // +0..4 fade / ash / smoke / sand / shatter
    IdmDepartLast = IdmDepartFirst + 4,
    IdmFxParallax = 40150,         // pointer tilts the field by depth
    IdmFxSlosh,                    // the field lags when the window moves
    IdmFxWarmup,                   // cold phosphor coming to temperature
    IdmFxSpotlight,                // dim everything but the current output
    IdmNightFirst = 40160,         // +0..2 off / after dark / always
    IdmNightLast = IdmNightFirst + 2,
    IdmTimeDialReset = 40165,      // put the time dial back to 1x
    // Safety. The cloak masks likely secrets at draw time; the risk policy
    // decides which commands are worth interrupting for.
    IdmCloak = 40170,              // Privacy Cloak on/off
    IdmCloakAddrs = 40171,         // also mask IP addresses
    IdmCloakHome = 40172,          // also mask home-directory names
    IdmRiskFirst = 40173,          // +0..4 → RiskPolicy Off..Everything
    IdmRiskLast = IdmRiskFirst + 4,
    // Remote display. AmberSSH ships no X server and no RDP client; these find
    // what is installed and hand off to it (docs/REMOTE-DISPLAY.md).
    IdmXServerReport = 40178,      // what was found, and under what terms
    IdmXServerStart = 40179,       // launch the best one that is installed
    IdmRemoteApp = 40180,          // tunnel + start Weston + the RDP client
    IdmRemoteAppTunnel = 40181,    // tunnel + client only (a running service)
    IdmRemoteDisplayDocs = 40182,  // open the runbook
    IdmWorkspaceSave = 40132,      // save the open sessions as a workspace
    IdmWorkspaceDelete = 40133,
    IdmWorkspaceFirst = 40800,     // +0..23 → m_workspaceNames
    IdmWorkspaceLast = IdmWorkspaceFirst + 23,

    IdmDensityAuto = 40199,
    IdmDensityFirst = 40200,       // +0..4 → 32 / 64 / 96 / 128 / 256 per cell
    IdmDensityCustom = IdmDensityFirst + 5,
    IdmDensityLast  = IdmDensityCustom,

    IdmFxCrispCore = 40290,        // Particle + Crisp Core (default)
    IdmFxParticlesOnly,            // legacy/artistic
    IdmFxShockwave,                // shockwave on Enter/Backspace/Delete
    IdmFxCascade,                  // baud-style paced output reveal
    IdmSpeedFirst = 40295,         // +0..3 → 0.5x / 1x / 1.5x / 2x
    IdmSpeedCustom = IdmSpeedFirst + 4,
    IdmSpeedLast = IdmSpeedCustom,
    IdmFxBloom = 40300,
    IdmFxScanlines,
    IdmFxVignette,
    IdmFxDrift,
    IdmFxPointerForce,
    IdmFxMiami,
    IdmBloomFirst = 40310,         // +0..2 low/medium/high
    IdmBloomLast = IdmBloomFirst + 2,
    IdmTwinkleFirst = 40320,       // +0..2 off/subtle/full
    IdmTwinkleLast = IdmTwinkleFirst + 2,
    IdmTrailFirst = 40340,         // +0..3 off/low/medium/high
    IdmTrailLast = IdmTrailFirst + 3,

    IdmViewFullscreen = 40400,
    IdmViewVsync,
    IdmViewOverlay,
    IdmViewSyntaxTint,
    IdmViewFontLarger,
    IdmViewFontSmaller,
    IdmViewDiag,
    IdmPaletteFirst = 40420,       // +0..1 Amber Miami / Classic xterm
    IdmPaletteLast = IdmPaletteFirst + 1,
    IdmTermFirst = 40430,          // +0..2 terminal types
    IdmTermLast = IdmTermFirst + 2,
    IdmTruecolorInfo = 40440,      // disabled indicator
    IdmFontStyleFirst = 40450,     // +0..2 Modern / Dot Matrix 8 / Dot Matrix 12
    IdmFontStyleLast = IdmFontStyleFirst + 2,
    IdmFontFaceFirst = 40470,      // +0..19 typefaces (kFontFaces)
    IdmFontFaceLast = IdmFontFaceFirst + 19,
    IdmThemeFirst = 40560,         // +0..6 → kThemes (last = Custom)
    IdmThemeLast = IdmThemeFirst + 7,
    IdmThemeEdit = 40570,          // "Customize Theme..." 3-color editor
    IdmFxBell = 40571,             // BEL rings a particle shockwave
    IdmLogSession = 40572,         // toggle transcript logging (active tab)
    IdmSearchScrollback = 40573,   // Ctrl+Shift+F
    IdmSearchNext = 40574,         // F3
    IdmSftpPanel = 40575,          // SFTP browser for the active session
    IdmImportProfiles = 40576,     // PuTTY / OpenSSH config import
    IdmSplitVertical = 40577,      // split the active tab side by side
    IdmSplitHorizontal = 40578,    // split the active tab top / bottom
    IdmSplitClose = 40579,         // close the split (keep focused pane)
    IdmBroadcast = 40580,          // type into both split panes at once
    IdmCommandPalette = 40581,     // Ctrl+Shift+P fuzzy action palette
    IdmBgFirst = 40590,            // +0..3 background depth: Off/Embers/Stars/Dust
    IdmBgLast = IdmBgFirst + 3,
    IdmAppearanceFirst = 40595,    // +0 Dark, +1 Light, +2 Paperwhite, +3 Pixel Art
    IdmAppearanceLast = IdmAppearanceFirst + 3,
    IdmShadowFirst = 40600,        // +0..3 text drop shadow: Off/Soft/Medium/Strong
    IdmShadowLast = IdmShadowFirst + 3,
    IdmFxHeat = 40610,             // activity heat map
    IdmFxGhost,                    // latency ghosting (RTT cursor smear)
    IdmFxAudio,                    // audio-reactive turbulence (WASAPI loopback)
    IdmFxBoot,                     // boot sequence + phosphor warm-up on connect
    IdmSaverFirst = 40620,         // +0..3 screensaver: Off / 1 / 5 / 15 min
    IdmSaverLast = IdmSaverFirst + 3,
    IdmEditSnippets = 40630,       // open snippets.txt in the editor
    IdmEditTriggers,               // open triggers.txt
    IdmHelloUnlock,                // Windows Hello before using stored secrets
    IdmRecordCast,                 // toggle asciinema recording (active tab)
    IdmPlayCast,                   // play a .cast file in a local tab
    IdmForwardsEdit,               // port-forwarding editor (active profile)
    IdmQuakeMode,                  // global-hotkey dropdown terminal
    IdmVitals,                     // remote vitals strip in the title bar
    IdmSftpBrowser,                // WinSCP-style tabbed SFTP browser
    IdmSharpFirst = 40640,         // +0..2 text sharpness: Soft / Crisp / Razor
    IdmSharpLast = IdmSharpFirst + 2,
    IdmFxLive = 40643,             // tide marks / pulse / echo / comet / weather
    IdmFxPersist = 40644,          // phosphor persistence
    IdmFxCube = 40645,             // Compiz-style cube on session switch
    IdmNextTab = 40646,            // Ctrl+Tab equivalent (palette / WM_COMMAND)
    IdmPrevTab = 40647,            // Ctrl+Shift+Tab equivalent
    IdmChromeFirst = 40650,        // +0..kChromeCount-1 interface style (skin)
    IdmSnippetFirst = 40700,       // +0..99 → m_snippets
    IdmSnippetLast = IdmSnippetFirst + 99,
};

// ------------------------------------------------- UAH themed menu-bar draw
// The visually-styled Win32 menu bar cannot be owner-drawn through
// WM_DRAWITEM; instead the OS sends these undocumented "UAH" messages so an
// app can paint the bar itself. Structures per the public reverse-engineering
// (ysc3839/win32-darkmode); used by Windows Terminal and Notepad++.
#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU        0x0091
#define WM_UAHDRAWMENUITEM    0x0092
#endif

namespace
{
struct UAHMENU
{
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags;
};
struct UAHMENUITEMMETRICS
{
    union
    {
        struct { DWORD cx, cy; } rgsizeBar[2];
        struct { DWORD cx, cy; } rgsizePopup[4];
    };
};
struct UAHMENUPOPUPMETRICS
{
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
};
struct UAHMENUITEM
{
    int iPosition;
    UAHMENUITEMMETRICS umim;
    UAHMENUPOPUPMETRICS umpm;
};
struct UAHDRAWMENUITEM
{
    DRAWITEMSTRUCT dis;
    UAHMENU um;
    UAHMENUITEM umi;
};
} // namespace

// ------------------------------------------------------------------- themes
// Each theme is a 5-stop sRGB intensity ramp (dim glow → hot core), converted
// to linear light when applied. Index 6 is the user's Custom theme, generated
// from three picked colors.
struct ThemeDef
{
    const wchar_t* name;
    uint32_t stops[5];      // sRGB 0xRRGGBB
};
const ThemeDef kThemes[7] = {
    { L"Amber Nixie",   { 0x3D2A00, 0x7A5500, 0xFFB000, 0xFFD54A, 0xFFF3C4 } },
    { L"Emerald CRT",   { 0x062E0E, 0x0E6322, 0x2EE060, 0x8CF5AC, 0xE0FFE9 } },
    { L"Ice Cathode",   { 0x04242E, 0x0C5470, 0x2CB9F0, 0x8FE0FF, 0xE4F8FF } },
    { L"Violet Haze",   { 0x260A33, 0x571677, 0xC237F0, 0xE18CFF, 0xF8E4FF } },
    { L"Blood Cell",    { 0x330808, 0x771414, 0xF03434, 0xFF8C8C, 0xFFE4E4 } },
    { L"Paper White",   { 0x2B2B28, 0x62625B, 0xE6E6DA, 0xF3F3EA, 0xFFFFFF } },
    // Steampunk: a brass phosphor rather than a pure amber one — deep bronze
    // through aged and polished brass to a parchment white-hot.
    { L"Brass Gaslight", { 0x241A0C, 0x5E4419, 0xA87A34, 0xD9A94E, 0xF2D79A } },
};
constexpr int kThemeCount = 8;   // 7 presets + Custom
// The menu id range and the table must not drift apart.
static_assert(IdmThemeLast - IdmThemeFirst + 1 == kThemeCount,
              "IdmThemeLast must cover every preset plus Custom");
// The motion range is the only one that grows every time a style is added, so
// it must stay above every fixed id. Adding the 23rd style once pushed
// IdmMotionLast to 40122 and silently ate About and the Journal.
static_assert(IdmLocalFirst > IdmMotionFirst + 256,
              "the local-shell range must clear the motion range as it grows");
static_assert(IdmMotionFirst > IdmWorkspaceLast && IdmMotionFirst > IdmSnippetLast,
              "keep IdmMotionFirst above every other menu id");
static_assert(IdmGuardianStop > IdmLocalLast,
              "the guardian ids must clear the local-shell range as it grows");

// Terminal typefaces. Indices 0-6 are preserved for saved profiles; newer
// faces are appended. Missing fonts fall back automatically (SetFontFamily
// returns false and the previous face stays), so the futuristic picks below
// simply don't apply if the user hasn't installed them.
//   0-6   originals (JetBrains…MS Gothic)
//   7-14  popular coding fonts programmers install for terminals
//   15-19 futuristic / display monospaces
const wchar_t* const kFontFaces[20] = {
    L"JetBrains Mono", L"Cascadia Mono", L"Consolas", L"Courier New",
    L"Lucida Console", L"NSimSun", L"MS Gothic",
    // popular coding fonts
    L"Cascadia Code", L"Fira Code", L"Hack", L"Source Code Pro",
    L"IBM Plex Mono", L"Iosevka", L"Space Mono", L"Ubuntu Mono",
    // futuristic / display monospaces
    L"Share Tech Mono", L"OCR A Extended", L"Syne Mono", L"Orbitron",
    L"Michroma",
};
constexpr int kFontFaceCount = 20;

constexpr uint32_t kDensitySteps[5] = { 32, 64, 96, 128, 256 };
const char* const kDensityNames[5] = { "Performance", "Balanced",
                                       "High Clarity", "Ultra", "Ultra Max" };
const char* const kTermTypes[3] = { "xterm-256color", "screen-256color",
                                    "xterm-direct" };
constexpr float kBloomLevels[3] = { 0.25f, 0.50f, 0.90f };
} // namespace



// ---------------------------------------------------------------------- init
bool App::Init(HWND hwnd, bool diagMode, const std::string& connectId,
               const std::wstring& playPath, const std::string& localShell,
               int previewSafety)
{
    m_hwnd = hwnd;
    m_diagMode = diagMode || previewSafety != 0;
    m_previewSafety = previewSafety;
    QueryPerformanceFrequency(&m_qpcFreq);
    QueryPerformanceCounter(&m_qpcStart);

    // Dark titlebar, themed caption/border, and Termius-style rounded corners
    // on the main window (re-applied by ApplyTheme when the theme changes).
    amber::ApplyWindowChrome(hwnd);

    // The clipboard bridge listens from here on. Registering costs nothing
    // when no session wants the clipboard: OnWindowsClipboardChanged returns
    // immediately unless the session in front has asked for that direction.
    m_clipboardListener = AddClipboardFormatListener(hwnd) != 0;

    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 8);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 8);

    if (!m_device.Init(hwnd, w, h))
        return false;
    if (!m_shaders.Init())
        return false;
    if (!m_sampler.Init())
    {
        MessageBoxW(hwnd, L"No usable monospace font found (JetBrains Mono / Consolas).",
                    L"AmberSSH", MB_ICONERROR);
        return false;
    }

    if (!m_particles.Init(m_device, m_shaders)) return false;
    if (!m_bloom.Init(m_device, m_shaders)) return false;
    if (!m_composite.Init(m_device, m_shaders)) return false;
    if (!m_prims.Init(m_device, m_shaders)) return false;

    m_sceneRtvSlot = m_device.AllocRtv();
    m_sceneSrvSlot = m_device.AllocSrv();
    CreateSceneTarget();
    m_bloom.Resize(w, h);

    m_dpi = GetDpiForWindow(hwnd);
    m_fontPx = 16.0f * m_dpi / 96.0f;
    UpdateFontMetrics(m_fontPx);

    // Saved sessions. A malformed entry is skipped, never fatal.
    amber::ProfileStore::LoadReport report;
    m_profiles.Load(&report);
    if (report.skipped > 0)
        SetStatus("Skipped " + std::to_string(report.skipped) +
                  " malformed profile entries", 6.0);

    LoadSettings();
    ApplyEffectSettings();
    ApplyAppearance();

    // Load the fonts that ship with the app (exe\fonts) plus any downloaded
    // on demand (%LOCALAPPDATA%\AmberSSH\fonts) into a private DirectWrite
    // collection, so a selected face resolves without a system install.
    m_fontDirs.clear();
    m_fontDirs.push_back(ExeDir() + L"\\fonts");
    try
    {
        m_fontDirs.push_back((amber::DataRoot() / "fonts").wstring());
    }
    catch (...) {}
    m_sampler.LoadAppFonts(m_fontDirs);
    RegisterGdiFonts();
    // The strip letters in the skin's face through its own glyph atlas.
    if (m_chromeSampler.Init())
    {
        m_chromeSampler.LoadAppFonts(m_fontDirs);
        m_chromeReady = true;
    }
    ApplyChromeFace();

    {
        bool refont = false;
        // Apply the saved face (including the default index 0, so the bundled
        // JetBrains Mono loads even when it is not installed system-wide).
        int face = std::clamp(m_fontFace, 0, kFontFaceCount - 1);
        if (m_sampler.SetFontFamily(kFontFaces[face]))
            refont = true;
        else
            m_fontFace = 0;   // saved face unavailable — fall back
        if (m_fontStyle != 0)
        {
            m_sampler.SetTemplateStyle(
                static_cast<GlyphSampler::TemplateStyle>(m_fontStyle));
            refont = true;
        }
        if (refont)
            UpdateFontMetrics(m_fontPx);
    }
    BuildMenus();

    // Apply the custom-frame WM_NCCALCSIZE (removes the OS caption).
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);

    {
        char buf[512];
        DWORD n = GetEnvironmentVariableA("AMBERSSH_PERFLOG", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf))
            m_perfLogPath.assign(buf, n);
    }

    // Notification-area icon (toasts for output triggers / transfers), the
    // taskbar jump list of saved sessions, and the user's snippet/trigger
    // files.
    m_tray.Init(m_hwnd, WM_APP_TRAY);
    RebuildWorkspaceMenu();   // fills the Workspaces submenu before first use
    UpdateJumpList();
    LoadSnippets();
    LoadTriggers();
    amber::SftpBrowser::SetNewTabHandler(
        [](void* ctx) { static_cast<App*>(ctx)->SftpNewTab(); }, this);
    ApplyQuakeHotkey();

    if (m_diagMode)
    {
        StartDiagSession();
        UpdateGridDims();
        FeedDiagnostic(Cur());
        // --diag --play <cast>: a second, playback tab — two local sessions
        // to exercise tab switching (the cube) without a server.
        if (!playPath.empty())
            PlayRecordingFile(playPath);
        return true;
    }

    // --play <file.cast>: a local playback tab instead of the manager.
    if (!playPath.empty())
    {
        PlayRecordingFile(playPath);
        if (m_sessions.empty())
            return false;           // unreadable recording: exit cleanly
        UpdateGridDims();
        return true;
    }

    // --local <shell>: a console on this machine instead of the manager.
    if (!localShell.empty())
    {
        if (!NewLocalSession(localShell))
            return false;
        UpdateGridDims();
        return true;
    }

    // The connection manager is the first thing the user sees — unless a
    // jump-list launch named a profile to connect to directly.
    // --vnc-selfcheck opens its own tab on the first Tick; the modal dialog
    // here would sit in front of the loop that Tick belongs to.
    bool started = m_vncSelfCheckRequested ? true
                 : connectId.empty()       ? ShowConnectionDialog()
                                     : ConnectProfileById(connectId);
    if (!started)
        return false;               // cancelled at startup: exit cleanly
    UpdateGridDims();
    return true;
}

void App::Shutdown()
{
    m_tray.Shutdown();
    // Every worker must stop before the process exits.
    for (auto& sp : m_sessions)
        sp->ssh.Disconnect();
    m_sessions.clear();
    m_device.WaitIdle();
    m_device.Shutdown();
}

void App::CreateSceneTarget()
{
    m_device.WaitIdle();
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = std::max(8u, m_device.Width());
    rd.Height = std::max(8u, m_device.Height());
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = kSceneFormat;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kSceneFormat;

    m_scene.Reset();
    ThrowIfFailed(m_device.Dev()->CreateCommittedResource(
                      &hp, D3D12_HEAP_FLAG_NONE, &rd,
                      D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                      IID_PPV_ARGS(&m_scene)),
                  "scene target");
    m_scene->SetName(L"SceneHDR");
    m_sceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    m_device.Dev()->CreateRenderTargetView(m_scene.Get(), nullptr,
                                           m_device.RtvCpu(m_sceneRtvSlot));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = kSceneFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    m_device.Dev()->CreateShaderResourceView(m_scene.Get(), &sv,
                                             m_device.SrvCpu(m_sceneSrvSlot));
}

// Compiz cube: the departing face is a frozen copy of the last frame (scene
// + bloom result). (Re)create the copies whenever the backbuffer or the
// bloom chain changes size; a resize mid-turn simply ends the turn.
void App::EnsureCubeSnapshots()
{
    const uint32_t w = std::max(8u, m_device.Width());
    const uint32_t h = std::max(8u, m_device.Height());
    const uint32_t bw = std::max(1u, m_bloom.ResultWidth());
    const uint32_t bh = std::max(1u, m_bloom.ResultHeight());
    if (m_cubeSnap && m_cubeSnapBloom && m_cubeSnapW == w && m_cubeSnapH == h &&
        m_cubeSnapBW == bw && m_cubeSnapBH == bh)
        return;
    m_device.WaitIdle();
    auto make = [&](ComPtr<ID3D12Resource>& res, uint32_t& srvSlot,
                    uint32_t tw, uint32_t th, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = tw;
        rd.Height = th;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = kSceneFormat;
        rd.SampleDesc.Count = 1;
        res.Reset();
        ThrowIfFailed(m_device.Dev()->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                          IID_PPV_ARGS(&res)),
                      "cube snapshot");
        res->SetName(name);
        if (srvSlot == UINT32_MAX)
            srvSlot = m_device.AllocSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = kSceneFormat;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        m_device.Dev()->CreateShaderResourceView(res.Get(), &sv,
                                                 m_device.SrvCpu(srvSlot));
    };
    make(m_cubeSnap, m_cubeSnapSrv, w, h, L"CubeSnapScene");
    make(m_cubeSnapBloom, m_cubeSnapBloomSrv, bw, bh, L"CubeSnapBloom");
    m_cubeSnapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_cubeSnapBloomState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_cubeSnapW = w;
    m_cubeSnapH = h;
    m_cubeSnapBW = bw;
    m_cubeSnapBH = bh;
    m_cubeStart = -1e9;        // stale faces: end any turn in progress
    m_cubePending = false;
}

// Freeze the scene target and the bloom result (the previous frame, i.e.
// the outgoing session) into the cube snapshots.
void App::SnapshotForCube(ID3D12GraphicsCommandList* cl)
{
    if (!m_cubeSnap || !m_scene)
        return;
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                       D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    };
    barrier(m_scene.Get(), m_sceneState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(m_cubeSnap.Get(), m_cubeSnapState, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(m_cubeSnap.Get(), m_scene.Get());
    barrier(m_cubeSnap.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_cubeSnapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier(m_scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, srv);
    m_sceneState = srv;
    m_bloom.CopyResultTo(cl, m_cubeSnapBloom.Get(), m_cubeSnapBloomState);
}

void App::UpdateFontMetrics(float fontPx)
{
    m_fontPx = std::clamp(fontPx, 8.0f, 48.0f);
    m_sampler.EnsureAtlas(m_fontPx);
    if (m_chromeReady)
        m_chromeSampler.EnsureAtlas(m_fontPx);
    UpdateGridDims();
}

void App::UpdateGridDims()
{
    float cellW = m_sampler.AdvanceEm() * m_fontPx;
    float cellH = m_sampler.LineEm() * m_fontPx;
    m_gapPx = HasSession() ? std::clamp(Cur().profile.gapPx, 0, 64) : 8;   // Appearance page
    float pad = static_cast<float>(m_gapPx) * m_dpi / 96.0f;
    uint32_t w = m_device.Width(), h = m_device.Height();

    // Reserve the custom title-bar strip at the top and the status bar at the
    // foot. Both are chrome: the terminal grid is what is left between them.
    m_titleBarH = TitleBarH();
    m_statusBarH = StatusBarH();
    float top = m_titleBarH;
    float bottom = m_statusBarH;
    int cols = std::max(2, static_cast<int>((w - 2 * pad) / cellW));
    int rows = std::max(2, static_cast<int>((h - top - bottom - 2 * pad) / cellH));

    // Particle budget: never shrink the terminal — reduce density instead.
    // Blank cells take a near-free sim path and rasterize nothing, so real
    // cost tracks lit glyphs; 2.6M covers Ultra Max on a typical grid while
    // huge tiny-font grids step density down gracefully.
    constexpr uint32_t kParticleBudget = 2600000;
    uint32_t cells = static_cast<uint32_t>(cols) * static_cast<uint32_t>(rows);
    uint32_t ppc = std::clamp(std::min(RequestedDensity(),
                                       kParticleBudget / std::max(cells, 1u)),
                              8u, kMaxParticlesPerCell);
    m_particles.tun.particlesPerCell = ppc;

    m_gm.cellW = cellW;
    m_gm.cellH = cellH;
    m_gm.cols = cols;
    m_gm.rows = rows;
    m_gm.originX = (w - cols * cellW) * 0.5f;
    m_gm.originY = top + (h - top - bottom - rows * cellH) * 0.5f;

    m_particles.EnsureGrid(cols, rows);
    m_visuals.assign(static_cast<size_t>(cols) * rows, CellVisual{});

    // Every session shares the viewport geometry; tell each server. A split
    // tab divides the grid between its two panes (one divider line between).
    auto sizeSession = [](amber::Session& s, int c, int r)
    {
        // Unchanged geometry (density step, font metrics re-check) must not
        // touch the session: a resize request reaches the server as SIGWINCH
        // and full-screen apps answer it with a complete repaint.
        if (s.grid.Cols() == c && s.grid.Rows() == r)
            return;
        if (s.grid.Cols() == 0)
            s.grid.Init(c, r);
        else
            s.grid.Resize(c, r);
        s.ClearSelection();
        if (s.Live())
            s.ssh.RequestResize(c, r);
    };
    for (auto& sp : m_sessions)
    {
        // A single-pane tab keeps the whole grid; anything split is sized
        // from the layout tree, which is what makes a nested split propagate
        // the right column and row count down to each PTY.
        if (sp->layout.Empty() || sp->layout.Count() <= 1)
        {
            sizeSession(*sp, cols, rows);
            continue;
        }
        for (const auto& [id, r] : sp->layout.Rects(cols, rows))
        {
            amber::Session* p = PaneById(*sp, id);
            if (!p)
                continue;
            // A pane hidden by a zoom keeps its last size rather than being
            // resized to nothing: the remote application should not see a
            // 2x2 terminal because the user zoomed a different pane.
            if (r.cols <= 0 || r.rows <= 0)
                continue;
            sizeSession(*p, r.cols, r.rows);
        }
    }

    // Base particle size ≈ 1.3x the glyph-pixel pitch (cellH / 16 grid rows),
    // like the reference's "65% of a pixel" radius — neighbours fuse into
    // strokes. The renderer grows particles when density drops below full.
    // Dot-matrix styles use fewer, deliberately chunky round dots.
    // Dot styles now have a crisp dot core, so their particles are pure glow
    // sized to halo each dot rather than to *be* the dot.
    if (m_fontStyle == 0)
        m_particles.tun.glowSize = std::clamp(cellH * 0.082f, 1.4f, 4.5f);
    else if (m_fontStyle == 1)
        m_particles.tun.glowSize = std::clamp(cellH * 0.10f, 1.7f, 5.0f);
    else
        m_particles.tun.glowSize = std::clamp(cellH * 0.082f, 1.5f, 4.5f);
}

void App::HandleColorSpaceChange()
{
    if (m_device.UpdateColorSpace())
    {
        m_composite.RebuildForBackbuffer();
    }
}

// ---------------------------------------------------------------------- tick
void App::Tick()
{
    if (m_vncSelfCheckRequested || m_vncCheck)
        VncSelfCheckTick();
    // --preview-safety: open both safety boxes once, on the frame after the
    // window is up, with sample content and nothing connected. It exists so
    // the two modals can be reviewed on every interface skin without a server
    // — the same reason --diag exists.
    if (m_previewSafety)
    {
        const int want = m_previewSafety;
        m_previewSafety = 0;
        amber::ShowHostKeyDialog(
            m_hwnd, "sample.example.com",
            "ssh-ed25519 SHA256:uYtM0LrpZ8kX3vQ9dGfJhKlNpRsTwZaBcDeFgHiJkLm",
            want == 2);
        const amber::RiskReport r = amber::AnalyseCommand("sudo rm -rf /var/lib");
        amber::ShowRiskDialog(m_hwnd, r,
                              amber::ConfirmFor(r.level, amber::RiskPolicy::Standard),
                              "sample.example.com");
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (m_minimized)
    {
        // Minimized: zero GPU dispatches; keep draining the network so the
        // ring buffer never backs up.
        PumpSshEvents();
        PumpVncEvents();
        ForEachSession([&](amber::Session& s) { DrainSessionOutput(s, 64); });
        Sleep(30);
        return;
    }

    if (!m_focused)
    {
        // Unfocused: ~10 fps.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = double(now.QuadPart - m_qpcStart.QuadPart) / m_qpcFreq.QuadPart;
        if (t - m_lastFrameTime < 0.1)
        {
            Sleep(5);
            return;
        }
    }
    RenderFrame();

    CheckUserFiles();   // snippets / triggers edited in the editor
    SyncVitals();       // follow the active tab

    // Screensaver: after the idle threshold the screen dissolves into rain;
    // any input ramps it back (the blend is what the shader lerps on).
    {
        bool idle = m_saverSecs > 0 && HasSession() &&
                    (m_time - m_lastInputTime) > static_cast<double>(m_saverSecs);
        float target = idle ? 1.0f : 0.0f;
        float step = std::min(m_dt, 0.05f) * (idle ? 0.6f : 1.8f);
        if (m_rainAmt < target)
            m_rainAmt = std::min(target, m_rainAmt + step);
        else if (m_rainAmt > target)
            m_rainAmt = std::max(target, m_rainAmt - step);
    }

    // CRT power-off finished: do the real close.
    if (m_closingTab >= 0 && m_time - m_closeStart >= kCrtOffSecs)
    {
        int t = m_closingTab;
        m_closingTab = -1;
        CloseSessionNow(t);
    }

    // A prompt icon (PUA glyph) had no coverage in any face this frame —
    // fetch the standalone symbols font once so icons resolve from then on.
    if (m_sampler.PuaGlyphMissing())
    {
        m_sampler.ClearPuaMissing();
        DownloadSymbolsFont();
    }
}

void App::PumpSshEvents()
{
    // Every session is drained, not just the visible one: a background tab
    // (or pane) must keep making progress and must never stall another.
    ForEachSession([&](amber::Session& s)
    {
        // A drop goes to the Guardian, which owns classification, the security
        // gates, the backoff ladder and the retry limit (sessions/Guardian.h).
        // Everything here does is translate its answer into tab state.
        auto onDrop = [&](const std::string& why) -> bool
        {
            NoteInterruptedCommand(s, why);
            // Only the FIRST drop of an outage is annotated. Every failed
            // retry is another drop, and one line per attempt would bury the
            // screen in identical notices.
            const bool firstOfEpisode = !s.guardian.Armed();
            s.guardian.OnDrop(why, m_time);
            const amber::GuardianState g = s.guardian.State();
            if (g == amber::GuardianState::Idle ||
                g == amber::GuardianState::ConnectionLost)
            {
                // Not armed: either the guardian is off, the drop was clean,
                // or this session never connected in the first place.
                if (s.guardian.NeedsConsent())
                {
                    s.state = amber::SessionState::Disconnected;
                    s.status = s.guardian.StatusText(m_time);
                    if (firstOfEpisode)
                        AddNotice(s, 1, "connection lost: " + why);
                    return true;      // the consent prompt runs below
                }
                return false;
            }
            if (amber::GuardianTerminal(g))
            {
                // Authentication or the host key stopped it. The tab reports
                // the real reason; the existing warning flow is untouched.
                s.state = amber::SessionState::Error;
                s.status = s.guardian.StatusText(m_time);
                AddNotice(s, 1, s.status);
                return true;
            }
            s.state = amber::SessionState::Reconnecting;
            s.status = s.guardian.StatusText(m_time);
            if (firstOfEpisode)
                AddNotice(s, 1, "connection lost: " + why);
            return true;
        };

        SshEvent ev;
        while (s.ssh.PollEvent(ev))
        {
            switch (ev.type)
            {
            case SshEventType::Status:
                s.status = ev.text;
                if (s.state == amber::SessionState::Connecting)
                    s.state = amber::SessionState::Authenticating;
                break;

            case SshEventType::HostKeyPrompt:
                s.state = amber::SessionState::VerifyingHost;
                s.hostKeyPending = true;
                s.hostKeyText = ev.text;
                break;

            case SshEventType::Connected:
                s.state = amber::SessionState::Connected;
                s.status.clear();
                s.parser.Reset();
                s.localPending.clear();   // any undrained boot text is moot
                s.everConnected = true;
                s.guardian.OnConnected(m_time);
                if (s.guardian.JustReconnected())
                {
                    s.guardian.ClearJustReconnected();
                    OnSessionRecovered(s);
                }
                if (HasSession() && &s == &Cur())
                    SetWindowTextW(m_hwnd, WideFromUtf8(TitleFor(s)).c_str());
                break;

            case SshEventType::Closed:
                VncOnSshClosed(s);
                if (!onDrop(ev.text.empty() ? "connection closed" : ev.text))
                {
                    s.state = amber::SessionState::Disconnected;
                    s.status = ev.text.empty() ? "connection closed" : ev.text;
                    // Session page: close window on exit.
                    const amber::CloseOnExit coe = s.profile.closeOnExit;
                    if (coe == amber::CloseOnExit::Always ||
                        (coe == amber::CloseOnExit::CleanOnly && s.ssh.CleanClose()))
                        s.closeRequested = true;
                }
                break;

            case SshEventType::ForwardUp:
                // "bound:host:port". A VNC tab tunnelling through this session
                // is waiting for exactly this; every one that asked gets it
                // and keeps it only if it is its own.
                for (auto& vp : m_sessions)
                    if (vp->vnc && vp->vnc->session && vp->vnc->viaProfileId == s.profile.id)
                        vp->vnc->session->OnForwardUp(ev.text);
                break;

            case SshEventType::ClipboardText:
            {
                // An X client put text on the session's clipboard. This runs
                // on the UI thread, which is the only place allowed to touch
                // the Windows clipboard: opening it can block on whichever
                // application currently owns it, and the session worker has a
                // terminal to keep responsive.
                const int mode = s.profile.x11Clipboard;
                bool allow = (mode == 2 || mode == 4);
                if (mode == 1)
                    allow = amber::ShowClipboardDialog(m_hwnd, s.profile.host, false,
                                                       ev.text.size());
                if (allow)
                {
                    SetClipboardText(ev.text);
                    char msg[96];
                    snprintf(msg, sizeof msg, "Clipboard: %zu bytes from the session",
                             ev.text.size());
                    SetStatus(msg);
                }
                break;
            }

            case SshEventType::Error:
                VncOnSshClosed(s);
                if (!onDrop(ev.text))
                {
                    s.state = amber::SessionState::Error;
                    s.status = ev.text;
                    if (s.profile.closeOnExit == amber::CloseOnExit::Always)
                        s.closeRequested = true;
                }
                break;
            }
        }

        // Ask mode: one modal question per loss episode, never per attempt,
        // and never for a drop the guardian already refused on security
        // grounds — those states are terminal and never ask.
        if (s.guardian.NeedsConsent())
        {
            const std::wstring body =
                L"The connection to " + WideFromUtf8(s.label) + L" was lost:\r\n\r\n" +
                WideFromUtf8(s.guardian.LastReason()) +
                L"\r\n\r\nReconnect?\r\n\r\n"
                L"Processes that were running in a plain shell have already "
                L"ended and cannot be recovered.";
            const int answer = MessageBoxW(m_hwnd, body.c_str(),
                                           L"AmberSSH — connection lost",
                                           MB_YESNO | MB_ICONQUESTION);
            s.guardian.GrantConsent(answer == IDYES, m_time);
            if (answer == IDYES)
            {
                s.state = amber::SessionState::Reconnecting;
                s.status = s.guardian.StatusText(m_time);
            }
            else
            {
                s.state = amber::SessionState::Disconnected;
                s.status = "reconnect declined";
            }
        }

        // A host-key decision is a modal, explicit choice — never automatic.
        if (s.hostKeyPending)
        {
            s.hostKeyPending = false;
            // A fingerprint is 43 characters of base64 that nobody compares
            // properly. The sigil turns it into a figure the eye can reject at
            // a glance — and the text is still there, because the figure is a
            // recognition aid, never the evidence.
            //
            // "changed" is the case the whole feature exists for: the wording
            // the transport uses for a mismatch decides the alarm treatment.
            std::string low = s.hostKeyText;
            for (char& c : low)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool changed = low.find("changed") != std::string::npos ||
                                 low.find("mismatch") != std::string::npos ||
                                 low.find("does not match") != std::string::npos;
            const amber::HostKeyChoice choice = amber::ShowHostKeyDialog(
                m_hwnd, s.label, s.hostKeyText, changed);
            const bool answer = choice == amber::HostKeyChoice::Accept;
            s.ssh.AnswerHostKey(answer);
            if (!answer)
            {
                s.state = amber::SessionState::Disconnected;
                s.status = "host key rejected";
            }
        }
    });

    // Close-on-exit requests are honoured outside the iteration (closing a
    // tab mutates the session list); one at a time so the power-off plays.
    if (m_closingTab < 0)
    {
        for (size_t i = 0; i < m_sessions.size(); ++i)
        {
            if (m_sessions[i]->closeRequested)
            {
                m_sessions[i]->closeRequested = false;
                CloseSession(static_cast<int>(i));
                break;
            }
        }
    }
}

// Seed contents for the user's snippet / trigger files (created on first
// "Edit ..." from the menu or palette).
static const char kSnippetsTemplate[] =
    "# AmberSSH snippets — one per line:   name = command\n"
    "# {host} {user} {port} expand from the active session; any other {name}\n"
    "# prompts for a value. A literal \\n sends Enter. Run them from the\n"
    "# command palette (Ctrl+Shift+P, type the name).\n"
    "uptime = uptime\\n\n"
    "disk usage = df -h\\n\n"
    "tail syslog = sudo tail -f /var/log/syslog\\n\n"
    "ssh-copy-id here = ssh-copy-id {user}@{host}\\n\n";

static const char kTriggersTemplate[] =
    "# AmberSSH output triggers — one regular expression per line\n"
    "# (ECMAScript syntax, case-insensitive). When a line of output matches:\n"
    "# the status line shows it, the visual bell rings, and a Windows\n"
    "# notification appears if the window is in the background.\n"
    "\\bERROR\\b\n"
    "BUILD (SUCCESSFUL|FAILED)\n"
    "Permission denied\n";

// Plain-text extraction for output triggers: drops ESC/CSI/OSC/DCS sequences
// and C0 controls except newline/tab. st: 0 text, 1 ESC, 2 CSI, 3 string
// (OSC/DCS/APC until BEL or ST), 4 string-ESC.
static void StripToPlain(std::string& out, const uint8_t* d, size_t n, int& st)
{
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t b = d[i];
        switch (st)
        {
        case 0:
            if (b == 0x1B) st = 1;
            else if (b == '\n' || b == '\t' || b >= 0x20) out.push_back(static_cast<char>(b));
            break;
        case 1:
            if (b == '[') st = 2;
            else if (b == ']' || b == 'P' || b == '_' || b == '^' || b == 'X') st = 3;
            else st = 0;              // two-byte escape
            break;
        case 2:
            if (b >= 0x40 && b <= 0x7E) st = 0;
            break;
        case 3:
            if (b == 0x07) st = 0;
            else if (b == 0x1B) st = 4;
            break;
        case 4:
            st = (b == '\\') ? 0 : 3;
            break;
        }
    }
}

// Base64 → bytes (OSC 52 clipboard payloads). Skips padding/whitespace;
// tolerant of truncation — a hostile payload just yields garbage text.
static std::string DecodeBase64(const std::string& in)
{
    auto val = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int acc = 0, bits = 0;
    for (char ch : in)
    {
        int v = val(ch);
        if (v < 0)
            continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// Masking for anything PERSISTED. Deliberately independent of the screen
// cloak: that toggle is about who can see the window right now, this is about
// what ends up on disk and stays there. A password typed on a command line is
// a durable secret at rest whether or not anyone was screen sharing when it
// was typed, and the roadmap's rule is that raw detected secrets are never
// stored.
//
// Address and home-directory masking stay OFF here — they are not secrets, and
// masking a working directory would gut the journal's usefulness.
static std::string MaskForStorage(const std::string& s)
{
    amber::CloakOptions o;
    o.enabled = true;
    return amber::MaskLine(s, o);
}

// `mask` non-null turns on line-buffered redaction: complete lines are masked
// before they are written and a partial tail is held in `buf` until its
// newline arrives. Masking has to be line-at-a-time because the detectors
// reason about a whole line, and a secret split across two socket reads would
// otherwise slip through in halves.
static void LogFiltered(FILE* f, const uint8_t* d, size_t n, int& st,
                        const amber::CloakOptions* mask = nullptr,
                        std::string* buf = nullptr)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t b = d[i];
        switch (st)
        {
        case 0:
            if (b == 0x1B) st = 1;
            else if (b == '\r') {}                    // CRLF → LF
            else if (b >= 0x20 || b == '\n' || b == '\t')
                out.push_back(static_cast<char>(b));
            break;
        case 1:                                        // after ESC
            if (b == '[') st = 2;
            else if (b == ']') st = 3;
            else st = 0;                               // 2-char sequence
            break;
        case 2:                                        // CSI → final byte
            if (b >= 0x40 && b <= 0x7E) st = 0;
            break;
        case 3:                                        // OSC → BEL or ST
            if (b == 0x07) st = 0;
            else if (b == 0x1B) st = 4;
            break;
        case 4:
            st = (b == '\\') ? 0 : 3;
            break;
        }
    }
    if (out.empty())
        return;
    if (!mask || !buf)
    {
        fwrite(out.data(), 1, out.size(), f);
        return;
    }
    // Line-buffered: mask and emit whole lines, hold the remainder.
    buf->append(out);
    size_t at = 0;
    for (;;)
    {
        const size_t nl = buf->find('\n', at);
        if (nl == std::string::npos)
            break;
        const std::string line = amber::MaskLine(buf->substr(at, nl - at), *mask);
        fwrite(line.data(), 1, line.size(), f);
        fputc('\n', f);
        at = nl + 1;
    }
    buf->erase(0, at);
    // A line that never ends must not grow without bound, and must not be
    // written unmasked to make room. Emit it masked and start again.
    if (buf->size() > 64 * 1024)
    {
        const std::string line = amber::MaskLine(*buf, *mask);
        fwrite(line.data(), 1, line.size(), f);
        buf->clear();
    }
}

void App::DrainSessionOutput(amber::Session& s, int budget)
{
    uint8_t buf[16384];

    const bool activeTab =
        HasSession() && PaneIdOf(Cur(), s) != amber::kNoPane;

    // Terminal page: local echo / line editing "Auto" follow whether the far
    // end echoes (Telnet decides that during option negotiation).
    if (!s.diagnostic)
    {
        const bool remoteEcho = s.ssh.RemoteEcho();
        auto eff = [&](amber::TriState v) {
            return v == amber::TriState::On || (v == amber::TriState::Auto && !remoteEcho);
        };
        s.localEcho = eff(s.profile.localEcho);
        s.lineEditing = eff(s.profile.localLineEdit);
    }

    // Visual bell from the previous drain's BEL.
    if (s.grid.bellPending)
    {
        s.grid.bellPending = false;
        RingBell(s, activeTab);   // Bell page: style, taskbar flash, overload
    }

    auto feed = [&](const uint8_t* d, size_t n)
    {
        if (n > 0 && s.echoPending)
        {
            s.echoPending = false;
            s.echoRttMs = static_cast<float>((m_time - s.echoSentAt) * 1000.0);
            s.echoClosedAt = m_time;
        }
        // Bytes the block is charged with. Counted BEFORE the parser runs,
        // because the C and D marks arrive inside this same stream and the
        // count has to stop at D. This is bytes received between the two
        // marks, escape sequences included — which is what the summary means
        // by a size, and is the only figure AmberSSH can honestly give.
        if (s.cmdRunning)
            s.runningBytes += n;
        s.parser.Feed(d, n);
        if (s.logFile)
        {
            if (s.logRaw)
                fwrite(d, 1, n, s.logFile);          // Logging: all session output
            else
                // Raw logging is byte-exact by definition and cannot be
                // masked without corrupting the escape sequences it exists to
                // preserve; ToggleLogging says so when both are on.
                LogFiltered(s.logFile, d, n, s.logEscState,
                            m_cloak.enabled ? &m_cloak : nullptr,
                            m_cloak.enabled ? &s.logMaskBuf : nullptr);
            if (s.logFlush)
                fflush(s.logFile);
        }
        if (n > 0)
        {
            s.lastOutputAt = m_time;
            if (s.profile.scrollOnOutput)            // Window: reset scrollback on activity
                s.grid.SnapView();
        }
        if (!m_triggers.empty() || m_fxLive)
            ScanTriggers(s, d, n);
        if (s.castFile)
            RecordCast(s, d, n);
    };

    // Local (diagnostic) output honors the cascade pacing too, so the color
    // test screen demonstrates the materialize effects.
    // asciinema playback: release events whose timestamps have arrived.
    if (s.castNext < s.castEvents.size())
    {
        double el = m_time - s.castPlayStart;
        while (s.castNext < s.castEvents.size() &&
               s.castEvents[s.castNext].t <= el)
            s.localPending += s.castEvents[s.castNext++].data;
        if (s.castNext >= s.castEvents.size())
            s.localPending += "\r\n\x1b[2m[amberssh: end of recording]\x1b[0m\r\n";
    }

    // Local queued output (diagnostic screen, boot sequence) drains first.
    if (!s.localPending.empty())
    {
        size_t take = s.localPending.size();
        if (m_fxCascade && activeTab && !m_minimized)
        {
            // The boot sequence (still connecting) types slower than a
            // diagnostic dump so it reads like a POST log, not a blur.
            float rate = (s.state == amber::SessionState::Connected ? 1800.0f
                                                                    : 450.0f) *
                         CurrentSpeed();
            take = std::max<size_t>(
                8, static_cast<size_t>(rate * std::min(m_dt, 0.05f)));
            take = std::min(take, s.localPending.size());
        }
        feed(reinterpret_cast<const uint8_t*>(s.localPending.data()), take);
        s.localPending.erase(0, take);
        return;
    }

    // Cascade reveal: pace the ACTIVE session's output at a baud-style rate
    // so a directory listing visibly materializes letter by letter instead of
    // appearing in one frame (which also mutes the per-letter effects — the
    // burst suppressor sees a full-screen change). Motion Speed scales the
    // rate. Full-screen apps (alternate screen), background tabs, and a
    // minimized window drain at full speed; a growing backlog accelerates,
    // and a huge one abandons pacing entirely so we never fall far behind.
    bool paced = m_fxCascade && activeTab && !s.grid.AltActive() &&
                 !m_minimized;
    if (paced)
    {
        size_t pending = s.ssh.Output().Available();
        if (pending <= 98304)
        {
            // ~1800 chars/sec at Normal — a full line in ~45 ms, so each
            // letter's quarter-second materialize visibly overlaps the next.
            float rate = 1800.0f * CurrentSpeed();      // bytes per second
            if (pending > 16384)
                rate *= 16.0f;                          // catch-up
            size_t chunk = std::max<size_t>(
                16, static_cast<size_t>(rate * std::min(m_dt, 0.05f)));
            while (chunk > 0)
            {
                size_t n = s.ssh.Output().Pop(
                    buf, std::min(chunk, sizeof(buf)));
                if (!n)
                    break;
                feed(buf, n);
                chunk -= n;
            }
            return;
        }
        // fall through: backlog too large, drain everything
    }

    for (int i = 0; i < budget; ++i)
    {
        size_t n = s.ssh.Output().Pop(buf, sizeof(buf));
        if (!n)
            break;
        feed(buf, n);
        if (!activeTab)
            s.unread = true;
    }
}


bool App::NewLocalSession(const std::string& shellKey)
{
    amber::LocalShell sh;
    if (!amber::ResolveShellByKey(shellKey, sh))
    {
        SetStatus(shellKey + " is not installed on this machine.", 5.0);
        return false;
    }
    // An ephemeral profile: a local session is usually a thing you want now,
    // not a thing you name. Saving one is a deliberate act in the manager.
    amber::ConnectionRequest req;
    req.profile.id = amber::MakeUuid();
    req.profile.name = sh.name;
    req.profile.protocol = amber::Protocol::Local;
    req.profile.localShellKey = sh.key;
    req.profile.localShellIntegration = true;
    req.profile.closeOnExit = amber::CloseOnExit::CleanOnly;
    return StartSession(req);
}
bool App::ShowConnectionDialog()
{
    amber::ConnectionRequest req;
    if (!amber::ConnectionDialog::Show(m_hwnd, m_profiles, req))
        return false;
    UpdateJumpList();   // profiles may have been added/renamed in the dialog
    return StartSession(req);
}

bool App::UnlockSecrets()
{
    if (!m_helloUnlock)
        return true;
    static bool verifiedThisRun = false;   // one prompt per app run
    if (verifiedThisRun)
        return true;
    if (!amber::VerifyUserPresence(m_hwnd, L"Unlock AmberSSH stored credentials"))
    {
        SetStatus("Windows Hello verification failed — stored secrets not used.", 6.0);
        return false;
    }
    verifiedThisRun = true;
    return true;
}

// Connect a saved profile without the dialog (taskbar jump list, --connect).
// Remembered secrets come from Credential Manager; a profile that needs a
// password it doesn't have falls back to the dialog.
bool App::ConnectProfileById(const std::string& id)
{
    // The jump list and --connect share one id space; "ws:" names a workspace
    // rather than a profile.
    if (id.rfind("ws:", 0) == 0)
    {
        OpenWorkspace(id.substr(3));
        return true;
    }
    const amber::ConnectionProfile* p = m_profiles.Find(id);
    if (!p)
    {
        SetStatus("Unknown profile id — opening the connection manager.", 5.0);
        return ShowConnectionDialog();
    }
    amber::ConnectionRequest req;
    req.profile = *p;
    if (p->rememberPassword)
    {
        if (!UnlockSecrets())
            return false;
        amber::CredentialStore::Load(p->id, amber::SecretKind::Password, req.password);
    }
    if (p->rememberPassphrase)
    {
        if (!UnlockSecrets())
            return false;
        amber::CredentialStore::Load(p->id, amber::SecretKind::KeyPassphrase,
                                     req.passphrase);
    }
    if (p->rememberProxyPassword)
    {
        if (!UnlockSecrets())
            return false;
        amber::CredentialStore::Load(p->id, amber::SecretKind::ProxyPassword,
                                     req.proxyPassword);
    }
    // Only SSH authenticates up front; Telnet / Rlogin / Raw / Serial log in
    // (if at all) inside the session, so they connect straight away.
    bool needsPw = p->protocol == amber::Protocol::Ssh &&
                   (p->auth == amber::AuthMethod::Password ||
                    p->auth == amber::AuthMethod::KeyboardInteractive);
    if (needsPw && req.password.View().empty())
        return ShowConnectionDialog();
    return StartSession(req);
}

bool App::StartSession(amber::ConnectionRequest& req)
{
    auto session = std::make_unique<amber::Session>();
    session->profile = req.profile;
    {
        const amber::ConnectionProfile& pr = req.profile;
        switch (pr.protocol)
        {
        case amber::Protocol::Local:
            // A local tab is named for its shell. Letting the shell's own title
            // win would put "C:\Windows\System32\cmd.exe" on the tab, which is
            // what cmd sets it to and tells the user nothing.
            session->label = pr.name.empty() ? std::string("local") : pr.name;
            break;
        case amber::Protocol::Serial:
            session->label = pr.serialPort + " @ " + std::to_string(pr.serialBaud);
            break;
        case amber::Protocol::Telnet:
        case amber::Protocol::Raw:
            session->label = pr.username.empty()
                                 ? pr.host + ":" + std::to_string(pr.port)
                                 : pr.username + "@" + pr.host;
            break;
        case amber::Protocol::Vnc:
            // a desktop has no user; the display number is what people know
            session->label = pr.name.empty()
                                 ? pr.host + ":" + std::to_string(pr.port > 0 ? pr.port : 5900)
                                 : pr.name;
            break;
        default:
            session->label = pr.username + "@" + pr.host;
            break;
        }
    }

    // A VNC tab diverges here: no grid to size, no SSH config to build, no
    // shell. It still takes the theme rule and joins the tab list the same way.
    if (req.profile.protocol == amber::Protocol::Vnc)
    {
        session->bornAt = m_time;
        session->grid.Init(req.profile.cols > 0 ? req.profile.cols : 80,
                           req.profile.rows > 0 ? req.profile.rows : 24);
        session->themeOverride = req.profile.themeId >= 0
                                     ? req.profile.themeId
                                     : MatchHostTheme(req.profile.host);
        const bool started = StartVncSession(*session, req);
        req.password.Clear();
        req.passphrase.Clear();
        req.proxyPassword.Clear();
        m_sessions.push_back(std::move(session));
        m_active = static_cast<int>(m_sessions.size()) - 1;
        if (Cur().themeOverride >= 0)
            ApplyTheme();
        return started;
    }
    session->state = amber::SessionState::Connecting;
    session->status = "connecting...";

    int cols = m_gm.cols ? static_cast<int>(m_gm.cols) : req.profile.cols;
    int rows = m_gm.rows ? static_cast<int>(m_gm.rows) : req.profile.rows;
    session->grid.Init(cols, rows);
    session->bornAt = m_time;
    if (m_fxBoot)
        session->localPending = BootSequenceText(req.profile.host);

    SshConfig cfg;
    BuildSshConfig(req.profile, cfg);
    cfg.password = req.password.Reveal();
    cfg.passphrase = req.passphrase.Reveal();
    cfg.proxyPass = req.proxyPassword.Reveal();
    cfg.cols = cols;
    cfg.rows = rows;

    // Parser replies, clipboard, title, OSC 7, images, OSC 133 marks and the
    // remote resize request — all of it in one call. It used to be five
    // lambdas written out here and copied, differently, at three other
    // creation sites; one of them was missing the resize sink.
    BindSessionSinks(*session);
    // Every Connection-dialog page that shapes this session: parser
    // features, scrollback, echo, log file, global font / effect overrides.
    ApplyProfileToSession(*session);
    ApplyProfileGlobals(req.profile);
    OpenProfileLog(*session);

    // Theming: the Colours page theme id wins, then the global per-host
    // rules (first match), while this session is the active tab.
    session->themeOverride = req.profile.themeId >= 0
                                 ? req.profile.themeId
                                 : MatchHostTheme(req.profile.host);

    session->ssh.Start(cfg);

    // Retained (zeroed-on-destruction) for auto-reconnect, pane duplication
    // and the SFTP browser; scrubbed the moment the session closes.
    session->savedPassword.Assign(req.password.View());
    session->savedPassphrase.Assign(req.passphrase.View());
    session->savedProxyPassword.Assign(req.proxyPassword.View());

    // The worker owns its own copy now; scrub every copy we made.
    amber::ScrubString(cfg.password);
    amber::ScrubString(cfg.passphrase);
    amber::ScrubString(cfg.proxyPass);
    req.password.Clear();
    req.passphrase.Clear();
    req.proxyPassword.Clear();

    m_sessions.push_back(std::move(session));
    m_active = static_cast<int>(m_sessions.size()) - 1;
    Cur().ssh.RequestResize(cols, rows);
    if (Cur().themeOverride >= 0)
    {
        ApplyTheme();
        SetStatus("Host theme rule matched — " +
                  Utf8FromWide(kThemes[Cur().themeOverride].name), 5.0);
    }
    return true;
}

void App::CloseSession(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sessions.size()))
        return;
    if (m_closingTab >= 0)
        return;                        // one power-off at a time
    if (index != m_active || m_minimized)
    {
        CloseSessionNow(index);        // background tab: no animation
        return;
    }
    // Active tab: play the CRT collapse, then tear down (see Tick).
    m_closingTab = index;
    m_closeStart = m_time;
}

void App::CloseSessionNow(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sessions.size()))
        return;
    m_sessions[static_cast<size_t>(index)]->ssh.Disconnect();
    if (auto& vt = m_sessions[static_cast<size_t>(index)]->vnc; vt && vt->session)
    {
        // held input up first, then the worker joined, then the GPU
        // resources go with the session — after the frame that used them
        vt->session->Disconnect();
        m_device.WaitIdle();
    }
    m_sessions.erase(m_sessions.begin() + index);

    if (m_sessions.empty())
    {
        m_active = -1;
        // Last tab closed: offer a new connection, otherwise quit.
        if (!ShowConnectionDialog())
            PostQuitMessage(0);
        else
            UpdateGridDims();
        return;
    }
    m_active = std::min(index, static_cast<int>(m_sessions.size()) - 1);
    SelectTab(m_active);
}

void App::SelectTab(int index, int dir)
{
    if (index < 0 || index >= static_cast<int>(m_sessions.size()))
        return;
    // Compiz cube: the current screen rotates away and the new one rotates
    // in as the adjacent face. dir: +1 right / -1 left / 0 = tab order.
    const bool cube = m_fxCube && index != m_active && m_active >= 0 &&
                      m_sessions.size() > 1 && m_scene;
    if (cube)
    {
        m_cubeDir = dir != 0 ? (dir > 0 ? 1 : -1) : (index > m_active ? 1 : -1);
        m_cubePending = true;          // freeze the outgoing frame first
        m_cubeStart = m_time;
    }
    m_active = index;
    Cur().unread = false;
    if (std::clamp(Cur().profile.gapPx, 0, 64) != m_gapPx)
        UpdateGridDims();               // Appearance: per-profile text gap
    ApplyTheme();   // per-host danger theme follows the active tab
    // The new tab's grid must match the current viewport.
    if (Cur().grid.Cols() != static_cast<int>(m_gm.cols) ||
        Cur().grid.Rows() != static_cast<int>(m_gm.rows))
    {
        Cur().grid.Resize(static_cast<int>(m_gm.cols), static_cast<int>(m_gm.rows));
        Cur().ssh.RequestResize(static_cast<int>(m_gm.cols),
                                static_cast<int>(m_gm.rows));
    }
    if (cube)
        m_particles.ResetInstant();    // the arriving face is fully formed
    else
        m_particles.Reset();
    m_cursorTrail.clear();
    m_lastCurPx = m_lastCurPy = -1.0f;
    SetWindowTextW(m_hwnd, WideFromUtf8(TitleFor(Cur())).c_str());
}

void App::CycleTab(int delta)
{
    if (m_sessions.size() < 2)
        return;
    int n = static_cast<int>(m_sessions.size());
    SelectTab(((m_active + delta) % n + n) % n, delta);   // endless, either way
}

void App::SetStatus(const std::string& text, double seconds)
{
    m_status = text;
    m_statusUntil = m_time + seconds;
}

// --------------------------------------------------------------------- frame
void App::RenderFrame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - m_qpcStart.QuadPart) / m_qpcFreq.QuadPart;
    m_dt = static_cast<float>(std::clamp(t - m_lastFrameTime, 0.0001, 0.1));
    m_frameMs = static_cast<float>((t - m_lastFrameTime) * 1000.0);
    m_lastFrameTime = t;
    m_time = static_cast<float>(t);

    m_fpsAccum += m_dt;
    ++m_fpsFrames;
    if (m_fpsAccum >= 0.5)
    {
        m_fps = static_cast<float>(m_fpsFrames / m_fpsAccum);
        m_fpsAccum = 0;
        m_fpsFrames = 0;
    }

    if (t - m_lastColorSpaceCheck > 2.0)
    {
        m_lastColorSpaceCheck = t;
        HandleColorSpaceChange();
    }

    PumpSshEvents();
    PumpVncEvents();

    // Disarm a finished shockwave so blank cells return to the fast path.
    if (m_particles.tun.shockTime >= 0.0f &&
        m_time - m_particles.tun.shockTime > 1.2f)
        m_particles.tun.shockTime = -1.0f;

    // Cap parser work per frame, but drain every session so no tab starves.
    ForEachSession([&](amber::Session& s) { DrainSessionOutput(s, 64); });

    // Session Guardian: one Tick per session per frame. Tick returns true
    // exactly once per attempt, when the backoff has elapsed; the worker
    // thread must have finished first, because Start() joins it and the old
    // thread is what releases this session's forward listeners.
    ForEachSession([&](amber::Session& s)
    {
        if (s.ssh.Running())
            return;
        if (s.guardian.Tick(m_time))
            StartReconnect(s);
        else if (s.guardian.Armed() && s.state == amber::SessionState::Reconnecting)
            s.status = s.guardian.StatusText(m_time);   // count the wait down
        else if (s.guardian.State() == amber::GuardianState::GaveUp &&
                 s.state == amber::SessionState::Reconnecting)
            OnSessionGaveUp(s);
    });

    // ---- CPU compose ----------------------------------------------------
    m_prims.BeginFrame();
    DrawBackground();   // ambient depth field, behind all terminal content
    std::fill(m_visuals.begin(), m_visuals.end(), CellVisual{});
    uint32_t cursorIndex = kNoCursor;

    {
        UpdateFieldEffects();   // must precede compose: it sets the live style
        BuildVisualsFromGrid();
        int cr, cc;
        bool cvis;
        amber::Session& focussed = Foc();
        focussed.grid.CursorViewPos(cr, cc, cvis);
        if (focussed.state == amber::SessionState::Connected && cvis)
        {
            int co = 0, ro = 0;
            PaneOffset(focussed, co, ro);
            cursorIndex =
                static_cast<uint32_t>(cr + ro) * m_gm.cols +
                static_cast<uint32_t>(cc + co);
        }

        // Advance the cursor's border-orbit phase only while it is actually
        // travelling (plus a short grace), so an idle cursor's swirl glides
        // to a stop instead of spinning forever. Phase accumulation keeps the
        // particle positions continuous — no snap on stop or restart.
        if (cursorIndex != m_cursorPrevCell)
        {
            m_cursorPrevCell = cursorIndex;
            m_cursorMovedAt = m_time;
        }
        float spinF = 1.0f -
            std::clamp((m_time - m_cursorMovedAt - 0.8f) / 0.4f, 0.0f, 1.0f);
        m_cursorPhase += m_dt * 0.12f * spinF;
        m_particles.tun.cursorPhase = m_cursorPhase;
        // Eye-candy set (per frame): heat map, RTT-driven cursor ghost,
        // audio wind, screensaver rain blend.
        // Heat map on the main screen; in full-screen apps (alternate screen)
        // the live "diff glow": a hot, short flash on exactly the cells that
        // changed in a redraw, so a ticking htop number or a vim :s edit
        // shows itself.
        if (Foc().grid.AltActive() && m_fxLive)
        {
            m_particles.tun.heatAmp = 2.4f;
            m_particles.tun.heatTau = 0.35f;
        }
        else
        {
            m_particles.tun.heatAmp = m_fxHeat ? 1.6f : 0.0f;
            m_particles.tun.heatTau = 2.5f;
        }
        {
            float ghost = 0.0f;
            if (m_fxGhost && !Cur().diagnostic)
            {
                uint32_t rttUs = Foc().ssh.RttUs();
                ghost = std::clamp(static_cast<float>(rttUs) / 120000.0f, 0.0f, 1.0f);
            }
            m_particles.tun.ghostAmp = ghost;
        }
        if (m_fxAudio && !m_audio.Running())
            m_audio.Start();          // enabled in settings: spin up lazily
        UpdateWeather();
        float audioLvl = m_fxAudio ? std::clamp(m_audio.Level(), 0.0f, 1.0f) : 0.0f;
        m_particles.tun.audioWind = std::max(audioLvl, m_weatherTurb);
        m_particles.tun.rainMode = m_rainAmt;
        m_particles.tun.phosphorDecay = m_fxPersist ? 0.35f : 0.0f;
        m_particles.tun.cursorActivity = spinF;
        // Appearance page: cursor shape / blink follow the active tab.
        m_particles.tun.cursorShape = HasSession() ? static_cast<float>(Cur().profile.cursor) : 0.0f;
        m_particles.tun.cursorBlink = (!HasSession() || Cur().profile.cursorBlink) ? 1.0f : 0.0f;

        if (!Cur().Live())
        {
            std::string msg = "[ " + (Cur().status.empty()
                                          ? std::string("connection closed")
                                          : Cur().status) +
                              " — Ctrl+Shift+T for a new session ]";
            float tw = m_prims.MeasureText(msg, m_sampler);
            m_prims.AddText((m_device.Width() - tw) * 0.5f,
                            m_device.Height() - m_gm.cellH * 2.0f, msg, 0.9f,
                            m_sampler);
        }
        else if (Cur().state != amber::SessionState::Connected)
        {
            std::string msg = "[ " + std::string(SessionStateName(Cur().state)) +
                              (Cur().status.empty() ? "" : ": " + Cur().status) + " ]";
            float tw = m_prims.MeasureText(msg, m_sampler);
            m_prims.AddText((m_device.Width() - tw) * 0.5f,
                            m_device.Height() * 0.5f, msg, 0.9f, m_sampler);
        }

        if (Cur().grid.ViewOffset() > 0)
        {
            char sb[64];
            snprintf(sb, sizeof(sb), "[scrollback %d/%d]",
                     Cur().grid.ViewOffset(), Cur().grid.ScrollbackSize());
            m_prims.AddText(m_gm.originX, m_titleBarH + 2.0f, sb, 0.5f,
                            m_sampler);
        }
    }

    // No panel unless an overlay claims one this frame.
    SetPanelRect(0.0f, 0.0f, 0.0f, 0.0f);
    m_tabBarH = 0.0f;   // tabs now live in the title bar
    DrawTitleBar();
    DrawStatusBar();
    DrawStatusLine();
    DrawLiveEffects();
    DrawNotices();      // guardian annotations: not an effect, always drawn
    DrawBlockGutter();  // command-block bars: metadata, never terminal text
    DrawPaneBadges();   // read-only and broadcast markers, per pane
    DrawPalette();
    DrawJournal();
    DrawRemoteApps();
    DrawPasteGuard();
    UpdateTaskbarProgress();

    // ---- GPU frame ------------------------------------------------------
    EnsureCubeSnapshots();
    ID3D12GraphicsCommandList* cl = m_device.BeginFrame();
    FrameContext& frame = m_device.Frame();
    m_device.Stamp(cl, Device::StampFrameBegin);
    if (m_cubePending)
    {
        // The outgoing session's last frame is still in the scene target:
        // freeze it as the cube's departing face before this frame draws.
        SnapshotForCube(cl);
        m_cubePending = false;
    }

    m_prims.SyncAtlas(cl, frame, m_sampler);
    if (m_chromeReady)
        m_prims.SyncChromeAtlas(cl, frame, m_chromeSampler);
    m_particles.SyncGlyphPoints(m_sampler);

    // The time dial scales the simulation's own clock as well as the style
    // tempo, so slowing it down actually slows the springs rather than just
    // stretching the choreography over the same integration.
    if (VncActive())
        // a desktop tab: upload its damage, run its energy and particle
        // passes; the glyph field is not simulated for a grid nobody draws
        RenderVncPasses(cl, frame);
    else
        m_particles.Simulate(cl, frame, m_visuals, cursorIndex, m_time,
                             m_dt * m_timeDial, m_gm,
                             static_cast<float>(m_device.Width()),
                             static_cast<float>(m_device.Height()),
                             m_device.HdrActive());

    RecordScene(cl);

    // Backbuffer: PRESENT → RT, composite + overlay, RT → PRESENT.
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_device.BackBuffer();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    Composite::Params cp;
    cp.bloomStrength = m_fxBloom ? m_bloomStrength : 0.0f;
    if (!m_fxScanlines)
        cp.scanAmp = 0.0f;
    if (!m_fxVignette)
        cp.vignette = 0.0f;
    cp.time = m_time;
    // Phosphor warm-up: a fresh session fades in from a cold, dim tube over
    // ~1.4 s (the boot sequence text is scrolling meanwhile).
    if (m_fxBoot && HasSession())
    {
        float warm = std::clamp(static_cast<float>((m_time - Cur().bornAt) / 1.4),
                                0.0f, 1.0f);
        warm = warm * warm * (3.0f - 2.0f * warm);
        cp.exposure *= 0.12f + 0.88f * warm;
    }
    // Connection weather: a dropped / reconnecting link goes still and dim.
    if (m_fxLive && HasSession() && !Cur().diagnostic)
    {
        amber::SessionState st = Cur().state;
        if (st == amber::SessionState::Disconnected || st == amber::SessionState::Error ||
            st == amber::SessionState::Reconnecting)
            cp.exposure *= 0.55f;
    }
    // Screen shake: a 0.6 s decaying tremor after a kill/segfault/OOM line.
    {
        float age = static_cast<float>(m_time - m_shakeStart);
        if (age >= 0.0f && age < 0.6f)
        {
            float amp = 7.0f * static_cast<float>(m_dpi) / 96.0f * std::exp(-age * 6.0f);
            cp.shakeX = amp * std::sin(age * 70.0f) / std::max(1.0f, static_cast<float>(m_device.Width()));
            cp.shakeY = amp * 0.6f * std::cos(age * 55.0f) / std::max(1.0f, static_cast<float>(m_device.Height()));
        }
    }
    cp.hdrMode = m_device.HdrActive() ? 1u : 0u;
    cp.maxNits = m_device.MaxNits();
    // Light / paper modes: dark ink shouldn't glow, so drop bloom and ease
    // the scanlines; Paperwhite collapses to greyscale for the e-ink look.
    if (m_appearance == 1 || m_appearance == 2)
    {
        cp.bloomStrength = 0.0f;
        cp.scanAmp = 0.0f;
        cp.vignette = std::min(cp.vignette, 0.015f);
    }
    cp.grayscale = (m_appearance == 2) ? 1.0f : 0.0f;
    // Pixel Art (3): chunky blocks + 16-colour quantize; glow stays on.
    if (m_appearance == 3)
    {
        cp.pixelBlock = std::round(3.0f * static_cast<float>(m_dpi) / 96.0f);
        cp.invResX = 1.0f / std::max(1u, m_device.Width());
        cp.invResY = 1.0f / std::max(1u, m_device.Height());
    }
    // Exit-code flash: decays over ~0.7 s from the moment OSC 133;D landed.
    {
        float age = static_cast<float>(m_time - m_exitFlashStart);
        if (age >= 0.0f && age < 2.0f)
        {
            float k = std::exp(-age * 4.0f);
            cp.flashR = m_exitFlashCol[0];
            cp.flashG = m_exitFlashCol[1];
            cp.flashB = m_exitFlashCol[2];
            cp.flashAmt = m_exitFlashAmt * k;
        }
    }
    // CRT power-off: the active tab is closing — collapse the raster.
    if (m_closingTab == m_active && m_closingTab >= 0)
        cp.crtOff = std::clamp(
            static_cast<float>((m_time - m_closeStart) / kCrtOffSecs),
            0.0f, 1.0f);
    // Compiz cube: departing face = frozen snapshot, arriving face = live.
    {
        constexpr double kCubeSecs = 0.65;
        const double dur = kCubeSecs / std::max(0.25f, CurrentSpeed());
        const double ct = (m_time - m_cubeStart) / dur;
        if (m_fxCube && ct >= 0.0 && ct < 1.0 && m_cubeSnap && !m_cubePending)
        {
            float u = static_cast<float>(ct);
            u = u * u * (3.0f - 2.0f * u);              // ease in-out
            cp.cubeAngle = u * 1.57079633f;
            cp.cubeDir = static_cast<float>(m_cubeDir);
            cp.aspect = static_cast<float>(m_device.Width()) /
                        std::max(1.0f, static_cast<float>(m_device.Height()));
            if (m_appearance == 1 || m_appearance == 2)
            {
                // Paper modes: the void behind the cube is dim paper.
                cp.cubeBgR = 0.86f * 0.35f;
                cp.cubeBgG = 0.84f * 0.35f;
                cp.cubeBgB = 0.78f * 0.35f;
            }
        }
    }
    if (const amber::VncTab* vt = VncActive(); vt && vt->session)
    {
        // a desktop tab: its own composite path (composite.h), and bloom at
        // the profile's glow, not the terminal's
        cp.desktopMode = 1.0f;
        cp.bloomStrength *= static_cast<float>(std::clamp(Cur().profile.vncGlow, 0, 200)) / 100.0f;
        cp.exposure = 1.0f;
    }
    cl->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    m_composite.Record(cl, m_device.BackBufferRTV(),
                       m_device.SrvGpu(m_sceneSrvSlot), m_bloom.ResultSrvGpu(),
                       m_device.SrvGpu(m_cubeSnapSrv),
                       m_device.SrvGpu(m_cubeSnapBloomSrv),
                       cp, m_device.Width(), m_device.Height());


    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_device.BackBuffer();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    m_device.Stamp(cl, Device::StampFrameEnd);
    m_device.EndFrame(m_vsync);
    if (m_vncCheck)
        VncSelfCheckAfterFrame();   // reads the scene target back, synchronously

    UpdateAutoDensity();

    // Optional developer perf log (AMBERSSH_PERFLOG=<file>): metrics only,
    // never terminal content.
    if (!m_perfLogPath.empty() && m_lastFrameTime - m_perfLogLast >= 1.0)
    {
        m_perfLogLast = m_lastFrameTime;
        if (FILE* f = fopen(m_perfLogPath.c_str(), "a"))
        {
            fprintf(f, "%.1f,%ux%u,%s,%u,%.2f,%.2f,%.2f,%.2f\n", m_fps,
                    m_device.Width(), m_device.Height(), DensityLabel(),
                    m_particles.ParticleCount(), m_frameMs,
                    m_device.GpuFrameMs(), m_device.GpuBloomMs(),
                    static_cast<double>(m_particles.DirtyCellsLastFrame()));
            fclose(f);
        }
    }
}

void App::RecordScene(ID3D12GraphicsCommandList* cl)
{
    PixScope pix(cl, L"Scene");

    if (m_sceneState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_scene.Get();
        b.Transition.StateBefore = m_sceneState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        m_sceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_device.RtvCpu(m_sceneRtvSlot);
    // Dark: black. Light & Paperwhite: the same warm off-yellow paper (the
    // composite gives Paperwhite a warm-greyscale cast, so the page stays the
    // easy-on-the-eyes off-yellow). (linear scRGB)
    float clear[4] = { 0, 0, 0, 1 };
    if (m_appearance == 1 || m_appearance == 2)
    { clear[0] = 0.86f; clear[1] = 0.84f; clear[2] = 0.78f; }   // warm paper
    // Dark (0) and Pixel Art (3) clear to black (glow on black).
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);

    D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(m_device.Width()),
                          static_cast<float>(m_device.Height()), 0, 1 };
    D3D12_RECT sc = { 0, 0, static_cast<LONG>(m_device.Width()),
                      static_cast<LONG>(m_device.Height()) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    FrameContext& frame = m_device.Frame();
    D3D12_GPU_VIRTUAL_ADDRESS cb = m_particles.FrameCbGpu();
    if (VncActive())
    {
        // a desktop tab: the terminal's layer order with the particle
        // desktop in place of the glyph field (app_vnc.cpp); the FrameCB it
        // uploads is the one the post-bloom pass below must use too
        cb = DrawVncScene(cl, frame);
    }
    else
    {
    // Order matters for legibility: ANSI backgrounds and the selection
    // gradient first, then the crisp glyph cores (sharp, no blur), then the
    // additive particle field, then UI chrome on top.
    m_prims.RecordUnder(cl, frame, cb);
    // Inline graphics sit directly on the terminal background, UNDER the
    // text: a caption or a prompt drawn over an image has to stay legible,
    // and an image drawn over the text would hide it. This is the layer order
    // Stage 4 specifies — background, graphics, text core, particles,
    // overlays — and it used to run after the particle field.
    DrawInlineImages(cl, frame, cb);
    // Text sharpness: Soft draws the crisp letterform UNDER the particle glow
    // (the original diffuse nixie look); Crisp draws it on top so the glyph
    // reads sharp inside its halo; Razor also skips it here and draws it
    // after the bloom pass (see RenderFrame) so the letter never blooms.
    if (m_sharpness == 0)
        m_prims.RecordCore(cl, frame, cb);
    m_particles.Draw(cl);
    if (m_sharpness == 1)
        m_prims.RecordCore(cl, frame, cb);
    m_prims.RecordColor(cl, frame, cb);   // full-colour emoji over the field
    m_prims.RecordOverBlend(cl, frame, cb);   // darkening panels (palette)
    m_prims.RecordOver(cl, frame, cb);
    }

    // Scene → readable by bloom (compute) and composite (pixel).
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_scene.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        m_sceneState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    m_device.Stamp(cl, Device::StampBloomBegin);
    m_bloom.Record(cl, m_device.SrvGpu(m_sceneSrvSlot));
    m_device.Stamp(cl, Device::StampBloomEnd);

    // Razor: the crisp letterform lands in the scene AFTER bloom sampled it,
    // so the glyph itself never blooms — pin-sharp text inside a halo that
    // still glows like a tube.
    if (m_sharpness == 2)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_scene.Get();
        b.Transition.StateBefore = m_sceneState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        m_prims.RecordCore(cl, frame, cb);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
        m_sceneState = b.Transition.StateAfter;
    }
}

namespace
{
// Miami Sunset gradient (sRGB stops from the visual spec).
const uint32_t kMiamiStops[5] = { 0x34145F, 0x7A1CAC, 0xFF2F92, 0xFF6B6B,
                                  0xFFB347 };
const float kMiamiPos[5] = { 0.0f, 0.24f, 0.48f, 0.73f, 1.0f };

void MiamiAt(float t, float outLinear[3])
{
    t = std::clamp(t, 0.0f, 1.0f);
    int seg = 0;
    while (seg < 3 && t > kMiamiPos[seg + 1])
        ++seg;
    float f = (t - kMiamiPos[seg]) / (kMiamiPos[seg + 1] - kMiamiPos[seg]);
    float a[3], b[3];
    SrgbToLinear(kMiamiStops[seg], a);
    SrgbToLinear(kMiamiStops[seg + 1], b);
    for (int i = 0; i < 3; ++i)
        outLinear[i] = a[i] + (b[i] - a[i]) * f;
}

// Perceived brightness of the gradient position, for contrast-resolved text.
float MiamiLum(float t)
{
    float c[3];
    MiamiAt(t, c);
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}
} // namespace

void App::BuildVisualsFromGrid()
{
    const amber::Palette16& pal = Pal();

    // Compose one session's grid into the window-wide visual array at a cell
    // offset: a full tab composes at 0/0; a split tab composes both panes
    // around a divider line.
    auto composePane = [&](amber::Session& S, int colOff, int rowOff,
                           int paneCols, int paneRows)
    {
    BuildFoldMap(S);
    int selR0 = 0, selC0 = 0, selR1 = -1, selC1 = -1;
    if (S.selActive)
        S.SelectionBounds(selR0, selC0, selR1, selC1);

    // Selection ease-in: 140 ms, then perfectly stable (no drift).
    float selEase = 1.0f;
    if (S.selActive)
        selEase = std::clamp(
            static_cast<float>((m_lastFrameTime - S.selAnimStart) / 0.14), 0.0f,
            1.0f);
    float selAlpha = 0.80f * (selEase * selEase * (3.0f - 2.0f * selEase));

    // One smooth screen-space gradient across the whole selected region: a
    // diagonal sweep over the selection's bounding box (not a per-cell
    // rainbow). Stable while selected — position, not time, drives it.
    bool multiRow = selR1 > selR0;
    float selW = multiRow ? static_cast<float>(paneCols)
                          : static_cast<float>(std::max(1, selC1 - selC0 + 1));
    float selX0 = multiRow ? 0.0f : static_cast<float>(selC0);
    float selRows = static_cast<float>(std::max(1, selR1 - selR0 + 1));
    auto gradPos = [&](float col, int row)
    {
        float u = (col - selX0) / selW;
        float v = (static_cast<float>(row) - selR0) / selRows;
        return std::clamp(0.75f * u + 0.25f * v, 0.0f, 1.0f);
    };

    // Phosphor persistence bookkeeping: remember each cell's glyph from the
    // previous frame so a replaced glyph can leave an afterimage. Scrolls,
    // screen switches, scrollback viewing and resizes change every cell at
    // once — no afterimages for those, just resync.
    const size_t paneCells = static_cast<size_t>(paneCols) * static_cast<size_t>(paneRows);

    // Erase debounce for full-screen apps (alternate screen): only a cell
    // that went BLANK is held (kHoldSecs) before the renderer sees it, so an
    // erase-then-rewrite that straddles frames (a SIGWINCH repaint, vim
    // scrolling) neither flickers nor births particles for unchanged text.
    // New or replaced glyphs commit immediately — no added latency on the
    // content the user is watching. The main screen is unaffected.
    const bool debounce = S.grid.AltActive() && S.grid.ViewOffset() == 0;
    constexpr double kHoldSecs = 0.030;
    if (S.dbLast.size() != paneCells)
    {
        S.dbLast.assign(paneCells, Cell{});
        S.dbCommitted.assign(paneCells, Cell{});
        S.dbSince.assign(paneCells, -1.0);
    }
    auto sameCell = [](const Cell& a, const Cell& b) {
        return a.cp == b.cp && a.fg == b.fg && a.bg == b.bg && a.ul == b.ul &&
               a.attr == b.attr && a.flags == b.flags && a.link == b.link;
    };

    bool persistSkip = !m_fxPersist ||
                       S.grid.AltActive() != S.prevAlt ||
                       S.grid.ViewOffset() != S.prevViewOffset ||
                       S.grid.ViewOffset() != 0 ||
                       S.prevCp.size() != paneCells;
    // A scroll of k lines since last frame: this frame's row r held row
    // r+k's glyph last frame, and existing ghosts ride up with the text.
    // Detected by content, not the history counter, so it also holds for
    // alternate-screen apps and DECSTBM regions (nothing pushed to history):
    // pick the k in 0..3 whose shifted comparison matches the most ink.
    // Full-screen apps never scroll in this sense: a process list re-sorting
    // by one row looks exactly like a one-line scroll to the matcher, and
    // the ghosts would ride up the screen. Ghosts stay put there.
    int persistShift = 0;
    if (!persistSkip && S.prevCp.size() == paneCells && !S.grid.AltActive())
    {
        int best = 0, bestScore = -1;
        for (int k = 0; k <= 3; ++k)
        {
            int match = 0, total = 0;
            for (int r = 0; r + k < paneRows; ++r)
            {
                for (int c = 0; c < paneCols; ++c)
                {
                    char32_t now = S.grid.ViewCell(r, c).cp;
                    char32_t was = S.prevCp[static_cast<size_t>(r + k) * paneCols + c];
                    if ((now == U' ' || now == 0) && (was == U' ' || was == 0))
                        continue;
                    ++total;
                    if (now == was)
                        ++match;
                }
            }
            int score = total ? match * 100 / total : (k == 0 ? 100 : 0);
            if (score > bestScore + (k == 0 ? 0 : 5))   // prefer no-scroll on ties
            {
                bestScore = score;
                best = k;
            }
        }
        persistShift = best;
        if (persistShift > 0)
            for (auto& a : S.afterimages)
                a.y -= static_cast<float>(persistShift) * m_gm.cellH;
    }
    if (S.prevCp.size() != paneCells)
    {
        S.prevCp.assign(paneCells, 0);
        S.prevRgb.assign(paneCells, 0);
    }
    S.prevPushed = S.grid.TotalPushed();
    S.prevAlt = S.grid.AltActive();
    S.prevViewOffset = S.grid.ViewOffset();

    const float defaultBright = 0.80f;   // default amber foreground level
    float ulH = std::max(1.5f, m_fontPx * 0.07f);
    double blinkPhase = std::fmod(m_lastFrameTime, 1.2) < 0.72 ? 1.0 : 0.35;

    // Mothership shadow: during a bulk reveal the incoming ship's shadow
    // creeps down the pane ahead of the front — rows dim before their
    // letters descend, exactly like the city going dark before the hull
    // slides overhead.
    // Spotlight: everything outside the output of the command that is running
    // (or the last one that ran) is dimmed, so the thing you are watching
    // floats clear of the history above it.
    uint64_t spotFrom = 0, spotTo = 0;
    if (m_fxSpotlight && !S.grid.AltActive())
    {
        if (S.outputStartRow > 0)
        {
            spotFrom = S.outputStartRow;
            spotTo = S.grid.TotalPushed() +
                     static_cast<uint64_t>(std::max(0, S.grid.CurY()));
        }
        else
        {
            // The most recent block that actually printed something. One that
            // produced no output has no rows to spotlight.
            for (size_t i = S.blocks.size(); i-- > 0;)
            {
                if (!S.blocks[i].hasOutput)
                    continue;
                spotFrom = S.blocks[i].outputFirst;
                spotTo = S.blocks[i].outputLast;
                break;
            }
        }
    }
    const uint64_t spotBase = S.grid.TotalPushed() -
                              static_cast<uint64_t>(S.grid.ViewOffset());
    auto spotlight = [&](int gridRow) -> float {
        if (spotTo == 0)
            return 1.0f;
        const uint64_t abs = spotBase + static_cast<uint64_t>(gridRow);
        return (abs >= spotFrom && abs <= spotTo) ? 1.0f : 0.30f;
    };

    auto id4Shadow = [&](int gridRow) -> float {
        // Dark mode only: on light paper "dimming" reads as fog, not shadow.
        if (m_particles.tun.animStyle != 11u || m_appearance != 0 ||
            m_particles.tun.reducedMotion)
            return 1.0f;
        float rd = m_particles.WaveRowDelay();
        if (rd <= 0.0f)
            return 1.0f;
        float front = (m_time - m_particles.WaveStart()) / rd;
        float d = static_cast<float>(gridRow) - front;   // rows until arrival
        if (d <= 0.0f || d > 10.0f)
            return 1.0f;
        return 1.0f - 0.72f * (1.0f - d / 10.0f);
    };

    // OSC 8 hover: every cell sharing the link under the pointer is
    // underlined in the accent colour so the whole target reads as one link.
    uint16_t hoverLink = 0;
    {
        int hr, hc;
        if (CellFromPx(m_lastMousePx, m_lastMousePy, hr, hc))
        {
            hr -= rowOff;
            hc -= colOff;
            if (hr >= 0 && hc >= 0 && hr < S.grid.Rows() && hc < S.grid.Cols())
                hoverLink = S.grid.ViewCell(hr, hc).link;
        }
    }

    // Which cells the cloak covers this frame. Computed once for the pane,
    // before anything is emitted, so a masked cell is never drawn first and
    // covered afterwards.
    RebuildCloak(S, paneRows, paneCols);

    for (int r = 0; r < paneRows; ++r)
    {
        if (r + rowOff >= static_cast<int>(m_gm.rows))
            break;
        float cy = m_gm.originY + (r + rowOff) * m_gm.cellH;
        const float shadowF = id4Shadow(r + rowOff) * spotlight(r);
        for (int c = 0; c < paneCols; ++c)
        {
            if (c + colOff >= static_cast<int>(m_gm.cols))
                break;
            // Folding lives here, in the ONE place compose reads a cell: a
            // collapsed block's rows are replaced by its summary line.
            Cell live = FoldedCell(S, r, c);
            const size_t di = static_cast<size_t>(r) * paneCols + static_cast<size_t>(c);
            // Privacy cloak: cover the cell rather than change the grid. On a
            // fixed grid the covered run is still as long as the secret — the
            // status bar says so, and the copy path uses a fixed-width marker
            // instead. Both are honest about what they do.
            if (!S.cloakMask.empty() && di < S.cloakMask.size() && S.cloakMask[di])
            {
                live.cp = U'█';
                live.link = 0;
                live.attr &= static_cast<uint16_t>(~(AttrUnderline | AttrDblUnder));
            }
            if (!sameCell(live, S.dbLast[di]))
            {
                S.dbLast[di] = live;
                S.dbSince[di] = m_time;
            }
            {
                const bool erased = (live.cp == 0 || live.cp == U' ') &&
                                    !(live.flags & CellWideTail) &&
                                    amber::ColIsDefault(live.bg) &&
                                    !(live.attr & AttrInverse);
                if (!debounce || !erased || m_time - S.dbSince[di] >= kHoldSecs)
                    S.dbCommitted[di] = live;
            }
            const Cell& cell = S.dbCommitted[di];
            size_t idx = static_cast<size_t>(r + rowOff) * m_gm.cols +
                         (c + colOff);
            float cx = m_gm.originX + (c + colOff) * m_gm.cellW;
            bool selected = S.CellSelected(r, c);

            // ---- resolve colors (inverse swaps the resolved pair) ---------
            bool fgExplicit = !amber::ColIsDefault(cell.fg);
            uint32_t fgRgb = amber::ResolveCellColor(cell.fg, pal, 0xFFB000);
            bool bgExplicit = !amber::ColIsDefault(cell.bg);
            uint32_t bgRgb = amber::ResolveCellColor(cell.bg, pal, 0x000000);
            if (cell.attr & AttrInverse)
            {
                std::swap(fgRgb, bgRgb);
                std::swap(fgExplicit, bgExplicit);
                // Default fg painted as a bg becomes an amber block; default
                // bg painted as fg becomes near-black text.
                if (!bgExplicit) { bgRgb = 0xFFB000; bgExplicit = true; }
                if (!fgExplicit) { fgRgb = 0x120A00; fgExplicit = true; }
            }

            // ---- explicit ANSI background rectangle -----------------------
            if (bgExplicit && !selected)
            {
                float lin[3];
                SrgbToLinear(bgRgb, lin);
                // Background fills fade out with the screensaver rain.
                float rgba[4] = { lin[0], lin[1], lin[2], 1.0f - m_rainAmt };
                m_prims.AddRectRgba(cx, cy, m_gm.cellW, m_gm.cellH, rgba, 0.0f,
                                    PrimLayer::Under);
            }

            // ---- Miami Sunset selection gradient --------------------------
            float gp = 0.0f;
            if (selected && m_miamiSelection)
            {
                gp = gradPos(static_cast<float>(c), r);
                float gpR = gradPos(static_cast<float>(c) + 1.0f, r);
                float c0[4], c1[4];
                MiamiAt(gp, c0);
                MiamiAt(gpR, c1);
                c0[3] = c1[3] = selAlpha;
                m_prims.AddRectGradient(cx, cy, m_gm.cellW, m_gm.cellH, c0, c1,
                                        r == selR0);
            }
            else if (selected)
            {
                float rgba[4] = { 0.28f, 0.20f, 0.05f, 0.55f };
                m_prims.AddRectRgba(cx, cy, m_gm.cellW, m_gm.cellH, rgba, 0.0f,
                                    PrimLayer::Under);
            }

            // ---- glyph brightness / color ---------------------------------
            float b = 0.0f;
            uint32_t glyph = 0;
            bool concealed = (cell.attr & AttrConceal) != 0;
            bool hasInk = cell.cp != U' ' && cell.cp != 0 && !concealed;
            if (hasInk)
            {
                glyph = m_sampler.GlyphIdFor(cell.cp);
                b = fgExplicit ? 1.0f : defaultBright;
                if ((cell.attr & AttrBold) && S.profile.boldStyle != amber::BoldStyle::Font)
                    b = std::min(1.30f, b * 1.30f + 0.05f);   // Colours: bold = brighter
                if (cell.attr & AttrDim)
                    b *= 0.55f;
                if (cell.attr & AttrBlink)
                    b *= static_cast<float>(blinkPhase);
                b = std::max(b, 0.05f);
            }

            // Selected text: contrast beats color — ivory over the dark half
            // of the gradient, near-black plum over the bright half.
            if (selected && m_miamiSelection && hasInk)
            {
                fgExplicit = true;
                fgRgb = (MiamiLum(gp) > 0.30f) ? 0x2A0A22 : 0xFFF0D6;
                b = std::max(b, 1.0f);
            }

            // Full-colour emoji render through the colour atlas, not the
            // amber particle field: park the particles for that cell.
            // Emoji-default codepoints always; text-default symbols only when
            // the application forced colour with VS16 (and VS15 wins if both).
            bool vs16 = (cell.flags & CellEmojiVS) && !(cell.flags & CellTextVS);
            bool emoji = hasInk && !(cell.flags & CellTextVS) &&
                         (GlyphSampler::IsColorEmoji(cell.cp) ||
                          (vs16 && cell.cp >= 0xA9));   // ©️ is the lowest
            if (emoji)
            {
                glyph = 0;
                b = 0.0f;
            }

            // Bright backgrounds wash out additive particles. With the crisp
            // core on (every font style has one now — DirectWrite letterforms
            // or hard dots), the core carries those cells.
            bool coreOn = m_crispCore;
            float particleB = b * shadowF;
            if ((bgExplicit || (selected && m_miamiSelection)) && hasInk)
                particleB *= coreOn ? 0.30f : 0.65f;
            // Sharper text: the crisp core carries the letterform, so the
            // particle glow steps back to a halo instead of competing.
            if (hasInk && coreOn && m_sharpness > 0)
                particleB *= (m_sharpness == 2) ? 0.55f : 0.75f;

            // ---- crisp glyph core / colour emoji (staged on the birth wave) -
            if (hasInk && (coreOn || emoji))
            {
                CoreGlyphPlan cg;
                cg.cell = static_cast<uint32_t>(idx);
                cg.cp = cell.cp;
                cg.emoji = emoji;
                cg.wide = (cell.flags & CellWideLead) != 0;
                cg.vs16 = vs16;
                cg.x = cx;
                cg.y = cy;
                SrgbToLinear(fgRgb, cg.rgb);
                const bool light = m_appearance != 0;
                if (!fgExplicit)
                {
                    if (light)
                    {
                        // Dark ink on paper (warm near-black; e-ink desaturates).
                        cg.rgb[0] = 0.05f; cg.rgb[1] = 0.045f; cg.rgb[2] = 0.04f;
                        if (cell.attr & AttrDim)
                            for (float& v : cg.rgb) v = v * 0.5f + 0.18f;
                    }
                    else
                    {
                        // Default amber, scaled by the same brightness rules.
                        AmberRampCpu(std::min(b, 1.0f), cg.rgb);
                    }
                }
                else
                {
                    float boost = ((cell.attr & AttrBold) && S.profile.boldStyle != amber::BoldStyle::Font) ? 1.18f : 1.0f;
                    float dimf = (cell.attr & AttrDim) ? 0.55f : 1.0f;
                    for (float& v : cg.rgb)
                        v *= boost * dimf;
                    if (light)
                    {
                        // Keep ANSI hues readable on a light page: cap very
                        // bright colours so they don't wash out.
                        float mx = std::max({ cg.rgb[0], cg.rgb[1], cg.rgb[2] });
                        if (mx > 0.55f)
                            for (float& v : cg.rgb) v *= 0.55f / mx;
                    }
                }
                if (cell.attr & AttrBlink)
                    for (float& v : cg.rgb)
                        v *= static_cast<float>(blinkPhase);
                for (float& v : cg.rgb)
                    v *= shadowF;   // rows under the approaching hull darken
                cg.shear = (cell.attr & AttrItalic) ? m_fontPx * 0.18f : 0.0f;
                m_corePlan.push_back(cg);
            }

            // ---- decoration bars (in the resolved fg/ul color) ------------
            uint16_t deco = cell.attr & (AttrUnderline | AttrDblUnder | AttrStrike);
            if (deco && !concealed)
            {
                uint32_t ulRgb = amber::ColIsDefault(cell.ul)
                                     ? fgRgb
                                     : amber::ResolveCellColor(cell.ul, pal, fgRgb);
                float lin[3];
                if (fgExplicit || !amber::ColIsDefault(cell.ul))
                    SrgbToLinear(ulRgb, lin);
                else
                    AmberRampCpu(std::min(b, 1.0f), lin);
                float rgba[4] = { lin[0], lin[1], lin[2], 0.9f * (1.0f - m_rainAmt) };
                if (cell.attr & (AttrUnderline | AttrDblUnder))
                {
                    m_prims.AddRectRgba(cx, cy + m_gm.cellH * 0.88f, m_gm.cellW,
                                        ulH, rgba, 0.0f, PrimLayer::Under);
                    if (cell.attr & AttrDblUnder)
                        m_prims.AddRectRgba(cx, cy + m_gm.cellH * 0.88f + ulH * 1.8f,
                                            m_gm.cellW, ulH, rgba, 0.0f,
                                            PrimLayer::Under);
                }
                if (cell.attr & AttrStrike)
                    m_prims.AddRectRgba(cx, cy + m_gm.cellH * 0.52f, m_gm.cellW,
                                        ulH, rgba, 0.0f, PrimLayer::Under);
            }

            if (cell.link && cell.link == hoverLink)
            {
                float lin[3];
                AmberRampCpu(0.85f, lin);
                float rgba[4] = { lin[0], lin[1], lin[2], 0.95f };
                m_prims.AddRectRgba(cx, cy + m_gm.cellH * 0.90f, m_gm.cellW, ulH,
                                    rgba, 0.0f, PrimLayer::Over);
            }

            // Phosphor persistence: a glyph that was replaced (or erased)
            // leaves an afterimage that fades under whatever comes next.
            {
                size_t pidx = static_cast<size_t>(r) * paneCols + static_cast<size_t>(c);
                if (pidx < S.prevCp.size())
                {
                    // Last frame's glyph for this screen position (accounting
                    // for any scroll since then).
                    size_t oidx = static_cast<size_t>(r + persistShift) * paneCols +
                                  static_cast<size_t>(c);
                    bool haveOld = (r + persistShift) < paneRows && oidx < S.prevCp.size();
                    char32_t old = haveOld ? S.prevCp[oidx] : 0;
                    uint32_t orgb = haveOld ? S.prevRgb[oidx] : 0;
                    if (!persistSkip && old != cell.cp && old != U' ' && old != 0 &&
                        !(cell.flags & CellWideTail) && S.afterimages.size() < 4000)
                    {
                        amber::Session::After a;
                        a.x = cx;
                        a.y = cy;
                        a.cp = old;
                        a.t = m_time;
                        a.rate = S.grid.AltActive() ? 10.0f : 1.0f;
                        if (m_appearance != 0)
                        { a.rgb[0] = 0.05f; a.rgb[1] = 0.045f; a.rgb[2] = 0.04f; }
                        else if (orgb & 0x1000000u)
                            SrgbToLinear(orgb & 0xFFFFFFu, a.rgb);
                        else
                            AmberRampCpu(defaultBright, a.rgb);
                        S.afterimages.push_back(a);
                    }
                    S.prevCp[pidx] = concealed ? U' ' : cell.cp;
                    S.prevRgb[pidx] = fgExplicit ? (0x1000000u | (fgRgb & 0xFFFFFFu)) : 0u;
                }
            }

            CellVisual v;
            v.glyph = glyph;
            v.bright = particleB;
            v.rgb = fgExplicit ? (fgRgb & 0xFFFFFFu) : 0u;
            // Full-screen apps: in-place glyph replacements morph quietly
            // instead of re-running the motion style per changed digit.
            v.flags = (selected ? kVisSelected : 0u) |
                      (fgExplicit ? kVisColor : 0u) |
                      (S.grid.AltActive() ? kVisQuiet : 0u);
            m_visuals[idx] = v;
        }
    }
    };   // composePane

    amber::Session& P = Cur();
    if (P.layout.Empty() || P.layout.Count() <= 1)
    {
        composePane(P, 0, 0,
                    std::min(P.grid.Cols(), static_cast<int>(m_gm.cols)),
                    std::min(P.grid.Rows(), static_cast<int>(m_gm.rows)));
    }
    else
    {
        const int gc = static_cast<int>(m_gm.cols), gr = static_cast<int>(m_gm.rows);
        for (const auto& [id, r] : P.layout.Rects(gc, gr))
        {
            if (r.cols <= 0 || r.rows <= 0)
                continue;               // hidden by a zoom
            amber::Session* p = PaneById(P, id);
            if (!p)
                continue;
            composePane(*p, r.col, r.row, std::min(p->grid.Cols(), r.cols),
                        std::min(p->grid.Rows(), r.rows));
        }

        // One rule per divider, from the tree, so a nested layout gets the
        // lines it actually has rather than the single line the old
        // one-level model could draw. The focused pane's edges are brighter,
        // which is how the eye finds where input is going.
        float lin[3];
        AmberRampCpu(0.30f, lin);
        for (int r = 0; r < gr; ++r)
            for (int c = 0; c < gc; ++c)
            {
                bool vertical = false;
                amber::PaneId before = amber::kNoPane, after = amber::kNoPane;
                if (!P.layout.DividerAt(c, r, gc, gr, vertical, before, after))
                    continue;
                const bool hot = before == P.focus || after == P.focus;
                float rgba[4] = { lin[0], lin[1], lin[2], hot ? 0.95f : 0.55f };
                const float x = m_gm.originX + static_cast<float>(c) * m_gm.cellW;
                const float y = m_gm.originY + static_cast<float>(r) * m_gm.cellH;
                if (vertical)
                    m_prims.AddRectRgba(x + m_gm.cellW * 0.42f, y,
                                        m_gm.cellW * 0.16f, m_gm.cellH, rgba,
                                        0.0f, PrimLayer::Over);
                else
                    m_prims.AddRectRgba(x, y + m_gm.cellH * 0.42f, m_gm.cellW,
                                        m_gm.cellH * 0.16f, rgba, 0.0f,
                                        PrimLayer::Over);
            }
    }

    // Assign this frame's births (individual / scroll-snap / reveal wave),
    // then emit only the crisp cores whose birth has arrived — the sharp
    // letterform fades in just behind its particles. Births anchor to a
    // FRESH timestamp: composing this frame may have included heavy work
    // (lazy glyph rasterization after a font switch) and anchoring to the
    // frame-start time would let a reveal wave expire before it is ever seen.
    LARGE_INTEGER qnow;
    QueryPerformanceCounter(&qnow);
    float birthNow = static_cast<float>(
        double(qnow.QuadPart - m_qpcStart.QuadPart) / m_qpcFreq.QuadPart);
    m_particles.PlanBirths(m_visuals, birthNow);
    // Nebula Twist rides every fresh character around a small decaying circle
    // (same per-cell phase/decay as the twirl in particle_sim.hlsl); the crisp
    // core must follow that circle or the sharp letter would sit still while
    // its particle cloud orbits it.
    // Phosphor afterimages: drawn first so live letters sit on top of the
    // fading ghosts of what they replaced (~1.2 s, P39-style).
    if (m_fxPersist)
    {
        auto drawAfter = [&](amber::Session& S) {
            auto& list = S.afterimages;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const amber::Session::After& a) {
                                          return (m_time - a.t) * a.rate > 1.4;
                                      }),
                       list.end());
            for (const amber::Session::After& a : list)
            {
                float age = static_cast<float>(m_time - a.t) * a.rate;
                float alpha = 0.85f * std::exp(-age / 0.42f) * (1.0f - m_rainAmt);
                if (alpha < 0.02f)
                    continue;
                m_prims.AddCoreGlyph(a.x, a.y, a.cp, a.rgb, alpha, 0.0f, m_sampler);
            }
        };
        ForEachPane(Cur(), [&](amber::Session& p) { drawAfter(p); });
    }
    else
    {
        ForEachPane(Cur(), [](amber::Session& p) { p.afterimages.clear(); });
    }

    const float twSpeed = std::max(m_particles.tun.effectSpeed, 0.1f);
    for (const CoreGlyphPlan& cg : m_corePlan)
    {
        float birth = m_particles.PlannedBirth(cg.cell);
        // Quiet cell (stationary app refresh): the letter changes where it
        // stands. Its EFFECTIVE style is Direct, so no style-specific core
        // treatment — present or future — applies to it. Inside this loop
        // always branch on `style`, never on tun.animStyle.
        const bool quiet = m_particles.CellQuiet(cg.cell);
        const uint32_t style = (quiet || m_particles.tun.reducedMotion)
                                   ? 0u
                                   : m_particles.tun.animStyle;
        const bool twirl = style == 1u;
        // Styles whose entrance IS the letterform — a tumble, a descent, an
        // iris, a burn, a fragment reveal — must not have a sharp copy of the
        // finished glyph sitting still underneath the effect. Each one names
        // its own hold in kMotionStyles, so a new style only adds a row.
        birth += MotionStyleAt(style).coreDelay / twSpeed;
        if (m_time < birth)
            continue;
        // Under an opaque overlay panel: the particle field is evicted from
        // that rectangle by the shader, and the sharp cores have to go too or
        // the text would still read through the box.
        if (m_panelRect.w > 0.0f && cg.x + m_gm.cellW > m_panelRect.x &&
            cg.x < m_panelRect.x + m_panelRect.w &&
            cg.y + m_gm.cellH > m_panelRect.y &&
            cg.y < m_panelRect.y + m_panelRect.h)
            continue;
        // Stationary apps: the sharp letter is readable at once.
        float reveal = std::min(1.0f, (m_time - birth) / (quiet ? 0.012f : 0.12f) + 0.35f);
        float gx = cg.x, gy = cg.y;
        if (cg.emoji)
        {
            m_prims.AddColorGlyph(gx, gy, cg.cp, 0.95f * reveal * (1.0f - m_rainAmt),
                                  (cg.wide ? 2.0f : 1.0f) * m_gm.cellW,
                                  m_gm.cellH, cg.vs16, m_sampler);
            continue;
        }
        if (twirl)
        {
            float spin = std::clamp(
                (1.8f / twSpeed - (m_time - birth)) / (0.9f / twSpeed),
                0.0f, 1.0f);
            if (spin > 0.0f)
            {
                spin = spin * spin * (3.0f - 2.0f * spin);
                uint32_t n = cg.cell * 7919u + 13u;   // shader HashU mirror
                n = (n << 13u) ^ n;
                n = n * (n * n * 15731u + 789221u) + 1376312589u;
                float ph = float(n & 0x7fffffffu) / float(0x7fffffff) *
                           6.28318530f;
                float w = m_time * 8.0f * twSpeed + ph;
                float r = 0.28f * m_gm.cellH * spin;
                gx += std::cos(w) * r;
                gy += std::sin(w) * r;
            }
        }
        // Light-mode drop shadow: offset dark copies behind the glyph, so
        // dark ink reads against the page. Strength/offset scale with the
        // level; a tighter second tap softens it.
        if (m_appearance != 0 && m_shadowLevel > 0)
        {
            static const float offs[4] = { 0.0f, 0.06f, 0.10f, 0.15f };
            static const float alfa[4] = { 0.0f, 0.35f, 0.50f, 0.66f };
            int lvl = std::clamp(m_shadowLevel, 0, 3);
            float o = offs[lvl] * m_fontPx;
            float a = alfa[lvl] * reveal;
            const float sh[3] = { 0.0f, 0.0f, 0.0f };
            m_prims.AddCoreGlyph(gx + o, gy + o, cg.cp, sh, a, cg.shear,
                                 m_sampler);
            m_prims.AddCoreGlyph(gx + o * 0.5f, gy + o * 0.5f, cg.cp, sh,
                                 a * 0.6f, cg.shear, m_sampler);
        }
        m_prims.AddCoreGlyph(gx, gy, cg.cp, cg.rgb,
                             (m_sharpness ? 1.0f : 0.88f) * reveal * (1.0f - m_rainAmt),
                             cg.shear, m_sampler);
    }
    m_corePlan.clear();
}

void App::DrawBackground()
{
    if (m_bgStyle == 0 || m_minimized)
        return;

    const float W = static_cast<float>(m_device.Width());
    const float H = static_cast<float>(m_device.Height());
    const float top = m_titleBarH;
    const float t = m_time;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;

    // Stateless hashed field — deterministic per particle index, animated by
    // time, so no per-frame storage is needed.
    auto h = [](uint32_t n) {
        n = (n << 13) ^ n;
        n = n * (n * n * 15731u + 789221u) + 1376312589u;
        return static_cast<float>(n & 0x7fffffff) / static_cast<float>(0x7fffffff);
    };
    auto dot = [&](float x, float y, float sz, const float rgb[3], float a) {
        if (y < top - sz || a <= 0.003f)
            return;
        float c[4] = { rgb[0] * a, rgb[1] * a, rgb[2] * a, a };
        m_prims.AddRectRgba(x - sz * 0.5f, y - sz * 0.5f, sz, sz, c, 0.0f,
                            PrimLayer::Under);
    };

    if (m_bgStyle == 1)   // ---- Embers: warm motes rising with a slow sway
    {
        const int N = 130;
        for (int i = 0; i < N; ++i)
        {
            float d = 0.25f + 0.75f * h(i * 2u + 1u);        // depth 0..1
            float speed = 8.0f + 42.0f * d;                  // px/s upward
            float ph = h(i * 5u + 2u) * 6.2831853f;
            float y = H - std::fmod(t * speed + h(i * 3u) * H, H + 40.0f);
            float x = h(i * 7u + 3u) * W + std::sin(t * 0.3f + ph) * 22.0f * d;
            if (x < 0) x += W; else if (x > W) x -= W;
            float rgb[3]; AmberRampCpu(0.55f + 0.35f * d, rgb);
            float tw = 0.6f + 0.4f * std::sin(t * 1.7f + ph);
            dot(x, y, (1.5f + 3.5f * d) * dpi, rgb, (0.10f + 0.26f * d) * tw);
        }
    }
    else if (m_bgStyle == 2)   // ---- Starfield: parallax twinkling stars
    {
        const int N = 200;
        for (int i = 0; i < N; ++i)
        {
            float d = 0.15f + 0.85f * h(i * 2u + 9u);
            float speed = 4.0f + 26.0f * d;
            float x = std::fmod(h(i * 11u + 1u) * W - t * speed, W);
            if (x < 0) x += W;
            float y = top + h(i * 13u + 2u) * (H - top);
            float ph = h(i * 17u + 3u) * 6.2831853f;
            float tw = 0.4f + 0.6f * (0.5f + 0.5f *
                       std::sin(t * (1.0f + 2.0f * d) + ph));
            float rgb[3]; AmberRampCpu(0.6f + 0.4f * d, rgb);
            dot(x, y, (0.8f + 2.4f * d) * dpi, rgb, (0.12f + 0.4f * d) * tw);
        }
    }
    else if (m_bgStyle == 3)   // ---- Cosmic Dust: slow curling themed haze
    {
        const int N = 220;
        for (int i = 0; i < N; ++i)
        {
            float d = 0.1f + 0.9f * h(i * 2u + 4u);
            float bx = h(i * 7u + 1u) * W;
            float by = top + h(i * 11u + 2u) * (H - top);
            float x = bx + std::cos(t * 0.07f * (0.5f + d) +
                                    h(i * 5u) * 6.2831853f) * 34.0f * d;
            float y = by + std::sin(t * 0.05f * (0.5f + d) +
                                    h(i * 9u) * 6.2831853f) * 26.0f * d;
            int si = (i % 3 == 0) ? 1 : (i % 3 == 1) ? 2 : 3;
            float rgb[3] = { gThemeStops[si][0], gThemeStops[si][1],
                             gThemeStops[si][2] };
            float pulse = 0.6f + 0.4f * std::sin(t * 0.8f + h(i) * 6.2831853f);
            dot(x, y, (1.0f + 2.6f * d) * dpi, rgb, (0.07f + 0.20f * d) * pulse);
        }
    }
}

float App::TitleBarH() const
{
    return m_fullscreen ? 0.0f
                        : std::round(34.0f * static_cast<float>(m_dpi) / 96.0f);
}

void App::CaptionLayout(RECT& menu, RECT& mn, RECT& mx, RECT& cl) const
{
    int W = static_cast<int>(m_device.Width());
    int th = static_cast<int>(m_titleBarH);
    int bw = MulDiv(46, static_cast<int>(m_dpi), 96);
    menu = { 0, 0, MulDiv(48, static_cast<int>(m_dpi), 96), th };
    cl = { W - bw, 0, W, th };
    mx = { W - 2 * bw, 0, W - bw, th };
    mn = { W - 3 * bw, 0, W - 2 * bw, th };
}

App::CapZone App::CaptionZoneAt(int px, int py) const
{
    if (m_titleBarH <= 0.0f || py < 0 || py >= static_cast<int>(m_titleBarH))
        return CapZone::None;
    RECT menu, mn, mx, cl;
    CaptionLayout(menu, mn, mx, cl);
    auto in = [&](const RECT& r) { return px >= r.left && px < r.right; };
    if (in(menu)) return CapZone::Menu;
    if (in(cl))   return CapZone::Close;
    if (in(mx))   return CapZone::Max;
    if (in(mn))   return CapZone::Min;
    if (px >= m_plusRect.left && px < m_plusRect.right &&
        py >= m_plusRect.top && py < m_plusRect.bottom)
        return CapZone::Plus;
    return CapZone::None;   // the draggable strip
}

int App::TitleTabAt(int px, int py, bool& closeHit) const
{
    closeHit = false;
    if (m_titleBarH <= 0.0f || py < 0 || py >= static_cast<int>(m_titleBarH))
        return -1;
    for (size_t i = 0; i < m_tabRects.size(); ++i)
    {
        const TabRect& t = m_tabRects[i];
        if (px >= t.x && px < t.x + t.w)
        {
            float cw = 20.0f * static_cast<float>(m_dpi) / 96.0f;
            closeHit = m_sessions.size() > 1 && px >= t.x + t.w - cw;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void App::CaptionClick(CapZone z)
{
    switch (z)
    {
    case CapZone::Menu:  OpenAppMenu(); break;
    case CapZone::Plus:
        if (ShowConnectionDialog())
            UpdateGridDims();
        break;
    case CapZone::Min:   ShowWindow(m_hwnd, SW_MINIMIZE); break;
    case CapZone::Max:   ToggleMaximize(); break;
    case CapZone::Close: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

void App::ToggleMaximize()
{
    if (m_fakeMax)
    {
        m_fakeMax = false;
        SetWindowPos(m_hwnd, nullptr, m_restoreRect.left, m_restoreRect.top,
                     m_restoreRect.right - m_restoreRect.left,
                     m_restoreRect.bottom - m_restoreRect.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        GetWindowRect(m_hwnd, &m_restoreRect);
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        RECT wa = mi.rcWork;   // excludes the taskbar, like a real maximize
        m_fakeMax = true;
        SetWindowPos(m_hwnd, nullptr, wa.left, wa.top, wa.right - wa.left,
                     wa.bottom - wa.top, SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void App::DrawTitleBarClassic()
{
    m_titleBarH = TitleBarH();
    if (m_titleBarH <= 0.0f)
        return;

    const float W = static_cast<float>(m_device.Width());
    const float th = m_titleBarH;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;

    auto stop = [](int i, float s, float out[4]) {
        out[0] = gThemeStops[i][0] * s;
        out[1] = gThemeStops[i][1] * s;
        out[2] = gThemeStops[i][2] * s;
        out[3] = 1.0f;
    };

    // Strip + bottom hairline.
    float bar[4];  stop(1, 0.50f, bar);
    m_prims.AddRectRgba(0, 0, W, th, bar, 0.0f, PrimLayer::Under);
    float hair[4]; stop(2, 0.55f, hair); hair[3] = 0.9f;
    float hh = std::max(1.0f, dpi);
    m_prims.AddRectRgba(0, th - hh, W, hh, hair, 0.0f, PrimLayer::Over);

    RECT menu, mn, mx, cl;
    CaptionLayout(menu, mn, mx, cl);
    DrawVitals(static_cast<float>(mn.left) - 12.0f * dpi);

    // Hover wash behind the focused control (close glows red).
    auto wash = [&](const RECT& r, CapZone z) {
        if (m_capHover != z)
            return;
        float w[4];
        if (z == CapZone::Close)
        {
            float lc[3];
            SrgbToLinear(0xE0403A, lc);
            w[0] = lc[0]; w[1] = lc[1]; w[2] = lc[2]; w[3] = 0.92f;
        }
        else
            stop(2, 0.55f, w), w[3] = 0.45f;
        m_prims.AddRectRgba(static_cast<float>(r.left), 0.0f,
                            static_cast<float>(r.right - r.left), th, w, 0.0f,
                            PrimLayer::Under);
    };
    wash(menu, CapZone::Menu);
    wash(mn, CapZone::Min);
    wash(mx, CapZone::Max);
    wash(cl, CapZone::Close);

    float ic[4]; stop(3, 1.0f, ic);
    float bt = std::max(1.5f, dpi * 1.6f);

    // Hamburger — three bars.
    float cxm = (menu.left + menu.right) * 0.5f;
    float iw = 16.0f * dpi;
    for (int k = -1; k <= 1; ++k)
        m_prims.AddRectRgba(cxm - iw * 0.5f,
                            th * 0.5f + k * (bt * 2.6f) - bt * 0.5f, iw, bt, ic,
                            0.0f, PrimLayer::Over);

    // -------- session tabs + new-session (+) button --------------------------
    m_tabRects.clear();
    const float gap = 6.0f * dpi;
    const float pad = 12.0f * dpi;
    const float plusW = 30.0f * dpi;
    const float tabTop = 5.0f * dpi;
    const float tabH = th - 9.0f * dpi;
    const float rightLimit = static_cast<float>(mn.left) - 8.0f * dpi;
    const float textY = (th - m_gm.cellH * 0.8f) * 0.5f;
    const float closeW = 20.0f * dpi;
    float x = static_cast<float>(menu.right) + gap;
    const float maxTabW = 220.0f * dpi;

    for (size_t i = 0; i < m_sessions.size(); ++i)
    {
        const amber::Session& s = *m_sessions[i];
        bool activeTab = (static_cast<int>(i) == m_active);
        std::string caption = s.Caption();
        if (s.unread && !activeTab)
            caption = "* " + caption;
        if (!s.Live())
            caption += " (closed)";

        float textW = m_prims.MeasureText(caption, m_sampler);
        float wNeed = textW + pad * 2.0f +
                      (m_sessions.size() > 1 ? closeW : 0.0f);
        float w = std::min(maxTabW, wNeed);
        if (x + w > rightLimit - plusW - gap)
            break;   // out of room — remaining tabs are reachable via Ctrl+Tab

        // Tab background: the active tab lifts and carries a bright underline.
        float tb[4];
        stop(activeTab ? 2 : 1, activeTab ? 0.85f : 0.55f, tb);
        tb[3] = activeTab ? 0.95f : (m_tabHover == static_cast<int>(i) ? 0.7f
                                                                       : 0.5f);
        m_prims.AddRectRgba(x, tabTop, w, tabH, tb, 0.0f, PrimLayer::Under);
        if (activeTab)
        {
            float ul[4]; stop(3, 1.0f, ul);
            m_prims.AddRectRgba(x, tabTop + tabH - 2.0f * dpi, w, 2.0f * dpi, ul,
                                0.0f, PrimLayer::Over);
        }

        float intensity = activeTab ? 1.0f : (s.Live() ? 0.7f : 0.4f);
        // Clip the caption to the tab: long "user@host" labels must never
        // run under the close × or into the next tab.
        {
            float avail = w - pad * 2.0f - (m_sessions.size() > 1 ? closeW : 0.0f);
            if (textW > avail && !caption.empty())
            {
                std::string cut = caption;
                while (!cut.empty() &&
                       m_prims.MeasureText(cut + "\xE2\x80\xA6", m_sampler) > avail)
                {
                    cut.pop_back();
                    while (!cut.empty() &&
                           (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
                        cut.pop_back();
                }
                caption = cut + "\xE2\x80\xA6";
            }
        }
        m_prims.AddText(x + pad, textY, caption, intensity, m_sampler);

        // Activity spark on background tabs: flickers while that session is
        // producing output, burns red for a few seconds after a trigger.
        if (!activeTab && m_fxLive)
        {
            float glow = std::exp(-static_cast<float>(m_time - s.lastOutputAt) / 1.5f);
            bool trig = (m_time - s.lastTriggerAt) < 5.0;
            if (glow > 0.03f || trig)
            {
                float sx = x + w - (m_sessions.size() > 1 ? closeW : 0.0f) - 7.0f * dpi;
                float sy = tabTop + tabH * 0.5f;
                float flick = 0.7f + 0.3f * std::sin(static_cast<float>(m_time) * 17.0f +
                                                     static_cast<float>(i) * 2.1f);
                float sz = (2.5f + 2.5f * glow) * dpi;
                float sc[4];
                if (trig)
                { sc[0] = 1.0f; sc[1] = 0.25f; sc[2] = 0.08f; sc[3] = 0.95f * flick; }
                else
                { sc[0] = ic[0]; sc[1] = ic[1]; sc[2] = ic[2]; sc[3] = glow * flick; }
                m_prims.AddRectRgba(sx - sz * 0.5f, sy - sz * 0.5f, sz, sz, sc, 0.0f,
                                    PrimLayer::Over);
            }
        }

        // Per-tab close ×, when more than one session is open.
        if (m_sessions.size() > 1)
        {
            float cxX = x + w - closeW * 0.5f;
            float hx = 4.0f * dpi;
            float xc[4];
            if (m_tabHover == static_cast<int>(i))
            { xc[0] = xc[1] = xc[2] = 1.0f; xc[3] = 1.0f; }
            else { xc[0] = ic[0]; xc[1] = ic[1]; xc[2] = ic[2]; xc[3] = 0.7f; }
            auto seg = [&](float x0, float y0, float x1, float y1) {
                int steps = std::max(3, static_cast<int>(std::abs(x1 - x0)));
                for (int k = 0; k <= steps; ++k)
                {
                    float t = static_cast<float>(k) / steps;
                    m_prims.AddRectRgba(x0 + (x1 - x0) * t - bt * 0.5f,
                                        y0 + (y1 - y0) * t - bt * 0.5f, bt, bt,
                                        xc, 0.0f, PrimLayer::Over);
                }
            };
            float my = th * 0.5f;
            seg(cxX - hx, my - hx, cxX + hx, my + hx);
            seg(cxX - hx, my + hx, cxX + hx, my - hx);
        }

        m_tabRects.push_back({ x, w });
        x += w + gap;
    }

    // New-session (+) button after the last visible tab.
    {
        m_plusRect = { static_cast<LONG>(x), static_cast<LONG>(tabTop),
                       static_cast<LONG>(x + plusW),
                       static_cast<LONG>(tabTop + tabH) };
        if (m_capHover == CapZone::Plus)
        {
            float pb[4]; stop(2, 0.55f, pb); pb[3] = 0.5f;
            m_prims.AddRectRgba(x, tabTop, plusW, tabH, pb, 0.0f,
                                PrimLayer::Under);
        }
        float pcx = x + plusW * 0.5f, pcy = th * 0.5f, pl = 6.0f * dpi;
        m_prims.AddRectRgba(pcx - pl, pcy - bt * 0.5f, pl * 2.0f, bt, ic, 0.0f,
                            PrimLayer::Over);
        m_prims.AddRectRgba(pcx - bt * 0.5f, pcy - pl, bt, pl * 2.0f, ic, 0.0f,
                            PrimLayer::Over);
    }

    // Minimize — one bar.
    float cxn = (mn.left + mn.right) * 0.5f;
    m_prims.AddRectRgba(cxn - 6.0f * dpi, th * 0.5f - bt * 0.5f, 12.0f * dpi, bt,
                        ic, 0.0f, PrimLayer::Over);

    // Maximize (outlined square) / restore (two offset squares).
    float cxx = (mx.left + mx.right) * 0.5f;
    float sq = 11.0f * dpi;
    float bord = std::max(1.0f, dpi * 1.3f);
    if (m_fakeMax)
    {
        float o = 2.5f * dpi;
        m_prims.AddRectRgba(cxx - sq * 0.5f + o, th * 0.5f - sq * 0.5f - o, sq,
                            sq, ic, bord, PrimLayer::Over);   // back square
        // Front square overpaints its overlap with the strip colour.
        m_prims.AddRectRgba(cxx - sq * 0.5f - o, th * 0.5f - sq * 0.5f + o, sq,
                            sq, bar, 0.0f, PrimLayer::Under);
        m_prims.AddRectRgba(cxx - sq * 0.5f - o, th * 0.5f - sq * 0.5f + o, sq,
                            sq, ic, bord, PrimLayer::Over);
    }
    else
    {
        m_prims.AddRectRgba(cxx - sq * 0.5f, th * 0.5f - sq * 0.5f, sq, sq, ic,
                            bord, PrimLayer::Over);
    }

    // Close — an X of small overlapping squares (prims are axis-aligned).
    float xcol[4];
    if (m_capHover == CapZone::Close)
    { xcol[0] = xcol[1] = xcol[2] = 1.0f; xcol[3] = 1.0f; }
    else
    { xcol[0] = ic[0]; xcol[1] = ic[1]; xcol[2] = ic[2]; xcol[3] = 1.0f; }
    float cxc = (cl.left + cl.right) * 0.5f;
    float half = 6.0f * dpi;
    auto line = [&](float x0, float y0, float x1, float y1) {
        int steps = std::max(4, static_cast<int>(std::abs(x1 - x0)));
        for (int i = 0; i <= steps; ++i)
        {
            float t = static_cast<float>(i) / steps;
            float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
            m_prims.AddRectRgba(x - bt * 0.5f, y - bt * 0.5f, bt, bt, xcol, 0.0f,
                                PrimLayer::Over);
        }
    };
    line(cxc - half, th * 0.5f - half, cxc + half, th * 0.5f + half);
    line(cxc - half, th * 0.5f + half, cxc + half, th * 0.5f - half);
}

// ------------------------------------------------------------ interface skin
// The title strip is drawn per skin (ui/Chrome.h). Classic is the original
// theme-coloured strip; every other skin goes through the shape-language
// renderer below, so a new skin is a ChromeSpec entry, not new drawing code.
void App::DrawTitleBar()
{
    if (amber::Chrome().useThemeAccent && amber::Chrome().tabSlant <= 0.0f)
        DrawTitleBarClassic();
    else
        DrawTitleBarCyber();
}

// Hamburger / minimize / maximize-restore / close glyphs in colour ic; bar
// is the strip ground (the restore icon overpaints its overlap with it).
void App::DrawCaptionGlyphs(const RECT& menu, const RECT& mn, const RECT& mx,
                            const RECT& cl, const float icIn[4], const float bar[4],
                            float th, float dpi, float bt, bool dark)
{
    const amber::ChromeSpec& ch = amber::Chrome();
    // Dark glyphs (LCARS: black on coloured segments) go through the
    // premultiplied-over layer; light ones are additive neon.
    const PrimLayer L = dark ? PrimLayer::OverBlend : PrimLayer::Over;
    float ic[4] = { icIn[0], icIn[1], icIn[2], icIn[3] };
    if (dark && !amber::Chrome().rehaut && !amber::Chrome().impression)
    { ic[0] = ic[1] = ic[2] = 0.0f; ic[3] = 1.0f; }
    float cxm = (menu.left + menu.right) * 0.5f;
    float iw = 16.0f * dpi;
    for (int k = -1; k <= 1; ++k)
        m_prims.AddRectRgba(cxm - iw * 0.5f,
                            th * 0.5f + k * (bt * 2.6f) - bt * 0.5f, iw, bt, ic,
                            0.0f, L);
    float cxn = (mn.left + mn.right) * 0.5f;
    m_prims.AddRectRgba(cxn - 6.0f * dpi, th * 0.5f - bt * 0.5f, 12.0f * dpi, bt,
                        ic, 0.0f, L);
    float cxx = (mx.left + mx.right) * 0.5f;
    float sq = 11.0f * dpi;
    float bord = std::max(1.0f, dpi * 1.3f);
    if (m_fakeMax)
    {
        float o = 2.5f * dpi;
        m_prims.AddRectRgba(cxx - sq * 0.5f + o, th * 0.5f - sq * 0.5f - o, sq,
                            sq, ic, bord, L);
        if (!dark)
            m_prims.AddRectRgba(cxx - sq * 0.5f - o, th * 0.5f - sq * 0.5f + o, sq,
                                sq, bar, 0.0f, PrimLayer::Under);
        m_prims.AddRectRgba(cxx - sq * 0.5f - o, th * 0.5f - sq * 0.5f + o, sq,
                            sq, ic, bord, L);
    }
    else
        m_prims.AddRectRgba(cxx - sq * 0.5f, th * 0.5f - sq * 0.5f, sq, sq, ic,
                            bord, L);
    float xcol[4];
    if (dark && !amber::Chrome().rehaut && !amber::Chrome().impression)
    { xcol[0] = xcol[1] = xcol[2] = 0.0f; xcol[3] = 1.0f; }
    else if (m_capHover == CapZone::Close)
    {
        if (ch.useThemeAccent)
        { xcol[0] = xcol[1] = xcol[2] = 1.0f; }
        else
            SrgbToLinear(0xFFFFFF, xcol);
        xcol[3] = 1.0f;
    }
    else
    { xcol[0] = ic[0]; xcol[1] = ic[1]; xcol[2] = ic[2]; xcol[3] = 1.0f; }
    float cxc = (cl.left + cl.right) * 0.5f;
    float half = 6.0f * dpi;
    auto line = [&](float x0, float y0, float x1, float y1) {
        int steps = std::max(4, static_cast<int>(std::abs(x1 - x0)));
        for (int i = 0; i <= steps; ++i)
        {
            float t = static_cast<float>(i) / steps;
            float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
            m_prims.AddRectRgba(x - bt * 0.5f, y - bt * 0.5f, bt, bt, xcol, 0.0f, L);
        }
    };
    line(cxc - half, th * 0.5f - half, cxc + half, th * 0.5f + half);
    line(cxc - half, th * 0.5f + half, cxc + half, th * 0.5f - half);
}

// Chrome label text in the skin's own case. ASCII-only on purpose: session
// captions carry host names and paths, and upcasing non-ASCII bytes here would
// corrupt UTF-8 sequences.
std::string App::SkinCase(const std::string& s)
{
    if (!amber::Chrome().uppercase)
        return s;
    std::string out = s;
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x80)
            c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return out;
}

float App::SkinText(float x, float y, const std::string& s, const float rgb[3],
                    uint32_t onFill)
{
    if (amber::Chrome().darkText)
    {
        // Black, unless the caller says what the text sits on — then the ink
        // is chosen by that fill's luma, so rhodium lands on a dark index and
        // the shade on lume.
        float k[3] = { 0.0f, 0.0f, 0.0f };
        if (onFill)
        {
            const COLORREF ink = amber::InkOn(onFill);
            SrgbToLinear((GetRValue(ink) << 16) | (GetGValue(ink) << 8) | GetBValue(ink), k);
        }
        else if (amber::Chrome().lightGround)
        {
            // A light ground prints in its own ink — Letterpress in oxblood,
            // Reference in the engraving colour — rather than in black.
            k[0] = rgb[0]; k[1] = rgb[1]; k[2] = rgb[2];
        }
        return m_prims.AddTextCore(x, y, s, k, 1.0f, m_chromeSampler, true);
    }
    return m_prims.AddTextRgb(x, y, s, rgb, m_chromeSampler, true);
}

// Skinned strip: neon rails, scanlines, leaning tabs with index tags and a
// magenta rail under the active one, chamfered hover washes, HUD ticks.
void App::DrawTitleBarCyber()
{
    m_titleBarH = TitleBarH();
    if (m_titleBarH <= 0.0f)
        return;
    const amber::ChromeSpec& ch = amber::Chrome();
    const float W = static_cast<float>(m_device.Width());
    const float th = m_titleBarH;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    auto lin = [](uint32_t srgb, float a, float out[4]) {
        float c[3];
        SrgbToLinear(srgb, c);
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = a;
    };
    const float hh = std::max(1.0f, dpi);

    // Ground + scanlines.
    float bg[4];
    lin(ch.bg, 1.0f, bg);
    m_prims.AddRectRgba(0, 0, W, th, bg, 0.0f, PrimLayer::Under);
    if (ch.scanlines)
    {
        float sl[4];
        lin(ch.neonA, 0.05f, sl);
        for (float fy : { 0.22f, 0.44f, 0.66f })
            m_prims.AddRectRgba(0, std::floor(th * fy), W, hh, sl, 0.0f, PrimLayer::Over);
    }
    // Reference: the plate is brushed — a hairline every 2px, a few percent
    // over it. Tenmoku: the glaze thins to rust along the strip's top edge
    // and carries oil-spot flecks, seeded by position so they never crawl.
    if (ch.meter)
    {
        float br[4];
        lin(0xFFFFFF, 0.06f, br);
        for (float fy = 1.0f; fy < th - 3.0f; fy += 2.0f * dpi)
            m_prims.AddRectRgba(0, fy, W, hh, br, 0.0f, PrimLayer::OverBlend);
    }
    if (ch.glaze)
    {
        float rim[4], spot[4];
        lin(0x7A3B1E, 0.9f, rim);
        lin(0x7FA0B8, 0.85f, spot);
        m_prims.AddRectRgba(0, 0, W, 2.0f * dpi, rim, 0.0f, PrimLayer::OverBlend);
        const float cell = 40.0f * dpi;
        for (float cx = 0.0f; cx < W; cx += cell)
        {
            const uint32_t hsh = static_cast<uint32_t>(cx / cell) * 2654435761u;
            if ((hsh >> 8) % 100 < 45)
            {
                const float sx = cx + (hsh % 37) * cell / 37.0f;
                const float sy = 4.0f * dpi + ((hsh >> 12) % 23) * (th - 10.0f * dpi) / 23.0f;
                const float sz = ((hsh >> 20) & 1 ? 2.0f : 1.4f) * dpi;
                m_prims.AddRectRgba(sx, sy, sz, sz, spot, 0.0f, PrimLayer::OverBlend);
            }
        }
    }
    // Bottom rail: neon hairline with a soft glow above it. Horologe runs the
    // rehaut minute track here instead — a tick every T, a long one every
    // fifth, rising from a shade line at the strip's foot.
    float na[4];
    lin(ch.neonA, 0.95f, na);
    if (ch.rehaut)
    {
        float tk[4], lt[4], sh[4];
        lin(ch.textDim, 1.0f, tk);
        lin(ch.text, 1.0f, lt);
        lin(ch.field, 1.0f, sh);
        m_prims.AddRectRgba(0, th - hh, W, hh, sh, 0.0f, PrimLayer::Over);
        const float T = 6.0f * dpi;
        int i = 0;
        for (float tx = 0.0f; tx <= W; tx += T, ++i)
        {
            const bool lng = (i % 5) == 0;
            const float h = (lng ? 7.0f : 4.0f) * dpi;
            m_prims.AddRectRgba(tx, th - hh - h, hh, h, lng ? lt : tk, 0.0f,
                                PrimLayer::Over);
        }
    }
    else if (ch.impression)
    {
        // Letterpress: a printer's thick-thin rule, in the ink.
        float ink[4];
        lin(ch.neonA, 1.0f, ink);
        m_prims.AddRectRgba(0, th - 6.0f * dpi, W, 3.0f * dpi, ink, 0.0f, PrimLayer::OverBlend);
        m_prims.AddRectRgba(0, th - 1.0f * dpi, W, 1.0f * dpi, ink, 0.0f, PrimLayer::OverBlend);
    }
    else if (ch.stitch)
    {
        // Atelier: a seam — the painted edge with a stitch line either side.
        float paint[4], thread[4];
        lin(ch.border, 1.0f, paint);
        lin(ch.neonA, 1.0f, thread);
        m_prims.AddRectRgba(0, th - 4.0f * dpi, W, 2.0f * dpi, paint, 0.0f, PrimLayer::OverBlend);
        const float L = 6.0f * dpi, G = 4.0f * dpi, t = 1.5f * dpi;
        for (float sx = 2.0f * dpi; sx + L < W; sx += L + G)
        {
            m_prims.AddShapeRgba(sx, th - 7.0f * dpi, L, t, thread, 0.0f, PrimLayer::OverBlend, 3, 1.3f * dpi);
            m_prims.AddShapeRgba(sx, th - 1.5f * dpi, L, t, thread, 0.0f, PrimLayer::OverBlend, 3, 1.3f * dpi);
        }
    }
    else if (ch.meter)
    {
        // Reference: the plate's lower edge — a shade, then a highlight —
        // before the black glass of the terminal begins.
        float sh[4], lt[4];
        lin(0x7E8287, 1.0f, sh);
        lin(0xE6E8EB, 1.0f, lt);
        m_prims.AddRectRgba(0, th - 2.0f * dpi, W, 1.0f * dpi, sh, 0.0f, PrimLayer::OverBlend);
        m_prims.AddRectRgba(0, th - 1.0f * dpi, W, 1.0f * dpi, lt, 0.0f, PrimLayer::OverBlend);
    }
    else if (ch.glaze)
    {
        // Tenmoku: the strip stands on its raw stoneware foot.
        float clay[4];
        lin(0x8B7355, 1.0f, clay);
        m_prims.AddRectRgba(0, th - 3.0f * dpi, W, 3.0f * dpi, clay, 0.0f, PrimLayer::OverBlend);
    }
    else
    {
        float rail = std::max(1.0f, (ch.pills ? 3.0f : 1.5f) * dpi);   // LCARS: a bar
        m_prims.AddRectRgba(0, th - rail, W, rail, na, 0.0f, PrimLayer::Over);
    }
    if (ch.glow)
    {
        float ng[4];
        lin(ch.neonA, 0.16f, ng);
        m_prims.AddRectRgba(0, th - 5.0f * dpi, W, 5.0f * dpi, ng, 0.0f, PrimLayer::Over);
    }
    // HUD ticks at the strip's outer corners.
    if (ch.hudBrackets)
    {
        float tk[4];
        lin(ch.neonB, 0.8f, tk);
        float L = 10.0f * dpi;
        m_prims.AddRectRgba(0, 0, L, hh, tk, 0.0f, PrimLayer::Over);
        m_prims.AddRectRgba(0, 0, hh, L, tk, 0.0f, PrimLayer::Over);
        m_prims.AddRectRgba(W - L, 0, L, hh, tk, 0.0f, PrimLayer::Over);
        m_prims.AddRectRgba(W - hh, 0, hh, L, tk, 0.0f, PrimLayer::Over);
    }

    RECT menu, mn, mx, cl;
    CaptionLayout(menu, mn, mx, cl);
    DrawVitals(static_cast<float>(mn.left) - 12.0f * dpi);

    // Yellow-and-black hazard striping: a solid warning ground with black
    // diagonal bars laid over it, used wherever a skin marks something
    // destructive. The bars are slanted quads, so they lean with the chrome.
    auto hazardFill = [&](float hx, float hy, float hw, float hh, float alpha) {
        float base[4], bar[4];
        lin(ch.neonB, alpha, base);
        bar[0] = bar[1] = bar[2] = 0.0f;
        bar[3] = alpha;
        m_prims.AddRectRgba(hx, hy, hw, hh, base, 0.0f, PrimLayer::Under);
        const float pitch = 9.0f * dpi;
        const float barW = 4.0f * dpi;
        const float lean = hh * 0.55f;
        for (float bx = hx - lean; bx < hx + hw; bx += pitch)
        {
            float x0 = (std::max)(bx, hx);
            float x1 = (std::min)(bx + barW, hx + hw);
            if (x1 <= x0)
                continue;
            m_prims.AddShapeRgba(x0, hy, x1 - x0, hh, bar, 0.0f,
                                 PrimLayer::Under, 3, lean);
        }
    };

    // Hover washes (chamfered); close burns in the danger colour.
    auto wash = [&](const RECT& r, CapZone z) {
        if (m_capHover != z || ch.pills)
            return;
        float x0 = static_cast<float>(r.left);
        float w0 = static_cast<float>(r.right - r.left);
        if (z == CapZone::Close && ch.hazard)
        {
            hazardFill(x0, 2.0f * dpi, w0, th - 4.0f * dpi, 0.95f);
            return;
        }
        float w[4];
        if (z == CapZone::Close)
            lin(ch.danger, 0.85f, w);
        else
            lin(ch.neonA, ch.lightGround ? 0.30f : 0.22f, w);
        m_prims.AddShapeRgba(x0, 2.0f * dpi, w0, th - 4.0f * dpi, w,
                             0.0f, PrimLayer::Under, 2, ch.chamfer * dpi);
    };
    wash(menu, CapZone::Menu);
    wash(mn, CapZone::Min);
    wash(mx, CapZone::Max);
    wash(cl, CapZone::Close);

    float ic[4];
    lin(ch.neonA, 1.0f, ic);
    float bt = std::max(1.5f, dpi * 1.6f);
    if (ch.pills || ch.rehaut || ch.stitch || ch.meter || ch.glaze)
    {
        // LCARS: every caption control sits on its own coloured segment.
        auto seg = [&](const RECT& r, uint32_t c, CapZone z) {
            float f[4];
            lin(m_capHover == z ? ch.neonB : c, 1.0f, f);
            m_prims.AddShapeRgba(static_cast<float>(r.left) + 3.0f * dpi, 4.0f * dpi,
                                 static_cast<float>(r.right - r.left) - 6.0f * dpi,
                                 th - 8.0f * dpi, f, 0.0f, PrimLayer::Under, (ch.pills || ch.stitch || ch.meter || ch.glaze) ? 5 : 3, 0.0f);
        };
        // Atelier: brass snaps, close sewn in red thread. Tenmoku: fired-clay
        // buttons, close in cinnabar. Horologe: the close cap is the one
        // blued screw. Everyone else: the bar palette.
        const uint32_t capA = ch.stitch ? ch.neonB : ch.glaze ? ch.bars[3] : ch.bars[0];
        const uint32_t capB = ch.stitch ? ch.neonB : ch.glaze ? ch.bars[3] : ch.bars[1];
        const uint32_t capC = ch.rehaut ? ch.neonB
                              : (ch.stitch || ch.glaze) ? ch.danger : ch.bars[2];
        seg(menu, capA, CapZone::Menu);
        seg(mn, capA, CapZone::Min);
        seg(mx, capB, CapZone::Max);
        seg(cl, capC, CapZone::Close);
    }
    DrawCaptionGlyphs(menu, mn, mx, cl, ic, bg, th, dpi, bt, ch.darkText || ch.stitch || ch.glaze);

    // -------- session tabs: leaning parallelograms ----------------------------
    m_tabRects.clear();
    const float gap = 8.0f * dpi;
    const float pad = 10.0f * dpi;
    const float plusW = 30.0f * dpi;
    const float tabTop = 5.0f * dpi;
    const float tabH = th - 9.0f * dpi;
    const float slant = ch.tabSlant * dpi;
    const float rightLimit = static_cast<float>(mn.left) - 8.0f * dpi;
    const float textY = (th - m_gm.cellH * 0.8f) * 0.5f;
    const float closeW = 20.0f * dpi;
    const float maxTabW = 240.0f * dpi;
    float x = static_cast<float>(menu.right) + gap;
    float txt[3], dim[3], nb[3];
    SrgbToLinear(ch.text, txt);
    SrgbToLinear(ch.textDim, dim);
    SrgbToLinear(ch.neonB, nb);
    const bool multi = m_sessions.size() > 1;

    for (size_t i = 0; i < m_sessions.size(); ++i)
    {
        const amber::Session& s = *m_sessions[i];
        const bool activeTab = (static_cast<int>(i) == m_active);
        const bool hover = m_tabHover == static_cast<int>(i);
        std::string caption = SkinCase(s.Caption());
        if (s.unread && !activeTab)
            caption = "* " + caption;
        if (!s.Live())
            caption += SkinCase(" (closed)");
        std::string tag;
        if (ch.numberedTabs)
        {
            char b[8];
            snprintf(b, sizeof(b), "%02d ", static_cast<int>(i + 1));
            tag = b;
        }
        float tagW = m_prims.MeasureText(tag, m_chromeSampler);
        float textW = m_prims.MeasureText(caption, m_chromeSampler);
        const float lead = (ch.stitch || ch.meter || ch.glaze) ? 12.0f * dpi : 0.0f;
        float wNeed = slant + pad * 2.0f + tagW + textW + (multi ? closeW : 0.0f) +
                      (ch.pills ? tabH * 0.9f : 0.0f) + lead;
        float w = std::min(maxTabW, wNeed);
        if (x + w > rightLimit - plusW - gap)
            break;

        float fill[4];
        // What the caption will sit on, for skins whose indices are dark and
        // whose active tab is pale — the ink has to follow the fill. Zero
        // keeps the plain black of the LCARS-style skins.
        const uint32_t tabSrgb = ch.rehaut
            ? (activeTab ? ch.neonA : hover ? ch.neonB : ch.bars[i % 4]) : 0;
        // A skin that letters in BLACK on its tabs must have colour under the
        // lettering. LCARS and Brass get that from pill segments; Solder Mask
        // wants square gold fingers. The fill rule is the same for both — only
        // the silhouette differs — so the shape mode follows `pills` while the
        // decision to fill at all follows `darkText`. A light ground is
        // excluded: Swiss letters in black on paper, not on colour.
        const bool solidTab = ch.pills || (ch.darkText && !ch.lightGround);
        if (ch.impression)
        {
            // Letterpress: an inactive tab is blind-debossed — the impression
            // with no ink — and the active one is foil-stamped.
            float sh[4], lt[4];
            lin(ch.border, 1.0f, sh);
            lin(0xFFFFFF, 1.0f, lt);
            if (activeTab)
            {
                float fa[4], fb[4], rule[4];
                lin(0xE0A070, 1.0f, fa);
                lin(0x9E6636, 1.0f, fb);
                lin(0x6E4320, 1.0f, rule);
                m_prims.AddRectRgba(x, tabTop, w, tabH * 0.5f, fa, 0.0f, PrimLayer::Under);
                m_prims.AddRectRgba(x, tabTop + tabH * 0.5f, w, tabH * 0.5f, fb, 0.0f, PrimLayer::Under);
                m_prims.AddRectRgba(x, tabTop + tabH - dpi, w, dpi, rule, 0.0f, PrimLayer::Under);
            }
            m_prims.AddRectRgba(x, tabTop, w, dpi, sh, 0.0f, PrimLayer::OverBlend);
            m_prims.AddRectRgba(x, tabTop, dpi, tabH, sh, 0.0f, PrimLayer::OverBlend);
            m_prims.AddRectRgba(x, tabTop + tabH - dpi, w, dpi, lt, 0.0f, PrimLayer::OverBlend);
            m_prims.AddRectRgba(x + w - dpi, tabTop, dpi, tabH, lt, 0.0f, PrimLayer::OverBlend);
        }
        else if (ch.stitch)
        {
            // Atelier: a stitched patch — second-cut hide inside a painted
            // edge, stitched at its two ends — a run along the top or bottom
            // would cross the caption. The active one is riveted.
            float hide[4], paint[4], thread[4];
            lin(activeTab || hover ? ch.bars[1] : ch.bars[0], 1.0f, hide);
            lin(ch.border, 1.0f, paint);
            lin(ch.neonA, 1.0f, thread);
            m_prims.AddRectRgba(x, tabTop, w, tabH, paint, 0.0f, PrimLayer::Under);
            m_prims.AddRectRgba(x + 2.0f * dpi, tabTop + 2.0f * dpi, w - 4.0f * dpi,
                                tabH - 4.0f * dpi, hide, 0.0f, PrimLayer::Under);
            const float L = 5.0f * dpi, G = 3.5f * dpi, in = 4.5f * dpi, t = 1.4f * dpi;
            for (float sy = tabTop + in; sy + L <= tabTop + tabH - in; sy += L + G)
            {
                m_prims.AddRectRgba(x + in, sy, t, L, thread, 0.0f, PrimLayer::OverBlend);
                m_prims.AddRectRgba(x + w - in - t, sy, t, L, thread, 0.0f, PrimLayer::OverBlend);
            }
            if (activeTab)
            {
                float brass[4], ring[4];
                lin(ch.neonB, 1.0f, brass);
                lin(0x5C4526, 1.0f, ring);
                const float r = 3.0f * dpi, cx0 = x + 10.0f * dpi, cy0 = tabTop + tabH * 0.5f;
                m_prims.AddShapeRgba(cx0 - r - dpi, cy0 - r - dpi, 2.0f * (r + dpi), 2.0f * (r + dpi), ring, 0.0f, PrimLayer::OverBlend, 5, 0.0f);
                m_prims.AddShapeRgba(cx0 - r, cy0 - r, 2.0f * r, 2.0f * r, brass, 0.0f, PrimLayer::OverBlend, 5, 0.0f);
            }
        }
        else if (ch.meter)
        {
            // Reference: a source-selector button on the plate — bevelled,
            // with an LED at its left that lights for the active source.
            float al[4], lt[4], sh[4], led[4], glow[4];
            lin(hover ? ch.bars[1] : ch.bars[0], 1.0f, al);
            lin(0xE6E8EB, 1.0f, lt);
            lin(0x7E8287, 1.0f, sh);
            lin(activeTab ? ch.neonA : 0x5A3D0A, 1.0f, led);
            lin(ch.neonA, 0.35f, glow);
            m_prims.AddRectRgba(x, tabTop, w, tabH, al, 0.0f, PrimLayer::Under);
            m_prims.AddRectRgba(x, tabTop, w, dpi, lt, 0.0f, PrimLayer::OverBlend);
            m_prims.AddRectRgba(x, tabTop + tabH - dpi, w, dpi, sh, 0.0f, PrimLayer::OverBlend);
            const float r = 2.0f * dpi, cx0 = x + 9.0f * dpi, cy0 = tabTop + tabH * 0.5f;
            if (activeTab)
                m_prims.AddShapeRgba(cx0 - r * 2.5f, cy0 - r * 2.5f, r * 5.0f, r * 5.0f, glow, 0.0f, PrimLayer::OverBlend, 5, 0.0f);
            m_prims.AddShapeRgba(cx0 - r, cy0 - r, 2.0f * r, 2.0f * r, led, 0.0f, PrimLayer::OverBlend, 5, 0.0f);
        }
        else if (ch.glaze)
        {
            // Tenmoku: a glazed tile — pooled darker at its edges, rust along
            // the rim, a raw stoneware foot — with the chop on the active one.
            float gl[4], pool[4], rim[4], clay[4];
            lin(activeTab || hover ? ch.bars[1] : ch.bars[0], 1.0f, gl);
            lin(0x120C0A, 1.0f, pool);
            lin(0x7A3B1E, 1.0f, rim);
            lin(0x8B7355, 1.0f, clay);
            m_prims.AddRectRgba(x, tabTop, w, tabH, pool, 0.0f, PrimLayer::Under);
            m_prims.AddRectRgba(x + dpi, tabTop + dpi, w - 2.0f * dpi, tabH - 2.0f * dpi, gl, 0.0f, PrimLayer::Under);
            m_prims.AddRectRgba(x + dpi, tabTop + dpi, w - 2.0f * dpi, 1.5f * dpi, rim, 0.0f, PrimLayer::OverBlend);
            m_prims.AddRectRgba(x, tabTop + tabH - 3.0f * dpi, w, 3.0f * dpi, clay, 0.0f, PrimLayer::OverBlend);
            if (activeTab)
            {
                float chop[4];
                lin(ch.danger, 1.0f, chop);
                const float cs = 8.0f * dpi, cx0 = x + 6.0f * dpi, cy0 = tabTop + (tabH - cs) * 0.5f;
                m_prims.AddShapeRgba(cx0, cy0, cs, cs, chop, 0.0f, PrimLayer::OverBlend, 4, 1.5f * dpi);
                m_prims.AddShapeRgba(cx0 + cs * 0.3f, cy0 + cs * 0.3f, cs * 0.4f, cs * 0.4f, gl, 0.0f, PrimLayer::OverBlend, 4, 0.0f);
            }
        }
        else if (solidTab)
        {
            // LCARS: a solid segment; the active tab is orange, the rest
            // cycle through the bar colours, hover turns mustard. Solder Mask:
            // the same, as dulled gold edge-connector fingers.
            lin(activeTab ? ch.neonA : hover ? ch.neonB : ch.bars[i % 4], 1.0f, fill);
            m_prims.AddShapeRgba(x, tabTop, w, tabH, fill, 0.0f, PrimLayer::Under,
                                 ch.pills ? 5 : 3, ch.pills ? 0.0f : slant);
        }
        else if (ch.outline > 0.0f)
        {
            // Blueprint: no fills anywhere. The tab is a drafted outline; the
            // active one is inked heavier and gets a second inset line, the way
            // a drawing marks the sheet you are looking at.
            float ol[4];
            lin(activeTab ? ch.neonA : hover ? ch.neonB : ch.border,
                activeTab ? 1.0f : hover ? 0.9f : 0.75f, ol);
            float pen = std::max(1.0f, ch.outline * dpi);
            m_prims.AddShapeRgba(x, tabTop, w, tabH, ol, pen, PrimLayer::Over, 3, slant);
            if (activeTab)
            {
                float in = 3.0f * dpi;
                m_prims.AddShapeRgba(x + in, tabTop + in, w - in * 2.0f,
                                     tabH - in * 2.0f, ol, std::max(1.0f, dpi),
                                     PrimLayer::Over, 3, slant);
            }
        }
        else
        {
            if (activeTab)      lin(ch.neonA, ch.lightGround ? 1.0f : 0.16f, fill);
            else if (hover)     lin(ch.neonA, ch.lightGround ? 0.18f : 0.08f, fill);
            else                lin(ch.panel, 0.92f, fill);
            m_prims.AddShapeRgba(x, tabTop, w, tabH, fill, 0.0f, PrimLayer::Under, 3, slant);
            float ol[4];
            if (activeTab)      lin(ch.neonA, 0.95f, ol);
            else if (hover)     lin(ch.neonA, 0.55f, ol);
            else                lin(ch.border, 0.9f, ol);
            m_prims.AddShapeRgba(x, tabTop, w, tabH, ol, std::max(1.0f, 1.2f * dpi),
                                 PrimLayer::Over, 3, slant);
        }
        if (activeTab && ch.glow)
        {
            float gl[4];
            lin(ch.neonA, 0.20f, gl);
            m_prims.AddShapeRgba(x - 2.0f * dpi, tabTop - 2.0f * dpi, w + 4.0f * dpi,
                                 tabH + 4.0f * dpi, gl, 3.0f * dpi, PrimLayer::Over, 3, slant);
        }
        if (activeTab && !solidTab)
        {
            // Underline on the active tab. A hazard skin makes it the striped
            // warning bar instead of a plain accent rule.
            if (ch.hazard)
                hazardFill(x + slant * 0.5f, tabTop + tabH - 4.0f * dpi,
                           w - slant, 4.0f * dpi, 0.95f);
            else
            {
                float rb[4];
                lin(ch.neonB, 0.9f, rb);
                m_prims.AddRectRgba(x + slant * 0.5f, tabTop + tabH - 2.0f * dpi,
                                    w - slant, 2.0f * dpi, rb, 0.0f, PrimLayer::Over);
            }
        }

        // Clip the caption to the tab so it never runs under the close glyph.
        float avail = w - slant - pad * 2.0f - tagW - (multi ? closeW : 0.0f) -
                      (ch.pills ? tabH * 0.9f : 0.0f) + lead;
        // Half a pixel of tolerance: avail is derived from w, which is derived
        // from textW, and fractional proportional advances do not survive the
        // round trip exactly. Without it a caption that fits gets an ellipsis.
        if (textW > avail + 0.5f && !caption.empty())
        {
            std::string cut = caption;
            while (!cut.empty() &&
                   m_prims.MeasureText(cut + "\xE2\x80\xA6", m_chromeSampler) > avail)
            {
                cut.pop_back();
                while (!cut.empty() &&
                       (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
                    cut.pop_back();
            }
            caption = cut + "\xE2\x80\xA6";
        }
        float tx = x + (ch.pills ? tabH * 0.45f : slant * 0.5f) + pad * (ch.pills ? 0.6f : 1.0f) + lead;
        if (ch.meter)
        {
            // Reference: the caption is engraved — the highlight copy one
            // pixel below, then the ink on top of it.
            float el[3];
            SrgbToLinear(0xE6E8EB, el);
            float ex = tx;
            if (!tag.empty())
                ex += SkinText(ex, textY + dpi, tag, el);
            SkinText(ex, textY + dpi, caption, el);
        }
        if (!tag.empty())
        {
            float tc[3];
            for (int k = 0; k < 3; ++k)
                // Letterpress: the foil is neonB, so its tag must be the ink.
                tc[k] = (activeTab && !ch.impression) ? nb[k]
                        : activeTab ? txt[k] : dim[k] * 0.9f;
            tx += SkinText(tx, textY, tag, tc, tabSrgb);
        }
        float cc[3];
        for (int k = 0; k < 3; ++k)
            cc[k] = activeTab ? txt[k] : (s.Live() ? dim[k] * 1.35f : dim[k] * 0.7f);
        SkinText(tx, textY, caption, cc, tabSrgb);

        // Activity spark on background tabs (magenta); red after a trigger.
        if (!activeTab && m_fxLive)
        {
            float glow = std::exp(-static_cast<float>(m_time - s.lastOutputAt) / 1.5f);
            bool trig = (m_time - s.lastTriggerAt) < 5.0;
            if (glow > 0.03f || trig)
            {
                float sx = x + w - slant * 0.5f - (multi ? closeW : 0.0f) - 7.0f * dpi;
                float sy = tabTop + tabH * 0.5f;
                float flick = 0.7f + 0.3f * std::sin(static_cast<float>(m_time) * 17.0f +
                                                     static_cast<float>(i) * 2.1f);
                float sz = (2.5f + 2.5f * glow) * dpi;
                float sc[4];
                if (trig)
                    lin(ch.danger, 0.95f * flick, sc);
                else
                    lin(ch.neonB, glow * flick, sc);
                m_prims.AddShapeRgba(sx - sz * 0.5f, sy - sz * 0.5f, sz, sz, sc, 0.0f,
                                     PrimLayer::Over, 4, sz * 0.3f);
            }
        }

        // Per-tab close ×, when more than one session is open. On a
        // dark-text skin the ink follows the tab's fill; otherwise neon,
        // danger red under the pointer.
        if (multi)
        {
            float cxX = x + w - slant * 0.5f - closeW * 0.5f;
            float hx = 4.0f * dpi;
            float xc[4];
            if (ch.darkText)
            {
                xc[0] = xc[1] = xc[2] = 0.0f;
                if (tabSrgb)
                {
                    const COLORREF ink = amber::InkOn(tabSrgb);
                    SrgbToLinear((GetRValue(ink) << 16) | (GetGValue(ink) << 8) | GetBValue(ink), xc);
                }
                xc[3] = 1.0f;
            }
            else if (hover)
                lin(ch.danger, 1.0f, xc);
            else
                lin(ch.neonA, 0.7f, xc);
            const PrimLayer XL = ch.darkText ? PrimLayer::OverBlend : PrimLayer::Over;
            auto seg = [&](float x0, float y0, float x1, float y1) {
                int steps = std::max(3, static_cast<int>(std::abs(x1 - x0)));
                for (int k = 0; k <= steps; ++k)
                {
                    float t = static_cast<float>(k) / steps;
                    m_prims.AddRectRgba(x0 + (x1 - x0) * t - bt * 0.5f,
                                        y0 + (y1 - y0) * t - bt * 0.5f, bt, bt,
                                        xc, 0.0f, XL);
                }
            };
            float my = th * 0.5f;
            seg(cxX - hx, my - hx, cxX + hx, my + hx);
            seg(cxX - hx, my + hx, cxX + hx, my - hx);
        }

        m_tabRects.push_back({ x, w });
        x += w + gap;
    }

    // New-session (+) button: chamfered wash on hover, neon plus.
    {
        m_plusRect = { static_cast<LONG>(x), static_cast<LONG>(tabTop),
                       static_cast<LONG>(x + plusW),
                       static_cast<LONG>(tabTop + tabH) };
        float pb[4];
        if (ch.pills)
        {
            lin(m_capHover == CapZone::Plus ? ch.neonB : ch.bars[3], 1.0f, pb);
            m_prims.AddShapeRgba(x, tabTop, plusW, tabH, pb, 0.0f, PrimLayer::Under, 5, 0.0f);
        }
        else
        {
            lin(ch.neonA, m_capHover == CapZone::Plus ? 0.22f : 0.0f, pb);
            if (pb[3] > 0.0f)
                m_prims.AddShapeRgba(x, tabTop, plusW, tabH, pb, 0.0f, PrimLayer::Under, 2,
                                     ch.chamfer * dpi);
            float po[4];
            lin(ch.border, 0.9f, po);
            m_prims.AddShapeRgba(x, tabTop, plusW, tabH, po, std::max(1.0f, dpi),
                                 PrimLayer::Over, 2, ch.chamfer * dpi);
        }
        float pcx = x + plusW * 0.5f, pcy = th * 0.5f, pl = 6.0f * dpi;
        float pc[4] = { ic[0], ic[1], ic[2], 1.0f };
        if (ch.darkText)
        { pc[0] = pc[1] = pc[2] = 0.0f; }
        const PrimLayer PL = ch.darkText ? PrimLayer::OverBlend : PrimLayer::Over;
        m_prims.AddRectRgba(pcx - pl, pcy - bt * 0.5f, pl * 2.0f, bt, pc, 0.0f, PL);
        m_prims.AddRectRgba(pcx - bt * 0.5f, pcy - pl, bt, pl * 2.0f, pc, 0.0f, PL);
    }
}

void App::DrawTabBar()
{
    m_tabRects.clear();
    // A single session needs no tab strip; the title bar already names it.
    if (m_sessions.size() < 2)
    {
        m_tabBarH = 0.0f;
        return;
    }

    const float h = std::max(18.0f, m_gm.cellH * 1.05f);
    m_tabBarH = h;

    const float top = m_titleBarH;   // tabs sit just below the title bar
    const float screenW = static_cast<float>(m_device.Width());
    const float pad = 10.0f;
    float x = 4.0f;
    const float maxW = std::max(80.0f, (screenW - 8.0f) /
                                       static_cast<float>(m_sessions.size()));

    // Strip background, so glyph particles behind it do not bleed through.
    m_prims.AddRect(0.0f, top, screenW, h, 0.06f, 0.0f, PrimLayer::Under);

    for (size_t i = 0; i < m_sessions.size(); ++i)
    {
        const amber::Session& s = *m_sessions[i];
        bool activeTab = (static_cast<int>(i) == m_active);

        std::string caption = s.Caption();
        if (s.unread && !activeTab)
            caption = "* " + caption;
        if (!s.Live())
            caption += " (closed)";

        float textW = m_prims.MeasureText(caption, m_sampler);
        float w = std::min(maxW, textW + pad * 2.0f);
        if (x + w > screenW - 4.0f)
            break;                       // out of room: stop drawing tabs

        // Active tab reads brighter; unread background tabs get a lift.
        float bg = activeTab ? 0.20f : (s.unread ? 0.12f : 0.07f);
        m_prims.AddRect(x, top + 1.0f, w - 2.0f, h - 2.0f, bg, 0.0f,
                        PrimLayer::Under);

        float intensity = activeTab ? 1.0f : (s.Live() ? 0.62f : 0.38f);
        m_prims.AddText(x + pad, top + (h - m_gm.cellH * 0.8f) * 0.5f, caption,
                        intensity, m_sampler);

        m_tabRects.push_back({ x, w });
        x += w;
    }
}

float App::StatusBarH() const
{
    if (!m_statusBar || m_minimized)
        return 0.0f;
    // Deliberately thin: one line of chrome, not a panel. Tied to the font so
    // it stays proportionate when the terminal is scaled up.
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    return std::floor(std::max(22.0f * dpi, m_gm.cellH + 4.0f * dpi));
}

void App::DrawStatusBar()
{
    m_sbChips.clear();
    if (m_statusBarH <= 0.0f)
        return;

    const amber::ChromeSpec& ch = amber::Chrome();
    const bool skin = amber::ChromeSkinned();
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const float W = static_cast<float>(m_device.Width());
    const float H = static_cast<float>(m_device.Height());
    const float y0 = H - m_statusBarH;
    const float h = m_statusBarH;
    const float hair = std::max(1.0f, dpi);

    // A skin paints from its own palette; Classic follows the terminal theme.
    auto col = [&](uint32_t srgb, float rampT, float a, float out[4]) {
        float c[3];
        if (skin)
            SrgbToLinear(srgb, c);
        else
            AmberRampCpu(rampT, c);
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = a;
    };

    // On a light skin the bar's ground is paper. The additive text path can
    // only ADD light, so it is invisible there — those skins must letter the
    // bar with opaque core glyphs instead, exactly as the tabs do.
    const bool barLight = skin && amber::LumaSrgb(ch.bg) > 0.55f;
    auto emit = [&](float ex, float ey, const std::string& t, const float rgb[3]) {
        return barLight ? m_prims.AddTextCore(ex, ey, t, rgb, 1.0f, m_sampler)
                        : m_prims.AddTextRgb(ex, ey, t, rgb, m_sampler);
    };

    float ground[4];
    col(ch.bg, 0.03f, 1.0f, ground);
    m_prims.AddRectRgba(0, y0, W, h, ground, 0.0f, PrimLayer::Under);
    // Accent rule along the top edge — a bar on the pill skins, a hairline
    // everywhere else, matching what the title strip does at the other end.
    float rule[4];
    col(ch.neonA, 0.55f, skin ? 0.95f : 0.55f, rule);
    // Additive on a dark bar so the accent glows; blended on a light one,
    // where adding light to paper does nothing.
    const PrimLayer barInk = barLight ? PrimLayer::OverBlend : PrimLayer::Over;
    m_prims.AddRectRgba(0, y0, W, (skin && ch.pills) ? 3.0f * dpi : hair, rule,
                        0.0f, barInk);

    const float textY = y0 + (h - m_gm.cellH * 0.8f) * 0.5f;
    const float padX = 10.0f * dpi;

    // ---- right: clickable chips, laid out from the right edge inwards ------
    // State-bearing entries render lit when they are on, so the bar reports as
    // well as acts.
    struct ChipDef { const char* label; int cmd; bool on; };
    const bool split = HasSession() && Cur().layout.Count() > 1;
    const size_t bcTargets = HasSession() ? Cur().broadcast.size() : 0;
    // "bcast 4" rather than a lit chip: the count is the thing that stops a
    // broadcast being sent somewhere the user had forgotten about.
    if (bcTargets)
        snprintf(m_bcastChip, sizeof(m_bcastChip), "bcast %zu", bcTargets);
    const bool folded = HasSession() && Cur().AnyCollapsed();
    // The cloak chip carries the count of covered runs on screen, because
    // "cloak" lit tells you the feature is on and nothing about whether it
    // actually found anything — which is the question you have when you are
    // about to share the window.
    if (m_cloak.enabled && HasSession() && Foc().cloakCount > 0)
        snprintf(m_cloakChip, sizeof(m_cloakChip), "cloak %d", Foc().cloakCount);
    const ChipDef defs[] = {
        { m_cloak.enabled && HasSession() && Foc().cloakCount > 0 ? m_cloakChip
                                                                  : "cloak",
          IdmCloak, m_cloak.enabled },
        { "quake",   IdmQuakeMode, m_quake },
        { "fold",    folded ? IdmFoldNone : IdmFoldAll, folded },
        // The chip carries the target COUNT, not just an on/off state, and
        // clicking it while broadcasting is the emergency stop rather than
        // another way into the picker.
        { bcTargets ? m_bcastChip : "bcast",
          bcTargets ? IdmBroadcastStop : IdmBroadcastPick, bcTargets > 0 },
        { "split",   IdmSplitVertical, split },
        { "sftp",    IdmSftpPanel, false },
        { "journal", IdmJournal,   m_jrnOpen },
        { "palette", IdmCommandPalette, m_palOpen },
    };
    float x = W - padX;
    for (const ChipDef& d : defs)
    {
        const float tw = m_prims.MeasureText(d.label, m_sampler);
        const float cw = tw + 14.0f * dpi;
        if (x - cw < W * 0.42f)
            break;                    // never crowd the left-hand readout
        x -= cw;
        StatusChip chip;
        chip.x = x;
        chip.w = cw;
        chip.cmd = d.cmd;
        chip.label = d.label;
        chip.on = d.on;
        m_sbChips.push_back(chip);
        x -= 4.0f * dpi;
    }

    for (size_t i = 0; i < m_sbChips.size(); ++i)
    {
        const StatusChip& c = m_sbChips[i];
        const bool hot = (static_cast<int>(i) == m_sbHover);
        if (c.on || hot)
        {
            float fill[4];
            col(c.on ? ch.neonA : ch.neonB, c.on ? 0.55f : 0.35f,
                c.on ? (skin ? 1.0f : 0.55f) : 0.22f, fill);
            const float iy = y0 + 3.0f * dpi;
            const float ih = h - 6.0f * dpi;
            if (skin && ch.pills)
                m_prims.AddShapeRgba(c.x, iy, c.w, ih, fill, 0.0f,
                                     PrimLayer::Under, 5, 0.0f);
            else if (skin && ch.chamfer > 0.0f)
                m_prims.AddShapeRgba(c.x, iy, c.w, ih, fill, 0.0f,
                                     PrimLayer::Under, 2, ch.chamfer * dpi * 0.6f);
            else
                m_prims.AddRectRgba(c.x, iy, c.w, ih, fill, 0.0f, PrimLayer::Under);
        }
        // Ink on a lit chip is chosen by the fill, not assumed: a bright accent
        // (LCARS orange, Swiss red, brass gold) needs dark lettering.
        float ink[3];
        if (c.on && skin && amber::LumaSrgb(ch.neonA) > 0.55f)
            ink[0] = ink[1] = ink[2] = 0.0f;
        else if (skin)
            SrgbToLinear(c.on ? ch.bg : hot ? ch.neonA : ch.textDim, ink);
        else
            AmberRampCpu(c.on ? 0.10f : hot ? 0.90f : 0.55f, ink);
        const float lx = c.x + (c.w - m_prims.MeasureText(c.label, m_sampler)) * 0.5f;
        if (c.on && skin && amber::LumaSrgb(ch.neonA) > 0.55f)
            m_prims.AddTextCore(lx, textY, c.label, ink, 1.0f, m_sampler);
        else
            emit(lx, textY, c.label, ink);
    }

    // ---- left: where you are, and what just happened -----------------------
    float lx = padX;
    if (HasSession())
    {
        const amber::Session& s = Cur();
        // Connection pip: green connected, amber connecting, red otherwise.
        float pip[4] = { 0.15f, 0.85f, 0.35f, 0.95f };
        if (s.state == amber::SessionState::Connecting)
        { pip[0] = 1.0f; pip[1] = 0.70f; pip[2] = 0.10f; }
        else if (s.state != amber::SessionState::Connected)
        { pip[0] = 1.0f; pip[1] = 0.25f; pip[2] = 0.18f; }
        // A session waiting on the guardian pulses, so the bar distinguishes
        // "down and coming back" from "down and staying down" at a glance.
        if (s.guardian.Armed())
            pip[3] = 0.45f + 0.5f * static_cast<float>(
                                        0.5 + 0.5 * std::sin(m_time * 4.0));
        const float ps = 6.0f * dpi;
        m_prims.AddRectRgba(lx, y0 + (h - ps) * 0.5f, ps, ps, pip, 0.0f, barInk);
        lx += ps + 8.0f * dpi;

        float dim[3];
        if (skin)
            SrgbToLinear(ch.textDim, dim);
        else
            AmberRampCpu(0.62f, dim);
        std::string where = s.Caption();
        if (!s.cwd.empty())
            where += "  " + s.cwd;
        // Advance by the MEASURED width rather than the draw call's return:
        // the core-glyph path reports a slightly different pen position, and
        // trusting it let the status message overlap the directory.
        emit(lx, textY, where, dim);
        lx += m_prims.MeasureText(where, m_sampler) + 16.0f * dpi;

        // Guardian read-out, in the skin's danger colour: what the reconnect
        // machine is doing, counted down, so nothing about it is a surprise.
        const std::string g = s.guardian.StatusText(m_time);
        if (!g.empty())
        {
            float warn[3];
            if (skin)
                SrgbToLinear(s.guardian.State() == amber::GuardianState::Reconnected
                                 ? ch.neonA
                                 : ch.danger,
                             warn);
            else if (s.guardian.State() == amber::GuardianState::Reconnected)
            { warn[0] = 0.25f; warn[1] = 0.95f; warn[2] = 0.45f; }
            else
            { warn[0] = 1.0f; warn[1] = 0.45f; warn[2] = 0.12f; }
            emit(lx, textY, g, warn);
            lx += m_prims.MeasureText(g, m_sampler) + 16.0f * dpi;
        }
    }
    // A hovered OSC 8 target outranks the transient message: it is about
    // where the pointer is right now, and it disappears the moment it moves.
    if (!m_hoverLink.empty())
    {
        float hi[3];
        if (skin)
            SrgbToLinear(ch.neonB, hi);
        else
            AmberRampCpu(0.80f, hi);
        const float room = (m_sbChips.empty() ? W - padX : m_sbChips.back().x) -
                           lx - 12.0f * dpi;
        std::string msg = m_hoverLink;
        if (room > 0.0f && m_prims.MeasureText(msg, m_sampler) > room)
        {
            while (!msg.empty() &&
                   m_prims.MeasureText(msg + "\xE2\x80\xA6", m_sampler) > room)
            {
                msg.pop_back();
                while (!msg.empty() &&
                       (static_cast<unsigned char>(msg.back()) & 0xC0) == 0x80)
                    msg.pop_back();
            }
            msg += "\xE2\x80\xA6";
        }
        if (room > 0.0f)
            emit(lx, textY, msg, hi);
        return;
    }
    // The transient message lives here now instead of floating over output.
    if (!m_status.empty() && m_time < m_statusUntil)
    {
        float hi[3];
        if (skin)
            SrgbToLinear(ch.neonA, hi);
        else
            AmberRampCpu(0.88f, hi);
        const float room = (m_sbChips.empty() ? W - padX : m_sbChips.back().x) -
                           lx - 12.0f * dpi;
        std::string msg = m_status;
        if (room > 0.0f && m_prims.MeasureText(msg, m_sampler) > room)
        {
            while (!msg.empty() &&
                   m_prims.MeasureText(msg + "\xE2\x80\xA6", m_sampler) > room)
            {
                msg.pop_back();
                while (!msg.empty() &&
                       (static_cast<unsigned char>(msg.back()) & 0xC0) == 0x80)
                    msg.pop_back();
            }
            msg += "\xE2\x80\xA6";
        }
        if (room > 0.0f)
            emit(lx, textY, msg, hi);
    }
}

int App::StatusChipAt(int px, int py) const
{
    if (m_statusBarH <= 0.0f)
        return -1;
    const float y0 = static_cast<float>(m_device.Height()) - m_statusBarH;
    if (static_cast<float>(py) < y0)
        return -1;
    for (size_t i = 0; i < m_sbChips.size(); ++i)
    {
        const StatusChip& c = m_sbChips[i];
        if (static_cast<float>(px) >= c.x && static_cast<float>(px) < c.x + c.w)
            return static_cast<int>(i);
    }
    return -1;
}

void App::DrawStatusLine()
{
    // When F3 is on, a compact performance read-out, drawn with the atlas text
    // renderer so no third-party UI toolkit is involved. It sits above the
    // status bar, which owns the transient message ("Copied 148 characters")
    // and gives it a fixed home instead of letting it float over output.
    if (m_statusBarH <= 0.0f && !m_status.empty() && m_time < m_statusUntil)
    {
        const float y = static_cast<float>(m_device.Height()) - m_gm.cellH * 1.2f;
        m_prims.AddText(m_gm.originX, y, m_status, 0.75f, m_sampler);
    }
    if (m_time >= m_statusUntil)
        m_status.clear();

    if (m_showOverlay)
    {
        uint32_t cells = m_gm.cols * m_gm.rows;
        uint32_t glyphs = m_prims.CoreGlyphsLastFrame();
        char line[320];
        snprintf(line, sizeof(line),
                 "%.1f fps  cpu %.2f ms  gpu %.2f ms (bloom %.2f)  "
                 "density %s (%u/glyph)",
                 m_fps, m_frameMs, m_device.GpuFrameMs(), m_device.GpuBloomMs(),
                 DensityLabel(), m_particles.tun.particlesPerCell);
        m_prims.AddText(m_gm.originX, m_titleBarH + 2.0f, line, 0.6f, m_sampler);
        snprintf(line, sizeof(line),
                 "cells %u  glyphs %u  particles %u  dirty %u  grid %ux%u  "
                 "%ls %.1fpx  %s",
                 cells, glyphs, m_particles.ParticleCount(),
                 m_particles.DirtyCellsLastFrame(), m_gm.cols, m_gm.rows,
                 m_sampler.FontName().c_str(), m_fontPx,
                 m_device.HdrActive() ? "HDR" : "SDR");
        m_prims.AddText(m_gm.originX, m_titleBarH + 2.0f + m_gm.cellH, line,
                        0.6f, m_sampler);

        // Protocol health. Every figure here is a count of something that
        // actually happened, so a rising number is evidence rather than a
        // guess: malformed UTF-8 recovered from, wide cells on screen right
        // now, grapheme clusters interned, glyphs the terminal face could not
        // draw, live OSC 8 targets, and what the inline images are costing.
        if (HasSession())
        {
            const amber::Session& S = Foc();
            const Grid& g = S.grid;
            int wide = 0, clusters = 0, links = 0;
            for (int r = 0; r < g.Rows(); ++r)
                for (int c = 0; c < g.Cols(); ++c)
                {
                    const Cell& cell = g.ViewCell(r, c);
                    if (cell.flags & CellWideLead)
                        ++wide;
                    if (amber::IsClusterAlias(cell.cp))
                        ++clusters;
                    if (cell.link)
                        ++links;
                }
            const size_t imgBytes = ImageBytes(S);
            snprintf(line, sizeof(line),
                     "utf8-err %llu  clusters %zu (screen %d, shaped %llu, "
                     "fallback %llu)  wide %d  links %zu/%d (rejected %llu)  "
                     "images %zu (%.1f MB)  img-fail %llu",
                     static_cast<unsigned long long>(S.parser.Utf8Errors()),
                     amber::Clusters().Count(), clusters,
                     static_cast<unsigned long long>(m_sampler.ClusterHits()),
                     static_cast<unsigned long long>(m_sampler.ClusterMisses()),
                     wide, S.parser.LinkCount(), links,
                     static_cast<unsigned long long>(S.parser.LinkRejects()),
                     S.images.size(),
                     static_cast<double>(imgBytes) / (1024.0 * 1024.0),
                     static_cast<unsigned long long>(S.parser.ImageDecodeFails()));
            m_prims.AddText(m_gm.originX, m_titleBarH + 2.0f + m_gm.cellH * 2.0f,
                            line, 0.6f, m_sampler);

            // A desktop tab: its own two lines, in app_vnc.cpp.
            if (S.IsVnc())
                VncStatusLines(S, m_titleBarH + 2.0f + m_gm.cellH * 3.0f);

            // AmberX, when this session has a host. Two lines: what the
            // server is holding, and what the transport is doing. Only
            // counts appear here — no titles, no window contents, and the
            // last error is the host's own bounded text.
            const RemoteAppReport ax = S.ssh.AmberXReport();
            if (ax.valid)
            {
                snprintf(line, sizeof(line),
                         "AmberX %s (X.Org %s)  %u client%s  %u window%s  "
                         "pixmaps %.1f MiB  %s",
                         amber::amberx::kAmberXVersion, amber::amberx::kUpstreamVersion,
                         ax.clients, ax.clients == 1 ? "" : "s",
                         ax.windows, ax.windows == 1 ? "" : "s",
                         static_cast<double>(ax.pixmapBytes) / (1024.0 * 1024.0),
                         S.profile.x11Trust != 0 ? "trusted" : "restricted");
                m_prims.AddText(m_gm.originX, m_titleBarH + 2.0f + m_gm.cellH * 3.0f,
                                line, 0.6f, m_sampler);
                snprintf(line, sizeof(line),
                         "AmberX x11 in %.2f MiB  out %.2f MiB  presents %u  "
                         "dirty %u  ipc high water %u B  refused %u  "
                         "host wait max %u us avg %u us (%u)  gpu n/a%s%s",
                         static_cast<double>(ax.x11In) / (1024.0 * 1024.0),
                         static_cast<double>(ax.x11Out) / (1024.0 * 1024.0),
                         ax.presents, ax.dirtyRects, ax.ipcHighWater, ax.rejected,
                         ax.hostWaitMaxUs, ax.hostWaitAvgUs, ax.hostWaitFrames,
                         ax.lastError.empty() ? "" : "  last error: ",
                         ax.lastError.c_str());
                m_prims.AddText(m_gm.originX, m_titleBarH + 2.0f + m_gm.cellH * 4.0f,
                                line, 0.6f, m_sampler);
            }
        }
    }
}

// --------------------------------------------------------------------- input
void App::SendToShell(const std::string& bytes)
{
    if (!HasSession() || bytes.empty())
        return;
    amber::Session& F = Foc();
    // A read-only pane still receives output and can still be selected,
    // copied and searched — it just refuses input. Checked here so every
    // route into the shell (typing, paste, snippets, the journal, block
    // rerun, broadcast) is covered by one test rather than seven.
    if (F.readOnly)
    {
        SetStatus("This pane is read-only — Ctrl+Shift+R unlocks it.", 4.0);
        return;
    }
    if (F.state != amber::SessionState::Connected)
        return;
    // Blast radius: the last gate before anything leaves. Placed here so every
    // route into the shell — typing, paste, snippets, block rerun, broadcast —
    // passes the same check, and a refusal sends nothing at all.
    if (!RiskCheck(bytes))
    {
        SetStatus("Not sent.", 3.0);
        return;
    }
    if (F.diagnostic)
    {
        // The diagnostic session has no remote end: echo locally so typing
        // exercises the per-letter materialize/dissolve animations.
        std::string echo;
        for (char ch : bytes)
        {
            if (ch == '\r')
                echo += "\r\n";
            else if (ch == '\x7f' || ch == '\b')
                echo += "\b \b";
            else
                echo += ch;
        }
        F.parser.Feed(reinterpret_cast<const uint8_t*>(echo.data()),
                      echo.size());
        F.grid.SnapView();
        return;
    }
    F.ssh.Send(bytes.data(), bytes.size());
    F.grid.SnapView();

    // Broadcast: mirror the keystrokes into every EXPLICITLY selected pane.
    // The focused pane already got them above.
    //
    // The set is a list of pane ids, never "all panes" — a pane opened after
    // the set was chosen is not in it, so splitting a new terminal off a
    // broadcasting tab cannot quietly add a target. A read-only pane is
    // skipped for the same reason it refuses typing.
    amber::Session& tab = Cur();
    if (tab.broadcast.empty())
        return;
    const amber::PaneId self = PaneIdOf(tab, F);
    for (amber::PaneId id : tab.broadcast)
    {
        if (id == self)
            continue;
        amber::Session* p = PaneById(tab, id);
        if (!p || p->readOnly || p->diagnostic ||
            p->state != amber::SessionState::Connected)
            continue;
        p->ssh.Send(bytes.data(), bytes.size());
        p->grid.SnapView();
    }
}

void App::OnChar(wchar_t wc, bool alt)
{
    m_lastInputTime = m_time;
    if (m_swallowChar)
    {
        m_swallowChar = false;
        return;
    }
    if (VncChar(wc))
        return;

    // Surrogate pairs → full codepoint.
    char32_t cp;
    if (wc >= 0xD800 && wc <= 0xDBFF)
    {
        m_pendingHighSurrogate = wc;
        return;
    }
    if (wc >= 0xDC00 && wc <= 0xDFFF && m_pendingHighSurrogate)
    {
        cp = 0x10000 + ((m_pendingHighSurrogate - 0xD800) << 10) + (wc - 0xDC00);
        m_pendingHighSurrogate = 0;
    }
    else
        cp = wc;

    // Palette open: printable input edits the query instead of the shell.
    if (m_palOpen)
    {
        if (cp >= 0x20 && cp != 0x7F)
        {
            AppendUtf8(m_palQuery, cp);
            FilterPalette();
        }
        return;
    }
    if (m_jrnOpen)
    {
        if (cp >= 0x20 && cp != 0x7F)
        {
            AppendUtf8(m_jrnQuery, cp);
            FilterJournal();
        }
        return;
    }

    if (!HasSession() || Foc().state != amber::SessionState::Connected)
        return;

    if (cp == 0x08)   // Backspace arrives via WM_KEYDOWN as 0x7F
        return;
    // The Windows emoji panel suffixes some emoji with a variation selector;
    // shells echo the invisible selector as "<fe0f>" noise (see Paste).
    if (cp == 0xFE0F || cp == 0xFE0E)
        return;
    std::string out;
    if (alt)
        out.push_back(0x1B);
    AppendUtf8(out, cp);
    if (cp == U'\r')
        TriggerShockwave();   // Enter fires the ring, like the reference
    if (Foc().profile.scrollOnKey)
        Foc().grid.SnapView();
    // Terminal page: local echo / line editing mirror or hold the input.
    if (HandleLocalLine(Foc(), out))
        return;
    // Typing echo ring: opens now, closes when the server's echo arrives.
    if (!Foc().diagnostic && cp >= 0x20)
    {
        Foc().echoPending = true;
        Foc().echoSentAt = m_time;
    }
    SendToShell(out);
}

bool App::OnKeyDown(WPARAM vk)
{
    m_lastInputTime = m_time;
    // A desktop tab takes the keys a terminal would turn into bytes; the
    // application's own shortcuts (palette, tabs) stay ahead of it inside.
    if (VncKey(vk, true))
        return true;
    KeyMods mods;
    mods.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    mods.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    mods.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    // ---- the paste guard is a question: nothing else runs until answered --
    if (m_pasteOpen)
        return PasteGuardKey(vk);

    // ---- command palette owns the keyboard while open ---------------------
    if (mods.ctrl && mods.shift && vk == 'P')
    {
        TogglePalette();
        m_swallowChar = true;
        return true;
    }
    if (m_palOpen)
        return PaletteKey(vk);
    if (mods.ctrl && mods.shift && vk == 'J')
    {
        ToggleJournal();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'G')
    {
        ToggleRemoteApps();
        m_swallowChar = true;
        return true;
    }
    if (m_appsOpen)
        return RemoteAppsKey(vk);
    if (mods.ctrl && mods.shift && vk == 'O')
    {
        ToggleFoldAtCursor();
        m_swallowChar = true;
        return true;
    }
    // The cloak needs to be one keystroke away: it is reached in the second
    // before a screen share starts, not from a settings page.
    if (mods.ctrl && mods.shift && vk == 'M')
    {
        HandleMenuCommand(IdmCloak);
        m_swallowChar = true;
        return true;
    }
    if (m_jrnOpen)
        return JournalKey(vk);
    // Ctrl+Up / Ctrl+Down step between command marks in the scrollback. Only
    // on the main screen: a full-screen app (vim, htop) needs those keys, and
    // it has no prompt marks to jump between anyway.
    if (mods.ctrl && !mods.shift && !mods.alt &&
        (vk == VK_UP || vk == VK_DOWN) && HasSession() &&
        !Foc().grid.AltActive())
    {
        JumpToMark(vk == VK_UP ? -1 : 1);
        return true;
    }

    // ---- global shortcuts (never forwarded to the remote shell) ----------
    if (vk == VK_F2)  { m_vsync = !m_vsync; UpdateMenuChecks(); return true; }
    if (vk == VK_F3)  { m_showOverlay = !m_showOverlay; UpdateMenuChecks(); return true; }
    if (vk == VK_F11) { ToggleFullscreen(); return true; }
    if (mods.ctrl && !mods.shift && (vk == VK_OEM_PLUS || vk == VK_ADD))
    {
        UpdateFontMetrics(m_fontPx + 1.0f);
        return true;
    }
    if (mods.ctrl && !mods.shift && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT))
    {
        UpdateFontMetrics(m_fontPx - 1.0f);
        return true;
    }
    if (mods.alt && vk == VK_RETURN)
    {
        if (!HasSession() || Cur().profile.altEnterFullscreen)   // Behaviour page
            ToggleFullscreen();
        return true;
    }
    if (mods.alt && vk == VK_SPACE && HasSession() && Cur().profile.altSpaceMenu)
    {
        OpenAppMenu();                                            // Behaviour page
        m_swallowChar = true;
        return true;
    }

    if (mods.ctrl && mods.shift && vk == 'T')
    {
        if (ShowConnectionDialog())
            UpdateGridDims();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'W')
    {
        if (HasSession())
        {
            bool live = Cur().Live();
            if (!live || !Cur().profile.warnOnClose ||
                MessageBoxW(m_hwnd, L"This session is still connected. Close it?",
                            L"AmberSSH", MB_YESNO | MB_ICONQUESTION) == IDYES)
                CloseSession(m_active);
        }
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && vk == VK_TAB)
    {
        CycleTab(mods.shift ? -1 : 1);
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && !mods.shift && vk >= '1' && vk <= '9')
    {
        SelectTab(static_cast<int>(vk - '1'));
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && vk == 'N')
    {
        if (ShowConnectionDialog())
            UpdateGridDims();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'F')
    {
        SearchScrollbackPrompt();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && !mods.shift && vk == 'G')
    {
        SearchNext();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'L')
    {
        ToggleLogging();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'B')
    {
        OpenSftpPanel();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'E')
    {
        SplitPane(true);
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift && vk == 'U')
    {
        SplitPane(false);
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && mods.shift &&
        (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN))
    {
        // Move focus by geometry — the pane that looks that way, whatever
        // the tree nesting happens to be. Alt as well moves the PANE rather
        // than the focus, and Ctrl+Shift+Alt+Shift is not a chord anyone can
        // type, so resize gets its own keys below.
        if (HasSession() && Cur().layout.Count() > 1)
        {
            using D = amber::PaneLayout::Dir;
            const D d = vk == VK_LEFT ? D::Left : vk == VK_RIGHT ? D::Right
                        : vk == VK_UP ? D::Up : D::Down;
            if (mods.alt)
                MovePane(d);
            else
                FocusPane(d);
            m_swallowChar = true;
            return true;
        }
    }

    if (!HasSession())
        return false;

    // A closed session: any key offers a fresh connection.
    if (!Cur().Live())
    {
        if (vk == VK_RETURN || vk == VK_SPACE)
        {
            if (ShowConnectionDialog())
                UpdateGridDims();
            m_swallowChar = true;
            return true;
        }
        return false;
    }

    if (mods.ctrl && mods.shift)
    {
        if (vk == 'C') { CopySelection(); m_swallowChar = true; return true; }
        if (vk == 'V') { Paste(); m_swallowChar = true; return true; }
        // Ctrl+Shift+B opens the picker, and is the emergency stop while
        // broadcasting: the same key both arms and disarms, so there is
        // always one chord that stops it without hunting a menu.
        if (vk == 'B')
        {
            if (HasSession() && !Cur().broadcast.empty())
                StopBroadcast();
            else
                PickBroadcastTargets();
            m_swallowChar = true;
            return true;
        }
        // Pane commands, all on the Ctrl+Shift convention so nothing a remote
        // application expects from a plain Ctrl combination is taken.
        if (vk == 'Z') { ToggleZoomPane();    m_swallowChar = true; return true; }
        if (vk == 'R') { ToggleReadOnlyPane(); m_swallowChar = true; return true; }
        if (vk == VK_OEM_6) { FocusPaneCycle(1);  m_swallowChar = true; return true; }
        if (vk == VK_OEM_4) { FocusPaneCycle(-1); m_swallowChar = true; return true; }
        if (vk == 'D')
        {
            Foc().userClosed = true;   // never auto-reconnect a manual close
            Foc().guardian.OnUserDisconnect();
            Foc().ssh.Disconnect();
            Foc().state = amber::SessionState::Disconnected;
            Foc().status = "disconnected";
            m_swallowChar = true;
            return true;
        }
    }
    if (mods.shift && vk == VK_PRIOR)
    {
        Foc().grid.ScrollView(static_cast<int>(m_gm.rows) - 1);
        return true;
    }
    if (mods.shift && vk == VK_NEXT)
    {
        Foc().grid.ScrollView(-(static_cast<int>(m_gm.rows) - 1));
        return true;
    }
    if (vk == VK_INSERT && mods.shift)
    {
        Paste();
        m_swallowChar = true;
        return true;
    }
    if (mods.ctrl && !mods.shift && vk == VK_SPACE)
    {
        SendToShell(std::string(1, '\0'));
        m_swallowChar = true;
        return true;
    }

    // Telnet page: keyboard sends Telnet special commands.
    if (Foc().profile.protocol == amber::Protocol::Telnet && Foc().profile.telnetKeyboard &&
        mods.ctrl && !mods.shift && !mods.alt)
    {
        uint8_t cmd = vk == 'C' ? 244 : vk == 'Z' ? 237 : vk == 'D' ? 236 : 0;
        if (cmd)
        {
            Foc().ssh.SendTelnetCommand(cmd);
            m_swallowChar = true;
            return true;
        }
    }

    // Keyboard page: Backspace code, Home/End and function-key flavours,
    // application cursor / keypad modes (the Features page can veto them).
    KeyOptions kopt;
    {
        const amber::ConnectionProfile& kp = Foc().profile;
        const TermModes& km = Foc().parser.Modes();
        kopt.appCursorKeys = km.appCursorKeys && kp.allowAppCursor;
        kopt.appKeypad = km.appKeypad && kp.allowAppKeypad;
        kopt.backspaceIsDel = kp.backspaceIsDel;
        kopt.homeEnd = static_cast<int>(kp.homeEnd);
        kopt.fnKeys = static_cast<int>(kp.fnKeys);
    }
    std::string seq = TranslateKey(static_cast<unsigned>(vk), mods, kopt);
    if (!seq.empty())
    {
        // Backspace/Delete blow a shockwave through the text.
        if (vk == VK_BACK || vk == VK_DELETE)
            TriggerShockwave();
        // Application-keypad keys would also arrive as WM_CHAR digits.
        if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
            m_swallowChar = true;
        // Typing returns the view to the live bottom (Window page).
        if (Foc().profile.scrollOnKey)
            Foc().grid.SnapView();
        if (HandleLocalLine(Foc(), seq))
            return true;
        SendToShell(seq);
        return true;
    }
    return false;
}

bool App::CellFromPx(int px, int py, int& row, int& col) const
{
    col = static_cast<int>((px - m_gm.originX) / m_gm.cellW);
    row = static_cast<int>((py - m_gm.originY) / m_gm.cellH);
    bool inside = col >= 0 && col < static_cast<int>(m_gm.cols) &&
                  row >= 0 && row < static_cast<int>(m_gm.rows);
    col = std::clamp(col, 0, static_cast<int>(m_gm.cols) - 1);
    row = std::clamp(row, 0, static_cast<int>(m_gm.rows) - 1);
    // With output folded, the row the user pointed at is not the row the grid
    // holds. Translating here keeps selection, links and mouse reporting
    // aligned with what is actually drawn, because every one of them goes
    // through this helper.
    if (HasSession())
    {
        const amber::Session& s = Foc();
        if (!s.rowMap.empty())
        {
            int co = 0, ro = 0;
            PaneOffset(const_cast<amber::Session&>(s), co, ro);
            const int local = row - ro;
            if (local >= 0 && local < static_cast<int>(s.rowMap.size()))
                row = s.rowMap[static_cast<size_t>(local)].src + ro;
        }
    }
    return inside;
}

bool App::MouseReport(int px, int py, int evt, int btn)
{
    if (!HasSession())
        return false;
    amber::Session& F = Foc();
    if (F.state != amber::SessionState::Connected)
        return false;
    const TermModes& tm = F.parser.Modes();
    if (tm.mouseMode == 0)
        return false;
    if ((GetKeyState(VK_SHIFT) & 0x8000) && F.profile.shiftOverridesMouse)
        return false;                 // Selection page: Shift overrides the app
    if (evt == 2 && tm.mouseMode == 1000)
        return false;                 // clicks-only protocol: no motion
    if (evt == 2 && tm.mouseMode == 1002 && m_mouseBtnDown < 0)
        return false;                 // drag protocol: motion needs a button

    int r, c;
    bool inside = CellFromPx(px, py, r, c);   // clamps r/c either way
    if (!inside && (evt == 0 || evt == 3 || evt == 4))
        return false;                 // presses/wheel must be in the grid
    int co = 0, ro = 0;
    PaneOffset(F, co, ro);
    r -= ro;
    c -= co;
    if ((r < 0 || c < 0 || r >= F.grid.Rows() || c >= F.grid.Cols()) &&
        (evt == 0 || evt == 3 || evt == 4))
        return false;
    // Captured drag/release beyond the pane: clamp so the app still gets a
    // coherent stream (and the press is always closed out).
    r = std::clamp(r, 0, std::max(0, F.grid.Rows() - 1));
    c = std::clamp(c, 0, std::max(0, F.grid.Cols() - 1));
    if (evt == 2)
    {
        if (r == m_mouseRepR && c == m_mouseRepC)
            return true;              // consumed — same cell, no report spam
        m_mouseRepR = r;
        m_mouseRepC = c;
    }

    int cb = btn;
    if (evt == 2)
        cb = ((m_mouseBtnDown >= 0) ? m_mouseBtnDown : 3) + 32;
    else if (evt == 3)
        cb = 64;                      // wheel up
    else if (evt == 4)
        cb = 65;                      // wheel down
    if (GetKeyState(VK_MENU) & 0x8000)
        cb += 8;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        cb += 16;

    const bool release = (evt == 1);
    char buf[48];
    if (tm.mouseSgr)
    {
        snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", cb, c + 1, r + 1,
                 release ? 'm' : 'M');
    }
    else
    {
        // Legacy X10 bytes: release reports button 3; coords cap at 223.
        if (release)
            cb = (cb & ~3) | 3;
        int bx = std::min(33 + c, 255);
        int by = std::min(33 + r, 255);
        snprintf(buf, sizeof(buf), "\x1b[M%c%c%c",
                 static_cast<char>(32 + cb), static_cast<char>(bx),
                 static_cast<char>(by));
    }
    SendToShell(buf);
    return true;
}

void App::OnMouseButton(bool down, int px, int py, bool rightButton, bool middleButton)
{
    m_lastInputTime = m_time;
    if (!HasSession())
        return;
    if (VncMouseButton(down, px, py, rightButton, middleButton))
        return;

    if (down)
    {
        // Status-bar chips come first: they sit outside the terminal grid, so
        // a click there must never reach the shell or start a selection.
        int chip = StatusChipAt(px, py);
        if (chip >= 0)
        {
            HandleMenuCommand(m_sbChips[chip].cmd);
            return;
        }
        if (m_statusBarH > 0.0f &&
            static_cast<float>(py) >=
                static_cast<float>(m_device.Height()) - m_statusBarH)
            return;                    // the bar's own ground swallows clicks

        // A click on a fold's summary line opens it. The summary is chrome,
        // not output, so this must happen before selection starts.
        if (!rightButton && !middleButton)
        {
            int fold = FoldSummaryAtPx(px, py);
            if (fold >= 0 && HasSession())
            {
                Foc().blocks[static_cast<size_t>(fold)].collapsed = false;
                SetStatus("Expanded output");
                return;
            }
        }

        // Tab bar hit test comes before everything terminal-bound.
        int tab = TabHitTest(px, py);
        if (tab >= 0)
        {
            SelectTab(tab);
            return;
        }
        if (py < static_cast<int>(m_tabBarH))
            return;

        // Terminal mouse reporting (vim / tmux / htop / lazygit). Shift
        // bypasses inside MouseReport so local selection and right-click
        // paste always stay reachable, like every other terminal.
        int btn = rightButton ? 2 : middleButton ? 1 : 0;
        if (MouseReport(px, py, 0, btn))
        {
            m_mouseBtnDown = btn;
            m_mouseRepR = m_mouseRepC = -1;
            SetCapture(m_hwnd);
            return;
        }

        // Selection page: what the right and middle buttons do.
        if (rightButton || middleButton)
        {
            const amber::MouseButtons mb = Cur().profile.mouseButtons;
            enum { PasteAct, ExtendAct, MenuAct } act;
            if (mb == amber::MouseButtons::Xterm)
                act = rightButton ? ExtendAct : PasteAct;
            else if (mb == amber::MouseButtons::Windows)
                act = rightButton ? MenuAct : ExtendAct;
            else
                act = rightButton ? PasteAct : ExtendAct;   // Compromise
            if (act == PasteAct)
                Paste();
            else if (act == MenuAct)
                ShowContextMenu(px, py);
            else
            {
                amber::Session& X = Foc();
                int r, c;
                CellFromPx(px, py, r, c);
                int co = 0, ro = 0;
                PaneOffset(X, co, ro);
                X.selEndR = std::clamp(r - ro, 0, std::max(0, X.grid.Rows() - 1));
                X.selEndC = std::clamp(c - co, 0, std::max(0, X.grid.Cols() - 1));
                X.selActive = true;
                if (X.profile.autoCopy)
                    CopySelection();
            }
            return;
        }

        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (alt && shiftHeld)
        {
            // Shift+Alt+drag drives the particle force field (plain Alt+drag
            // is the other selection mode, like PuTTY).
            m_forceDrag = true;
            m_particles.tun.mouseForce = m_fxPointerForce ? 2600.0f : 0.0f;
            m_particles.tun.mouseX = static_cast<float>(px);
            m_particles.tun.mouseY = static_cast<float>(py);
            SetCapture(m_hwnd);
            return;
        }

        int r, c;
        CellFromPx(px, py, r, c);

        // Pane focus follows the click; selection runs in that pane's local
        // grid coordinates. A click on a divider changes nothing — it starts
        // a drag instead, handled above.
        if (Cur().layout.Count() > 1)
        {
            const amber::PaneId hit =
                Cur().layout.PaneAt(c, r, static_cast<int>(m_gm.cols),
                                    static_cast<int>(m_gm.rows));
            if (hit != amber::kNoPane && PaneById(Cur(), hit))
                Cur().focus = hit;
        }
        amber::Session& F = Foc();
        int co = 0, ro = 0;
        PaneOffset(F, co, ro);
        r = std::clamp(r - ro, 0, std::max(0, F.grid.Rows() - 1));
        c = std::clamp(c - co, 0, std::max(0, F.grid.Cols() - 1));

        // Ctrl+click opens a URL under the cell; Ctrl+Shift+click selects
        // the whole command-output block (shell integration tide marks).
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            if (GetKeyState(VK_SHIFT) & 0x8000)
                SelectBlockAt(r, c);
            else
                OpenUrlAt(r, c);
            return;
        }

        F.selecting = true;
        F.selActive = true;
        F.selRect = alt != F.profile.rectSelectDefault;   // Alt+drag = the other mode
        F.selStartR = F.selEndR = r;
        F.selStartC = F.selEndC = c;
        F.selAnimStart = m_lastFrameTime;   // Miami gradient ease-in
        SetCapture(m_hwnd);
    }
    else
    {
        if (m_mouseBtnDown >= 0)
        {
            // Close out the reported press even if the pointer left the pane.
            MouseReport(px, py, 1, m_mouseBtnDown);
            m_mouseBtnDown = -1;
            ReleaseCapture();
            return;
        }
        if (m_forceDrag)
        {
            m_forceDrag = false;
            m_particles.tun.mouseForce = 0.0f;
            ReleaseCapture();
            return;
        }
        amber::Session& F = Foc();
        if (F.selecting)
        {
            F.selecting = false;
            ReleaseCapture();
            if (F.selStartR == F.selEndR && F.selStartC == F.selEndC)
            {
                F.selActive = false;   // click without drag clears selection
            }
            else if (F.profile.autoCopy)
            {
                // Releasing the button copies automatically — no Copy step.
                CopySelection();
            }
        }
    }
}

// OSC 8: the destination of the link under the pointer, held for the status
// bar. Shown because a hyperlink whose visible text says one thing and whose
// target says another is the oldest trick there is, and a terminal that hides
// the target makes Ctrl+click a guess.
//
// The URI was already refused at parse time if it carried control characters,
// so what is stored here is safe to draw; it is still clipped for length.
void App::UpdateLinkHover(amber::Session& s, int px, int py)
{
    m_hoverLink.clear();
    int r = 0, c = 0;
    if (!CellFromPx(px, py, r, c))
        return;
    int co = 0, ro = 0;
    PaneOffset(s, co, ro);
    const int row = r - ro, col = c - co;
    if (row < 0 || row >= s.grid.Rows() || col < 0 || col >= s.grid.Cols())
        return;
    const uint16_t id = s.grid.ViewCell(row, col).link;
    if (!id)
        return;
    const std::string& uri = s.parser.LinkUri(id);
    if (uri.empty())
        return;
    // Whether Ctrl+click would actually open it is part of the readout: a
    // scheme AmberSSH will not launch should say so before it is clicked, not
    // after.
    const bool openable = uri.rfind("http://", 0) == 0 ||
                          uri.rfind("https://", 0) == 0 ||
                          uri.rfind("mailto:", 0) == 0;
    m_hoverLink = (openable ? "link: " : "link (will not open): ") +
                  (uri.size() > 160 ? uri.substr(0, 159) + "\xE2\x80\xA6" : uri);
}

void App::OnMouseMove(int px, int py)
{
    m_lastMousePx = px;
    m_lastMousePy = py;
    m_lastInputTime = m_time;
    // Chip hover: tracked before the session check so the bar stays live even
    // with no session open.
    m_sbHover = StatusChipAt(px, py);
    if (!HasSession())
        return;
    if (VncMouseMove(px, py))
        return;

    if (m_forceDrag)
    {
        m_particles.tun.mouseX = static_cast<float>(px);
        m_particles.tun.mouseY = static_cast<float>(py);
        return;
    }
    if (m_mouseBtnDown >= 0)
    {
        MouseReport(px, py, 2, m_mouseBtnDown);   // reported drag
        return;
    }
    amber::Session& F = Foc();
    // Any-motion protocol (?1003): hover reports even with no button held.
    if (!F.selecting && F.parser.Modes().mouseMode == 1003)
    {
        if (MouseReport(px, py, 2, 3))
            return;
    }
    UpdateLinkHover(F, px, py);
    if (!F.selecting)
        return;

    int r, c;
    CellFromPx(px, py, r, c);
    int co = 0, ro = 0;
    PaneOffset(F, co, ro);
    F.selEndR = std::clamp(r - ro, 0, std::max(0, F.grid.Rows() - 1));
    F.selEndC = std::clamp(c - co, 0, std::max(0, F.grid.Cols() - 1));

    // Auto-scroll when dragging past the top or bottom edge.
    if (py < static_cast<int>(m_gm.originY))
        F.grid.ScrollView(1);
    else if (py > static_cast<int>(m_gm.originY + m_gm.rows * m_gm.cellH))
        F.grid.ScrollView(-1);
}

int App::TabHitTest(int px, int py) const
{
    if (py < 0 || py >= static_cast<int>(m_tabBarH))
        return -1;
    for (size_t i = 0; i < m_tabRects.size(); ++i)
    {
        const TabRect& t = m_tabRects[i];
        if (px >= t.x && px < t.x + t.w)
            return static_cast<int>(i);
    }
    return -1;
}

void App::OnWheel(int delta, bool ctrl)
{
    m_lastInputTime = m_time;
    m_wheelAccum += delta;
    int notches = m_wheelAccum / WHEEL_DELTA;
    if (notches == 0)
        return;
    m_wheelAccum -= notches * WHEEL_DELTA;

    if (ctrl)
    {
        // Live font-size adjust: regenerate metrics, resize grid, notify SSH.
        UpdateFontMetrics(m_fontPx + static_cast<float>(notches));
        return;
    }
    // Time dial: hold Alt and scroll to slow the effects down (or speed them
    // up) without touching the Motion Speed setting. Useful for watching what
    // a style is actually doing, and for showing it off.
    if ((GetKeyState(VK_MENU) & 0x8000) != 0)
    {
        m_timeDial = std::clamp(
            m_timeDial * std::pow(1.25f, static_cast<float>(notches)), 0.05f, 4.0f);
        char msg[64];
        snprintf(msg, sizeof(msg), "Time dial %.2fx%s", m_timeDial,
                 (m_timeDial > 0.98f && m_timeDial < 1.02f) ? " (normal)" : "");
        SetStatus(msg, 2.0);
        return;
    }
    // Wheel goes to the remote app when it asked for mouse reporting
    // (vim / less / htop scroll natively); otherwise local scrollback.
    if (HasSession())
    {
        int evt = notches > 0 ? 3 : 4;
        bool reported = false;
        for (int i = 0; i < std::abs(notches); ++i)
            reported = MouseReport(m_lastMousePx, m_lastMousePy, evt, 0) ||
                       reported;
        if (reported)
            return;
        Foc().grid.ScrollView(notches * 3);
    }
}

// The Windows clipboard changed. Only the session the user is looking at is
// offered the text, and only if its profile asked for that direction: a copy
// meant for one host has no business reaching every other host that happens
// to be connected. AmberSSH's own copies land here too — the text is the same
// either way, and re-offering it costs one comparison.
void App::OnWindowsClipboardChanged()
{
    if (!HasSession())
        return;
    amber::Session& s = Cur();
    const int mode = s.profile.x11Clipboard;
    if (mode != 1 && mode != 3 && mode != 4)
        return;
    if (s.profile.x11Backend != 1 || s.state != amber::SessionState::Connected)
        return;

    // Reading it is a UI-thread operation and may fail while another
    // application holds the clipboard; a failure is simply "not this time".
    if (!OpenClipboard(m_hwnd))
        return;
    std::string utf8;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
    {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h)))
        {
            utf8 = Utf8FromWide(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (utf8.empty() || utf8 == m_lastClipboardSent)
        return;
    if (utf8.size() > (1u << 20))
    {
        SetStatus("Clipboard: too large to share with the session");
        return;
    }
    if (mode == 1 && !amber::ShowClipboardDialog(m_hwnd, s.profile.host, true, utf8.size()))
        return;
    m_lastClipboardSent = utf8;
    s.ssh.OfferClipboard(utf8);
}

void App::SetClipboardText(const std::string& utf8)
{
    std::wstring wide = WideFromUtf8(utf8);
    if (!OpenClipboard(m_hwnd))
        return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    if (mem)
    {
        wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
        memcpy(dst, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

void App::CopySelection()
{
    amber::Session& F = Foc();
    if (!F.selActive)
        return;
    std::string text;
    if (F.selRect)
    {
        // Rectangular selection: the column band of each row, one line each.
        int r0, c0, r1, c1;
        F.SelectionBounds(r0, c0, r1, c1);
        int ca = std::min(F.selStartC, F.selEndC), cb = std::max(F.selStartC, F.selEndC);
        for (int r = r0; r <= r1; ++r)
        {
            std::string line = F.grid.GetText(r, ca, r, cb);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            text += line;
            if (r < r1)
                text += "\n";
        }
    }
    else
        text = F.grid.GetText(F.selStartR, F.selStartC, F.selEndR, F.selEndC);
    SetClipboardText(text);
    char msg[64];
    snprintf(msg, sizeof(msg), "Copied %zu characters", text.size());
    SetStatus(msg);
}

void App::Paste()
{
    if (!OpenClipboard(m_hwnd))
        return;
    std::string utf8;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
    {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h)))
        {
            utf8 = Utf8FromWide(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (utf8.empty())
        return;

    // Normalize newlines to CR for the pty, and drop variation selectors
    // (U+FE0E/U+FE0F = EF B8 8E/8F): line editors without combining-char
    // support echo the invisible selector as "<fe0f>" noise, while every
    // mainstream renderer draws the emoji base identically without it.
    std::string norm;
    norm.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i)
    {
        unsigned char b = static_cast<unsigned char>(utf8[i]);
        if (b == 0xEF && i + 2 < utf8.size() &&
            static_cast<unsigned char>(utf8[i + 1]) == 0xB8 &&
            (static_cast<unsigned char>(utf8[i + 2]) == 0x8E ||
             static_cast<unsigned char>(utf8[i + 2]) == 0x8F))
        {
            i += 2;
            continue;
        }
        if (utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n')
        {
            norm.push_back('\r');
            ++i;
        }
        else if (utf8[i] == '\n')
            norm.push_back('\r');
        else
            norm.push_back(utf8[i]);
    }

    // Smart paste guard. A newline in pasted text submits whatever precedes
    // it the instant it lands, so multi-line pastes are previewed and
    // confirmed. Bracketed paste does NOT make this safe: plenty of shells and
    // full-screen apps do not enable it, and the ones that do still run the
    // text the moment you press Enter.
    if (!m_pasteOpen && amber::PasteNeedsConfirm(norm, m_pasteGuard))
    {
        m_pastePending = norm;
        m_pasteLines = amber::PasteLineCount(norm);
        m_pasteOpen = true;
        m_palOpen = false;
        m_jrnOpen = false;
        return;
    }
    SendPasteText(norm);
}

void App::SendPasteText(const std::string& norm)
{
    if (!HasSession())
        return;
    // A desktop tab has no terminal to paste into: the text goes to the
    // server's clipboard as RFB cut text, under the profile's clipboard policy.
    if (VncActive())
    {
        VncSendClipboardText(norm);
        return;
    }
    // Broadcasting a multi-line paste to several hosts at once is the single
    // most destructive thing this application can be asked to do: every line
    // runs the moment it lands, on every target, simultaneously. The existing
    // paste guard has already previewed the text; this is a second, explicit
    // confirmation that names the count, and it is deliberately a modal that
    // defaults to "No".
    const amber::Session& tab = Cur();
    const size_t targets = tab.broadcast.size();
    if (targets >= 3 && amber::PasteLineCount(norm) > 1)
    {
        std::string names;
        for (amber::PaneId id : tab.broadcast)
        {
            const amber::Session* p = PaneById(tab, id);
            if (!p)
                continue;
            names += "\r\n    " + p->Caption();
        }
        const std::wstring body =
            L"You are about to run " +
            std::to_wstring(amber::PasteLineCount(norm)) +
            L" lines on " + std::to_wstring(targets) + L" hosts at once:\r\n" +
            WideFromUtf8(names) +
            L"\r\n\r\nEvery line runs as it arrives, on all of them. Continue?";
        if (MessageBoxW(m_hwnd, body.c_str(),
                        L"AmberSSH — broadcast a multi-line paste?",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            SetStatus("Broadcast paste cancelled.", 5.0);
            return;
        }
    }
    if (Cur().parser.Modes().bracketedPaste)
        SendToShell("\x1b[200~" + norm + "\x1b[201~");
    else
        SendToShell(norm);
}

void App::ToggleFullscreen()
{
    if (!m_fullscreen)
    {
        m_savedStyle = GetWindowLongW(m_hwnd, GWL_STYLE);
        GetWindowRect(m_hwnd, &m_savedRect);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetMenu(m_hwnd, nullptr);            // menu bar hides in fullscreen
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_fullscreen = true;
    }
    else
    {
        SetWindowLongW(m_hwnd, GWL_STYLE, m_savedStyle);
        if (m_menu)
            SetMenu(m_hwnd, m_menu);
        SetWindowPos(m_hwnd, nullptr, m_savedRect.left, m_savedRect.top,
                     m_savedRect.right - m_savedRect.left,
                     m_savedRect.bottom - m_savedRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        m_fullscreen = false;
    }
    UpdateMenuChecks();
}

// ------------------------------------------------------------------- theming
// "hostThemes" rules: comma-separated "pattern=themeId" pairs; '*' wildcards.
// First match wins; -1 when nothing matches.
int App::MatchHostTheme(const std::string& host) const
{
    if (m_hostThemes.empty() || host.empty())
        return -1;
    auto lower = [](std::string s) {
        for (char& ch : s)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    // Wildcard match: '*' spans any run of characters.
    std::function<bool(const char*, const char*)> wild =
        [&](const char* p, const char* s) -> bool {
        for (; *p; ++p, ++s)
        {
            if (*p == '*')
            {
                while (*p == '*')
                    ++p;
                if (!*p)
                    return true;
                for (; *s; ++s)
                    if (wild(p, s))
                        return true;
                return false;
            }
            if (!*s || *p != *s)
                return false;
        }
        return !*s;
    };
    std::string h = lower(host);
    size_t pos = 0;
    while (pos < m_hostThemes.size())
    {
        size_t comma = m_hostThemes.find(',', pos);
        std::string rule = m_hostThemes.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? m_hostThemes.size() : comma + 1;
        size_t eq = rule.find('=');
        if (eq == std::string::npos)
            continue;
        std::string pat = lower(rule.substr(0, eq));
        int id = atoi(rule.c_str() + eq + 1);
        if (id >= 0 && id < kThemeCount - 1 && wild(pat.c_str(), h.c_str()))
            return id;
    }
    return -1;
}

void App::ApplyTheme()
{
    ApplyChromeFace();
    m_themeId = std::clamp(m_themeId, 0, kThemeCount - 1);
    // The active session's danger-theme override (per-host rule) beats the
    // user's chosen theme while that tab is in front.
    int effective = m_themeId;
    if (HasSession() && Cur().themeOverride >= 0)
        effective = std::clamp(Cur().themeOverride, 0, kThemeCount - 1);
    uint32_t stops[5];
    // Presets occupy 0..kThemeCount-2; the last id is always Custom. This must
    // derive from the table, or adding a preset silently routes it to Custom.
    if (effective < kThemeCount - 1)
    {
        for (int i = 0; i < 5; ++i)
            stops[i] = kThemes[effective].stops[i];
    }
    else
    {
        // Custom: three picked colors (glow, core, highlight) expanded to the
        // 5-stop ramp shape. Stop 0 is the glow at ~40%, stop 3 the midpoint
        // of core and highlight — all blended in sRGB, close enough for UI.
        auto scale = [](uint32_t c, float f) -> uint32_t
        {
            auto ch = [&](int s) {
                return static_cast<uint32_t>(
                    std::min(255.0f, ((c >> s) & 0xFF) * f));
            };
            return (ch(16) << 16) | (ch(8) << 8) | ch(0);
        };
        auto mix = [](uint32_t a, uint32_t b) -> uint32_t
        {
            auto ch = [&](int s) {
                return (((a >> s) & 0xFF) + ((b >> s) & 0xFF)) / 2;
            };
            return (ch(16) << 16) | (ch(8) << 8) | ch(0);
        };
        stops[0] = scale(m_customTheme[0], 0.42f);
        stops[1] = m_customTheme[0];
        stops[2] = m_customTheme[1];
        stops[3] = mix(m_customTheme[1], m_customTheme[2]);
        stops[4] = m_customTheme[2];
    }

    // Night shift: after dark the phosphor drifts towards ember. Applied to
    // the STOPS, so the particle ramp, the crisp glyph cores and every dialog
    // warm together instead of drifting apart.
    if (const float ns = NightShiftAmount(); ns > 0.001f)
    {
        for (uint32_t& s : stops)
        {
            const float r = static_cast<float>((s >> 16) & 0xFF);
            const float g = static_cast<float>((s >> 8) & 0xFF);
            const float b = static_cast<float>(s & 0xFF);
            // Hold the reds, pull the greens back a little and the blues a
            // lot — the same direction a tube takes as it ages, and the same
            // one f.lux moves a display at night.
            const float g2 = g * (1.0f - 0.18f * ns);
            const float b2 = b * (1.0f - 0.55f * ns);
            s = (static_cast<uint32_t>(r) << 16) |
                (static_cast<uint32_t>(g2 + 0.5f) << 8) |
                static_cast<uint32_t>(b2 + 0.5f);
        }
    }

    for (int i = 0; i < 5; ++i)
    {
        float lin[3];
        SrgbToLinear(stops[i], lin);
        for (int c = 0; c < 3; ++c)
        {
            gThemeStops[i][c] = lin[c];
            m_particles.tun.ramp[i][c] = lin[c];
        }
        gThemeSrgb[i] = stops[i];   // dialogs derive their palette from these
    }

    // Menu chrome straight from the sRGB stops.
    auto cr = [](uint32_t c) {
        return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    };
    auto crScale = [](uint32_t c, float f) {
        auto ch = [&](int s) {
            return static_cast<BYTE>(
                std::min(255.0f, ((c >> s) & 0xFF) * f));
        };
        return RGB(ch(16), ch(8), ch(0));
    };
    m_menuColors.bg      = crScale(stops[0], 0.55f);  // popup dropdown ground
    m_menuColors.sel     = crScale(stops[1], 0.85f);
    m_menuColors.text    = crScale(stops[3], 0.90f);
    m_menuColors.textHot = cr(stops[4]);
    m_menuColors.dim     = crScale(stops[2], 0.60f);
    m_menuColors.border  = crScale(stops[2], 0.45f);
    // The top-level bar is painted via the UAH messages: a mid-amber strip
    // with dark labels, flipping to the dark dropdown ground + bright text on
    // hover. barBg is also the MENUINFO fallback brush.
    m_menuColors.barBg   = crScale(stops[2], 0.62f);
    m_menuColors.barText = crScale(stops[0], 0.85f);

    // Skinned chrome (Cyberpunk, LCARS) owns its own palette, exactly as the
    // dialogs do through MakeDialogPalette — the hamburger dropdown is part of
    // the same surface, so it must not stay in terminal-theme amber.
    if (amber::ChromeSkinned())
    {
        const amber::ChromeSpec& ch = amber::Chrome();
        m_menuColors.bg      = amber::SrgbRef(ch.bg);
        m_menuColors.sel     = amber::SrgbRef(ch.neonA);
        m_menuColors.text    = amber::SrgbRef(ch.text);
        m_menuColors.dim     = amber::SrgbRef(ch.textDim);
        m_menuColors.border  = amber::SrgbRef(ch.border);
        // The selection is a saturated accent bar in every skin, so the ink on
        // it is chosen by that accent's own luma — black on cyan or orange,
        // white on a deep red. Assuming either one breaks half the skins.
        m_menuColors.textHot = amber::InkOn(ch.neonA);
        // barBg doubles as the MENUINFO brush, which paints the margin around
        // the item list — it has to match the item ground, not contrast it.
        m_menuColors.barBg   = amber::SrgbRef(ch.bg);
        m_menuColors.barText = amber::SrgbRef(ch.text);
    }

    // Rebuilt every time, not just once: the face is a property of the skin,
    // so switching Interface Style has to re-make it.
    if (m_menuFont)
        DeleteObject(m_menuFont);
    m_menuFont = CreateFontW(-MulDiv(15, static_cast<int>(m_dpi), 96), 0, 0,
                             0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, MenuFaceName());
    if (m_menuBgBrush)
        DeleteObject(m_menuBgBrush);
    m_menuBgBrush = CreateSolidBrush(m_menuColors.barBg);
    if (m_menu)
    {
        MENUINFO mi = { sizeof(mi) };
        mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
        mi.hbrBack = m_menuBgBrush;
        SetMenuInfo(m_menu, &mi);
        DrawMenuBar(m_hwnd);
    }
    if (m_hwnd)
    {
        amber::ApplyWindowChrome(m_hwnd);   // caption/border follow the theme
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

bool App::PromptCustomTheme()
{
    static const wchar_t* names[3] = { L"GLOW (dim base)", L"CORE (main text)",
                                       L"HIGHLIGHT (hot center)" };
    static COLORREF custom[16] = {};
    uint32_t picked[3];
    for (int i = 0; i < 3; ++i)
    {
        uint32_t c = m_customTheme[i];
        CHOOSECOLORW cc = { sizeof(cc) };
        cc.hwndOwner = m_hwnd;
        cc.rgbResult = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        SetStatus(std::string("Pick color ") + std::to_string(i + 1) +
                  " of 3: " + (i == 0 ? "glow" : i == 1 ? "core" : "highlight"));
        RenderFrame();   // show the status hint under the modal picker
        if (!ChooseColorW(&cc))
            return false;
        picked[i] = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 16) |
                    (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 8) |
                    GetBValue(cc.rgbResult);
    }
    for (int i = 0; i < 3; ++i)
        m_customTheme[i] = picked[i];
    return true;
}

// The face the menu draws in: the skin's own choice when it names one, else
// the system UI font. Nostromo asks for a monospace, Brass for a serif.
const wchar_t* App::MenuFaceName()
{
    if (amber::ChromeSkinned() && amber::ChromeFace())
        return amber::ChromeFace();
    return L"Segoe UI";
}

void App::ThemeMenuBar(HMENU menu, int depth)
{
    // Owner-draw the DROPDOWN items (depth >= 1); the top-level bar (depth 0)
    // is left standard — the themed Win32 menu bar sends no WM_DRAWITEM, so we
    // colour it through the MENUINFO brush instead and keep its default text.
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i)
    {
        MENUITEMINFOW ii = { sizeof(ii) };
        ii.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &ii))
            continue;
        HMENU sub = ii.hSubMenu;

        if (depth >= 1)
        {
            // Label travels in dwItemData (stable wstring in m_menuStrings) so
            // the draw handler has it for items with no command id.
            wchar_t buf[160] = L"";
            GetMenuStringW(menu, static_cast<UINT>(i), buf, 160, MF_BYPOSITION);
            m_menuStrings.push_back(std::make_unique<std::wstring>(buf));

            ii.fMask = MIIM_FTYPE | MIIM_DATA;
            ii.fType |= MFT_OWNERDRAW;
            ii.dwItemData =
                reinterpret_cast<ULONG_PTR>(m_menuStrings.back().get());
            SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &ii);
        }
        if (sub)
            ThemeMenuBar(sub, depth + 1);
    }
}

void App::MeasureMenuItem(MEASUREITEMSTRUCT& mis)
{
    if (!m_menuFont)
        m_menuFont = CreateFontW(-MulDiv(15, static_cast<int>(m_dpi), 96), 0, 0,
                                 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, MenuFaceName());
    const auto* str = reinterpret_cast<const std::wstring*>(mis.itemData);
    if (!str || str->empty())
    {
        // Separator.
        mis.itemHeight = MulDiv(9, static_cast<int>(m_dpi), 96);
        mis.itemWidth = MulDiv(60, static_cast<int>(m_dpi), 96);
        return;
    }
    // Measure the text as it will be DRAWN. An uppercase skin renders wider
    // than the source string, and measuring the original clips long labels.
    std::wstring shown = *str;
    if (amber::ChromeSkinned() && amber::Chrome().uppercase)
        for (wchar_t& wc : shown)
            wc = static_cast<wchar_t>(::towupper(wc));
    HDC dc = GetDC(m_hwnd);
    HGDIOBJ of = SelectObject(dc, m_menuFont);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, shown.c_str(), static_cast<int>(shown.size()), &sz);
    SelectObject(dc, of);
    ReleaseDC(m_hwnd, dc);
    mis.itemWidth = sz.cx + MulDiv(44, static_cast<int>(m_dpi), 96);
    mis.itemHeight = MulDiv(26, static_cast<int>(m_dpi), 96);
}

void App::DrawMenuItem(const DRAWITEMSTRUCT& dis)
{
    HDC dc = dis.hDC;
    RECT rc = dis.rcItem;
    HMENU menu = reinterpret_cast<HMENU>(dis.hwndItem);

    const bool sel = (dis.itemState & ODS_SELECTED) != 0 ||
                     (dis.itemState & ODS_HOTLIGHT) != 0;
    const bool disabled = (dis.itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool checked = (dis.itemState & ODS_CHECKED) != 0;

    const amber::ChromeSpec& ch = amber::Chrome();
    const bool skin = amber::ChromeSkinned();
    auto D = [&](int v) { return MulDiv(v, static_cast<int>(m_dpi), 96); };

    // The ground goes down first and the selection is drawn on top as a SHAPE,
    // so a skin can round it into an LCARS pill or chamfer it like the
    // Cyberpunk tabs instead of being stuck with a full-bleed rectangle.
    HBRUSH gb = CreateSolidBrush(m_menuColors.bg);
    FillRect(dc, &rc, gb);
    DeleteObject(gb);
    if (sel)
    {
        HBRUSH sb = CreateSolidBrush(m_menuColors.sel);
        if (skin && ch.pills)
        {
            RECT pr = { rc.left + D(3), rc.top + D(1),
                        rc.right - D(3), rc.bottom - D(1) };
            int r = pr.bottom - pr.top;
            HGDIOBJ ob = SelectObject(dc, sb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, pr.left, pr.top, pr.right, pr.bottom, r, r);
            SelectObject(dc, op);
            SelectObject(dc, ob);
        }
        else if (skin && ch.chamfer > 0.0f)
        {
            int c = D(static_cast<int>(ch.chamfer));
            POINT p[5] = { { rc.left, rc.top },
                           { rc.right, rc.top },
                           { rc.right, rc.bottom - c },
                           { rc.right - c, rc.bottom },
                           { rc.left, rc.bottom } };
            HGDIOBJ ob = SelectObject(dc, sb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, p, 5);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            // Neon rail down the leading edge, the same cue the tabs use.
            HBRUSH rb = CreateSolidBrush(amber::SrgbRef(ch.neonB));
            RECT rail = { rc.left, rc.top, rc.left + D(3), rc.bottom };
            FillRect(dc, &rail, rb);
            DeleteObject(rb);
        }
        else if (skin && ch.impression)
        {
            // Letterpress: hover is a blind deboss; only the checked item is
            // printed — an ink bar with paper-coloured type.
            RECT ir = { rc.left + D(3), rc.top, rc.right - D(3), rc.bottom };
            if (checked)
                FillRect(dc, &ir, sb);
            else
                amber::skin::Impress(dc, ir, amber::SrgbRef(ch.border), RGB(255, 255, 255));
        }
        else if (skin && ch.stitch)
        {
            // Atelier: a second-cut patch with a rivet beside the item.
            RECT ir = { rc.left + D(3), rc.top, rc.right - D(3), rc.bottom };
            HBRUSH hb = CreateSolidBrush(amber::SrgbRef(ch.bars[1]));
            FillRect(dc, &ir, hb);
            DeleteObject(hb);
            amber::skin::Rivet(dc, ir.right - D(10), (ir.top + ir.bottom) / 2, D(3),
                               amber::SrgbRef(ch.neonB), RGB(0x5C, 0x45, 0x26));
        }
        else if (skin && ch.meter)
        {
            // Reference: the item's LED lights.
            RECT ir = { rc.left + D(3), rc.top, rc.right - D(3), rc.bottom };
            HBRUSH hb = CreateSolidBrush(amber::SrgbRef(ch.bars[1]));
            FillRect(dc, &ir, hb);
            DeleteObject(hb);
            amber::skin::Led(dc, ir.right - D(10), (ir.top + ir.bottom) / 2, D(2),
                             amber::SrgbRef(ch.neonA), true);
        }
        else if (skin && ch.glaze)
        {
            // Tenmoku: the pool brightens under the pointer; the checked item
            // is chopped.
            RECT ir = { rc.left + D(3), rc.top, rc.right - D(3), rc.bottom };
            amber::skin::Pool(dc, ir, amber::SrgbRef(ch.bars[1]), amber::skin::kRust);
            if (checked)
                amber::skin::Chop(dc, ir.right - D(16), (ir.top + ir.bottom) / 2 - D(5),
                                  D(10), amber::SrgbRef(ch.danger), amber::SrgbRef(ch.bars[1]));
        }
        else if (skin && ch.rehaut)
        {
            // Horologe: the hovered item is an applied index — the dial ring
            // lifted one step, raised by a rhodium line above and a shade
            // below. Lume is kept for the checked (current) choice.
            RECT ir = { rc.left + D(3), rc.top, rc.right - D(3), rc.bottom };
            HBRUSH rb = CreateSolidBrush(amber::SrgbRef(checked ? ch.neonA : ch.bars[1]));
            FillRect(dc, &ir, rb);
            DeleteObject(rb);
            amber::skin::AppliedIndex(dc, ir, amber::SrgbRef(ch.text),
                                      amber::skin::Dim(amber::SrgbRef(ch.bg), 0.45f));
        }
        else
            FillRect(dc, &rc, sb);
        DeleteObject(sb);
    }

    // ---- the Direct2D finish over the selection ----------------------------
    if (sel && skin && ch.outline <= 0.0f)
    {
        RECT ir = { rc.left + D(3), rc.top, rc.right - D(3), rc.bottom };
        amber::finish::Pass p(dc, ir);
        if (p.ok())
        {
            if (ch.impression && !checked)
            {
                p.InnerShadow(ir, D(3), 0.16f);
                p.InnerLight(ir, D(2), 0.6f);
            }
            else if (ch.glaze)
                p.Radial(ir, RGB(255, 255, 255), 0.05f, RGB(0, 0, 0), 0.42f);
            else if (ch.stitch)
                p.Radial(ir, RGB(255, 255, 255), 0.05f, RGB(0, 0, 0), 0.26f);
            else if (ch.meter)
                p.Gradient(ir, RGB(255, 255, 255), 0.22f, RGB(0, 0, 0), 0.10f);
            else
                p.Sheen(ir, ch.pills ? 0.12f : 0.16f, 0.5f);
        }
    }
    (void)menu;
    const auto* str = reinterpret_cast<const std::wstring*>(dis.itemData);
    if (!str || str->empty())
    {
        // Separator: a hairline normally; skins get a solid accent bar, which
        // on LCARS is the rounded divider the panels use.
        // Pill skins have a coloured bar palette to divide with; the others
        // use their border colour, which reads on a near-black ground where
        // their (deliberately dark) bar colours would not.
        HBRUSH lb = CreateSolidBrush(
            !skin        ? m_menuColors.border
            : ch.pills   ? amber::SrgbRef(ch.bars[2])
                         : amber::SrgbRef(ch.border));
        int h = skin ? D(3) : 1;
        int mid = (rc.top + rc.bottom) / 2;
        RECT ln = { rc.left + D(8), mid - h / 2, rc.right - D(8), mid - h / 2 + h };
        if (skin && ch.pills)
        {
            HGDIOBJ ob = SelectObject(dc, lb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, ln.left, ln.top, ln.right, ln.bottom + 1, h, h);
            SelectObject(dc, op);
            SelectObject(dc, ob);
        }
        else
            FillRect(dc, &ln, lb);
        DeleteObject(lb);
        return;
    }

    SetBkMode(dc, TRANSPARENT);
    // Horologe letters in rhodium on the hovered index; only a checked item
    // sits on lume, where the ink is dark.
    const bool ownInk = skin && ((ch.rehaut || ch.impression) ? !checked
                                                              : (ch.stitch || ch.meter || ch.glaze));
    const COLORREF hot = ownInk ? m_menuColors.text : m_menuColors.textHot;
    SetTextColor(dc, disabled ? m_menuColors.dim : sel ? hot : m_menuColors.text);
    HGDIOBJ of = SelectObject(dc, m_menuFont);

    // Split "label\taccelerator".
    std::wstring label = *str, accel;
    if (size_t tab = label.find(L'\t'); tab != std::wstring::npos)
    {
        accel = label.substr(tab + 1);
        label.resize(tab);
    }
    if (skin && ch.uppercase)
        for (wchar_t& wc : label)
            wc = static_cast<wchar_t>(::towupper(wc));

    // All owner-drawn items are dropdown items: left-aligned past the check
    // gutter, with any accelerator right-aligned in the dim tint.
    RECT tr = rc;
    tr.left += MulDiv(22, static_cast<int>(m_dpi), 96);
    tr.right -= MulDiv(12, static_cast<int>(m_dpi), 96);
    DrawTextW(dc, label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!accel.empty())
    {
        SetTextColor(dc, sel ? m_menuColors.textHot : m_menuColors.dim);
        DrawTextW(dc, accel.c_str(), -1, &tr,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (checked)
    {
        // Radio dot / check block in the gutter. On a skin whose highlight
        // text is black, an unselected row would draw a black pip on the dark
        // ground — invisible — so unselected rows use the accent instead.
        SetTextColor(dc, (skin && !sel) ? amber::SrgbRef(ch.neonA)
                                        : m_menuColors.textHot);
        RECT gr = { rc.left, rc.top,
                    rc.left + MulDiv(22, static_cast<int>(m_dpi), 96),
                    rc.bottom };
        DrawTextW(dc, L"●", -1, &gr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of);
}

void App::DrawMenuBarBg(void* uahMenu)
{
    auto* um = static_cast<UAHMENU*>(uahMenu);
    MENUBARINFO mbi = { sizeof(mbi) };
    if (!GetMenuBarInfo(m_hwnd, OBJID_MENU, 0, &mbi))
        return;
    RECT wr;
    GetWindowRect(m_hwnd, &wr);
    RECT bar = mbi.rcBar;
    OffsetRect(&bar, -wr.left, -wr.top);   // screen → window-DC coords
    FillRect(um->hdc, &bar, m_menuBgBrush);
}

void App::DrawMenuBarItem(void* uahDrawItem)
{
    auto* udm = static_cast<UAHDRAWMENUITEM*>(uahDrawItem);
    wchar_t text[128] = L"";
    MENUITEMINFOW mii = { sizeof(mii) };
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = text;
    mii.cch = 128;
    GetMenuItemInfoW(udm->um.hmenu, static_cast<UINT>(udm->umi.iPosition), TRUE,
                     &mii);

    const bool hot =
        (udm->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
    const bool disabled =
        (udm->dis.itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;

    // Normal: dark label on the mid-amber strip. Hover/open: flip to the dark
    // dropdown ground with a bright cream label.
    HBRUSH bg = CreateSolidBrush(hot ? m_menuColors.bg : m_menuColors.barBg);
    FillRect(udm->um.hdc, &udm->dis.rcItem, bg);
    DeleteObject(bg);

    SetBkMode(udm->um.hdc, TRANSPARENT);
    SetTextColor(udm->um.hdc, disabled  ? m_menuColors.dim
                             : hot       ? m_menuColors.textHot
                                         : m_menuColors.barText);
    HGDIOBJ of = SelectObject(udm->um.hdc, m_menuFont);
    DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if (udm->dis.itemState & ODS_NOACCEL)
        flags |= DT_HIDEPREFIX;
    DrawTextW(udm->um.hdc, text, -1, &udm->dis.rcItem, flags);
    SelectObject(udm->um.hdc, of);
}

// ------------------------------------------------------------------- menu bar
void App::BuildMenus()
{
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IdmNewConnection, L"&New Connection...\tCtrl+Shift+T");
    AppendMenuW(file, MF_STRING, IdmCloseTab, L"&Close Tab\tCtrl+Shift+W");
    AppendMenuW(file, MF_STRING, IdmDisconnect, L"&Disconnect\tCtrl+Shift+D");
    AppendMenuW(file, MF_STRING, IdmGuardianRetry, L"&Reconnect Now");
    AppendMenuW(file, MF_STRING, IdmGuardianStop, L"Stop Reconnec&ting");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdmSftpPanel, L"SFTP &Browser...\tCtrl+Shift+B");
    AppendMenuW(file, MF_STRING, IdmLogSession, L"&Log Session Output...\tCtrl+Shift+L");
    AppendMenuW(file, MF_STRING, IdmImportProfiles,
                L"&Import Profiles (PuTTY / OpenSSH)...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdmSplitVertical, L"Split &Vertical\tCtrl+Shift+E");
    AppendMenuW(file, MF_STRING, IdmSplitHorizontal, L"Split Hori&zontal\tCtrl+Shift+U");
    AppendMenuW(file, MF_STRING, IdmPaneClose, L"Close Pa&ne");
    {
        HMENU pane = CreatePopupMenu();
        AppendMenuW(pane, MF_STRING, IdmPaneZoom, L"&Zoom / Restore\tCtrl+Shift+Z");
        AppendMenuW(pane, MF_STRING, IdmPaneRotate, L"&Rotate the Split");
        AppendMenuW(pane, MF_STRING, IdmPaneReadOnly, L"Read-&only\tCtrl+Shift+R");
        AppendMenuW(pane, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pane, MF_STRING, IdmPaneFocusNext, L"&Next Pane\tCtrl+Shift+]");
        AppendMenuW(pane, MF_STRING, IdmPaneFocusPrev, L"&Previous Pane\tCtrl+Shift+[");
        AppendMenuW(pane, MF_STRING, IdmPaneFocusLeft, L"Focus &Left\tCtrl+Shift+Left");
        AppendMenuW(pane, MF_STRING, IdmPaneFocusRight, L"Focus &Right\tCtrl+Shift+Right");
        AppendMenuW(pane, MF_STRING, IdmPaneFocusUp, L"Focus &Up\tCtrl+Shift+Up");
        AppendMenuW(pane, MF_STRING, IdmPaneFocusDown, L"Focus &Down\tCtrl+Shift+Down");
        AppendMenuW(pane, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pane, MF_STRING, IdmPaneMoveLeft, L"Move Pane Left\tCtrl+Shift+Alt+Left");
        AppendMenuW(pane, MF_STRING, IdmPaneMoveRight, L"Move Pane Right\tCtrl+Shift+Alt+Right");
        AppendMenuW(pane, MF_STRING, IdmPaneMoveUp, L"Move Pane Up\tCtrl+Shift+Alt+Up");
        AppendMenuW(pane, MF_STRING, IdmPaneMoveDown, L"Move Pane Down\tCtrl+Shift+Alt+Down");
        AppendMenuW(pane, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pane, MF_STRING, IdmPaneSwapLeft, L"Swap With Left");
        AppendMenuW(pane, MF_STRING, IdmPaneSwapRight, L"Swap With Right");
        AppendMenuW(pane, MF_STRING, IdmPaneSwapUp, L"Swap With Above");
        AppendMenuW(pane, MF_STRING, IdmPaneSwapDown, L"Swap With Below");
        AppendMenuW(pane, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pane, MF_STRING, IdmPaneGrow, L"Wider");
        AppendMenuW(pane, MF_STRING, IdmPaneShrink, L"Narrower");
        AppendMenuW(pane, MF_STRING, IdmPaneGrowV, L"Taller");
        AppendMenuW(pane, MF_STRING, IdmPaneShrinkV, L"Shorter");
        AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(pane), L"Pa&ne");
    }
    {
        HMENU bc = CreatePopupMenu();
        AppendMenuW(bc, MF_STRING, IdmBroadcastPick, L"Choose &Targets...\tCtrl+Shift+B");
        AppendMenuW(bc, MF_STRING, IdmBroadcastAll, L"Select &All Panes");
        AppendMenuW(bc, MF_STRING, IdmBroadcastStop, L"&Stop Broadcasting");
        AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(bc), L"&Broadcast");
    }
    {
        // Safety. Two independent features, one place to find them: the cloak
        // is about who can SEE the session, the blast radius about what the
        // session can DO.
        HMENU safety = CreatePopupMenu();
        AppendMenuW(safety, MF_STRING, IdmCloak, L"&Privacy Cloak\tCtrl+Shift+M");
        AppendMenuW(safety, MF_STRING, IdmCloakAddrs, L"Cloak: also mask &IP addresses");
        AppendMenuW(safety, MF_STRING, IdmCloakHome, L"Cloak: also mask &home directory names");
        AppendMenuW(safety, MF_SEPARATOR, 0, nullptr);
        HMENU risk = CreatePopupMenu();
        AppendMenuW(risk, MF_STRING, IdmRiskFirst + 0, L"&Off");
        AppendMenuW(risk, MF_STRING, IdmRiskFirst + 1, L"&Critical only");
        AppendMenuW(risk, MF_STRING, IdmRiskFirst + 2, L"&High and critical");
        AppendMenuW(risk, MF_STRING, IdmRiskFirst + 3, L"&Standard (medium and above)");
        AppendMenuW(risk, MF_STRING, IdmRiskFirst + 4, L"&Everything");
        AppendMenuW(safety, MF_POPUP, reinterpret_cast<UINT_PTR>(risk),
                    L"Confirm &Risky Commands");
        AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(safety), L"&Safety");
    }
    {
        // Remote display. Nothing here is bundled: AmberSSH finds an X server
        // or an RDP client that is already installed and hands off to it.
        HMENU rd = CreatePopupMenu();
        AppendMenuW(rd, MF_STRING, IdmXServerReport, L"&Find X Servers");
        // "Installed", because AmberSSH has no X server of its own to start
        // — this launches VcXsrv/Xming/Cygwin-X if one is on disk. The
        // built-in path is AmberX, chosen per profile under SSH → X11.
        AppendMenuW(rd, MF_STRING, IdmXServerStart, L"&Start Installed X Server");
        AppendMenuW(rd, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(rd, MF_STRING, IdmRemoteApp,
                    L"&Wayland RemoteApp (start Weston)...");
        AppendMenuW(rd, MF_STRING, IdmRemoteAppTunnel,
                    L"Wayland RemoteApp (&tunnel only)...");
        AppendMenuW(rd, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(rd, MF_STRING, IdmRemoteDisplayDocs, L"Setup &Guide...");
        AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(rd),
                    L"Remote &Display");
    }
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdmCommandPalette,
                L"Command &Palette...\tCtrl+Shift+P");
    AppendMenuW(file, MF_STRING, IdmJournal,
                L"Command &Journal...\tCtrl+Shift+J");
    AppendMenuW(file, MF_STRING, IdmJournalCapture, L"Record Commands to Journal");
    AppendMenuW(file, MF_STRING, IdmJournalClear, L"Clear Command Journal...");
    AppendMenuW(file, MF_STRING, IdmEditSnippets, L"Edit S&nippets...");
    AppendMenuW(file, MF_STRING, IdmEditTriggers, L"Edit Output Tri&ggers...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdmRecordCast, L"&Record Session (asciinema)...");
    AppendMenuW(file, MF_STRING, IdmPlayCast, L"Pla&y Recording...");
    AppendMenuW(file, MF_STRING, IdmHelloUnlock, L"Windows &Hello for Stored Secrets");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdmForwardsEdit, L"Port &Forwarding / Jump Host...");
    AppendMenuW(file, MF_STRING, IdmVitals, L"Remote &Vitals in Title Bar");
    AppendMenuW(file, MF_STRING, IdmQuakeMode, L"&Quake Mode (Ctrl+` dropdown)");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    m_workspaceMenu = CreatePopupMenu();
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(m_workspaceMenu),
                L"&Workspaces");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdmAbout, L"&About Amber SSH...");
    AppendMenuW(file, MF_STRING, IdmExit, L"E&xit\tAlt+F4");

    HMENU motion = CreatePopupMenu();
    for (int i = 0; i < kMotionStyleCount; ++i)
        AppendMenuW(motion, MF_STRING, IdmMotionFirst + i, kMotionStyles[i].menu);
    AppendMenuW(motion, MF_SEPARATOR, 0, nullptr);
    // How text LEAVES, chosen separately from how it arrives.
    {
        HMENU depart = CreatePopupMenu();
        static const wchar_t* kDepart[5] = {
            L"&Fade (default)", L"&Ash — burn and fall", L"&Smoke — drift upward",
            L"Sa&nd — sink", L"S&hatter — burst" };
        for (int i = 0; i < 5; ++i)
            AppendMenuW(depart, MF_STRING, IdmDepartFirst + i, kDepart[i]);
        AppendMenuW(motion, MF_POPUP, reinterpret_cast<UINT_PTR>(depart),
                    L"&Departure Style");
    }
    AppendMenuW(motion, MF_STRING, IdmReducedMotion, L"Reduced Motion");
    AppendMenuW(motion, MF_STRING, IdmTimeDialReset,
                L"Reset Time Dial (Alt+Wheel)");
    AppendMenuW(motion, MF_SEPARATOR, 0, nullptr);
    HMENU speedSub = CreatePopupMenu();
    AppendMenuW(speedSub, MF_STRING, IdmSpeedFirst + 0, L"&Slow — 0.5x");
    AppendMenuW(speedSub, MF_STRING, IdmSpeedFirst + 1, L"&Normal — 1x");
    AppendMenuW(speedSub, MF_STRING, IdmSpeedFirst + 2, L"&Fast — 1.5x");
    AppendMenuW(speedSub, MF_STRING, IdmSpeedFirst + 3, L"&Hyper — 2x");
    AppendMenuW(speedSub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(speedSub, MF_STRING, IdmSpeedCustom, L"&Custom...");
    AppendMenuW(motion, MF_POPUP, reinterpret_cast<UINT_PTR>(speedSub),
                L"Motion S&peed");
    AppendMenuW(motion, MF_STRING, IdmFxShockwave,
                L"Shock&wave on Typing (Enter/Backspace)");
    AppendMenuW(motion, MF_STRING, IdmFxCascade,
                L"Cascade &Output Reveal (paced draw)");
    AppendMenuW(motion, MF_STRING, IdmFxBell, L"Visual &Bell (BEL shockwave)");
    AppendMenuW(motion, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(motion, MF_STRING, IdmFxHeat, L"Activity &Heat Map");
    AppendMenuW(motion, MF_STRING, IdmFxGhost, L"Latency &Ghosting");
    AppendMenuW(motion, MF_STRING, IdmFxAudio, L"&Audio-Reactive Turbulence");
    AppendMenuW(motion, MF_STRING, IdmFxBoot, L"Boot Se&quence on Connect");
    {
        HMENU sharp = CreatePopupMenu();
        AppendMenuW(sharp, MF_STRING, IdmSharpFirst + 0, L"&Soft (glow over the letter)");
        AppendMenuW(sharp, MF_STRING, IdmSharpFirst + 1, L"&Crisp (letter over the glow)");
        AppendMenuW(sharp, MF_STRING, IdmSharpFirst + 2, L"&Razor (letter drawn after bloom)");
        AppendMenuW(motion, MF_POPUP, reinterpret_cast<UINT_PTR>(sharp), L"Text S&harpness");
    }
    AppendMenuW(motion, MF_STRING, IdmFxLive,
                L"&Live Effects (tide marks, pulse, echo ring, comet, weather)");
    AppendMenuW(motion, MF_STRING, IdmFxPersist, L"Phosphor &Persistence (P39 afterglow)");
    AppendMenuW(motion, MF_STRING, IdmFxWarmup, L"Phosphor &Warm-up on Connect");
    AppendMenuW(motion, MF_STRING, IdmFxParallax, L"Depth Parallax (pointer tilt)");
    AppendMenuW(motion, MF_STRING, IdmFxSlosh, L"Window Slosh (field lags the frame)");
    AppendMenuW(motion, MF_STRING, IdmFxSpotlight, L"Spotlight the Running Command");
    {
        HMENU night = CreatePopupMenu();
        AppendMenuW(night, MF_STRING, IdmNightFirst + 0, L"&Off");
        AppendMenuW(night, MF_STRING, IdmNightFirst + 1, L"&After dark");
        AppendMenuW(night, MF_STRING, IdmNightFirst + 2, L"A&lways warm");
        AppendMenuW(motion, MF_POPUP, reinterpret_cast<UINT_PTR>(night),
                    L"&Night Shift");
    }
    AppendMenuW(motion, MF_STRING, IdmFxCube, L"C&ube Session Switch (Compiz)");
    {
        HMENU saver = CreatePopupMenu();
        AppendMenuW(saver, MF_STRING, IdmSaverFirst + 0, L"&Off");
        AppendMenuW(saver, MF_STRING, IdmSaverFirst + 1, L"After &1 minute");
        AppendMenuW(saver, MF_STRING, IdmSaverFirst + 2, L"After &5 minutes");
        AppendMenuW(saver, MF_STRING, IdmSaverFirst + 3, L"After 1&5 minutes");
        AppendMenuW(motion, MF_POPUP, reinterpret_cast<UINT_PTR>(saver),
                    L"Screensaver (Digital &Rain)");
    }

    HMENU density = CreatePopupMenu();
    AppendMenuW(density, MF_STRING, IdmDensityAuto, L"&Auto (48–128, frame-time driven)");
    AppendMenuW(density, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(density, MF_STRING, IdmDensityFirst + 0, L"&Performance — 32");
    AppendMenuW(density, MF_STRING, IdmDensityFirst + 1, L"&Balanced — 64");
    AppendMenuW(density, MF_STRING, IdmDensityFirst + 2, L"&High Clarity — 96");
    AppendMenuW(density, MF_STRING, IdmDensityFirst + 3, L"&Ultra — 128");
    AppendMenuW(density, MF_STRING, IdmDensityFirst + 4, L"Ultra &Max — 256");
    AppendMenuW(density, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(density, MF_STRING, IdmDensityCustom, L"&Custom...");

    HMENU bloomSub = CreatePopupMenu();
    AppendMenuW(bloomSub, MF_STRING, IdmBloomFirst + 0, L"&Low");
    AppendMenuW(bloomSub, MF_STRING, IdmBloomFirst + 1, L"&Medium");
    AppendMenuW(bloomSub, MF_STRING, IdmBloomFirst + 2, L"&High");

    HMENU twinkleSub = CreatePopupMenu();
    AppendMenuW(twinkleSub, MF_STRING, IdmTwinkleFirst + 0, L"&Off");
    AppendMenuW(twinkleSub, MF_STRING, IdmTwinkleFirst + 1, L"&Subtle");
    AppendMenuW(twinkleSub, MF_STRING, IdmTwinkleFirst + 2, L"&Full");

    HMENU trailSub = CreatePopupMenu();
    AppendMenuW(trailSub, MF_STRING, IdmTrailFirst + 0, L"&Off");
    AppendMenuW(trailSub, MF_STRING, IdmTrailFirst + 1, L"&Low");
    AppendMenuW(trailSub, MF_STRING, IdmTrailFirst + 2, L"&Medium");
    AppendMenuW(trailSub, MF_STRING, IdmTrailFirst + 3, L"&High");

    HMENU bgSub = CreatePopupMenu();
    AppendMenuW(bgSub, MF_STRING, IdmBgFirst + 0, L"&Off");
    AppendMenuW(bgSub, MF_STRING, IdmBgFirst + 1, L"&Embers — rising warm motes");
    AppendMenuW(bgSub, MF_STRING, IdmBgFirst + 2, L"&Starfield — parallax stars");
    AppendMenuW(bgSub, MF_STRING, IdmBgFirst + 3, L"Cosmic &Dust — drifting haze");

    HMENU effects = CreatePopupMenu();
    AppendMenuW(effects, MF_STRING, IdmFxCrispCore, L"Particle + &Crisp Core");
    AppendMenuW(effects, MF_STRING, IdmFxParticlesOnly, L"Particles &Only (legacy)");
    AppendMenuW(effects, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(effects, MF_STRING, IdmFxBloom, L"&Bloom Glow");
    AppendMenuW(effects, MF_POPUP, reinterpret_cast<UINT_PTR>(bloomSub),
                L"Bloom &Intensity");
    AppendMenuW(effects, MF_POPUP, reinterpret_cast<UINT_PTR>(twinkleSub),
                L"&Twinkle Intensity");
    AppendMenuW(effects, MF_POPUP, reinterpret_cast<UINT_PTR>(trailSub),
                L"T&rails");
    AppendMenuW(effects, MF_POPUP, reinterpret_cast<UINT_PTR>(bgSub),
                L"Bac&kground Depth");
    AppendMenuW(effects, MF_STRING, IdmFxScanlines, L"&Scanlines");
    AppendMenuW(effects, MF_STRING, IdmFxVignette, L"&Vignette");
    AppendMenuW(effects, MF_STRING, IdmFxDrift, L"&Gas Drift");
    AppendMenuW(effects, MF_STRING, IdmFxMiami, L"&Miami Sunset Selection");
    AppendMenuW(effects, MF_STRING, IdmFxPointerForce, L"&Pointer Force (Alt+Drag)");

    HMENU paletteSub = CreatePopupMenu();
    AppendMenuW(paletteSub, MF_STRING, IdmPaletteFirst + 0, L"&Amber Miami");
    AppendMenuW(paletteSub, MF_STRING, IdmPaletteFirst + 1, L"&Classic xterm");

    HMENU termSub = CreatePopupMenu();
    AppendMenuW(termSub, MF_STRING, IdmTermFirst + 0, L"xterm-&256color (default)");
    AppendMenuW(termSub, MF_STRING, IdmTermFirst + 1, L"&screen-256color");
    AppendMenuW(termSub, MF_STRING, IdmTermFirst + 2, L"xterm-&direct");

    HMENU fontSub = CreatePopupMenu();
    AppendMenuW(fontSub, MF_STRING, IdmFontStyleFirst + 0, L"&Modern (Crisp Core)");
    AppendMenuW(fontSub, MF_STRING, IdmFontStyleFirst + 1, L"Dot Matrix &8-pin");
    AppendMenuW(fontSub, MF_STRING, IdmFontStyleFirst + 2, L"Dot Matrix &12-pin");

    HMENU faceSub = CreatePopupMenu();
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 0, L"&JetBrains Mono");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 1, L"&Cascadia Mono");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 2, L"C&onsolas");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 3, L"Courier &New");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 4, L"&Lucida Console");
    AppendMenuW(faceSub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 7, L"Cascadia Cod&e");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 8, L"&Fira Code");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 9, L"&Hack");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 10, L"&Source Code Pro");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 11, L"&IBM Plex Mono");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 12, L"Iose&vka");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 13, L"Space &Mono");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 14, L"&Ubuntu Mono");
    AppendMenuW(faceSub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 15,
                L"Share &Tech Mono ▸ futuristic");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 16,
                L"OCR &A Extended ▸ retro-future");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 17,
                L"S&yne Mono ▸ futuristic");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 18,
                L"Or&bitron ▸ display (wide)");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 19,
                L"Michro&ma ▸ display (wide)");
    AppendMenuW(faceSub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 5,
                L"NSimSun — CJK terminal Latin");
    AppendMenuW(faceSub, MF_STRING, IdmFontFaceFirst + 6,
                L"MS Gothic — Japanese terminal");

    HMENU themeSub = CreatePopupMenu();
    for (int i = 0; i < kThemeCount - 1; ++i)
        AppendMenuW(themeSub, MF_STRING, IdmThemeFirst + i, kThemes[i].name);
    AppendMenuW(themeSub, MF_STRING, IdmThemeFirst + kThemeCount - 1, L"&Custom");
    AppendMenuW(themeSub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(themeSub, MF_STRING, IdmThemeEdit, L"Customi&ze Theme...");

    HMENU apprSub = CreatePopupMenu();
    AppendMenuW(apprSub, MF_STRING, IdmAppearanceFirst + 0, L"&Dark");
    AppendMenuW(apprSub, MF_STRING, IdmAppearanceFirst + 1, L"&Light (paper)");
    AppendMenuW(apprSub, MF_STRING, IdmAppearanceFirst + 2,
                L"&Paperwhite (e-ink greyscale)");
    AppendMenuW(apprSub, MF_STRING, IdmAppearanceFirst + 3,
                L"Pi&xel Art (16-color)");

    HMENU shadowSub = CreatePopupMenu();
    AppendMenuW(shadowSub, MF_STRING, IdmShadowFirst + 0, L"&Off");
    AppendMenuW(shadowSub, MF_STRING, IdmShadowFirst + 1, L"&Soft");
    AppendMenuW(shadowSub, MF_STRING, IdmShadowFirst + 2, L"&Medium");
    AppendMenuW(shadowSub, MF_STRING, IdmShadowFirst + 3, L"S&trong");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IdmViewFullscreen, L"&Fullscreen\tF11");
    {
        // Interface skin (ui/Chrome.h): one entry per ChromeSpec.
        HMENU chromeSub = CreatePopupMenu();
        for (int i = 0; i < amber::kChromeCount; ++i)
            AppendMenuW(chromeSub, MF_STRING, IdmChromeFirst + i,
                        WideFromUtf8(amber::ChromeAt(i).name).c_str());
        AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(chromeSub),
                    L"&Interface Style");
    }
    AppendMenuW(view, MF_STRING, IdmSearchScrollback,
                L"&Search Scrollback...\tCtrl+Shift+F");
    AppendMenuW(view, MF_STRING, IdmSearchNext, L"Search &Next\tF3");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(apprSub),
                L"&Appearance");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(themeSub),
                L"T&heme");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(shadowSub),
                L"Text &Shadow (light mode)");
    AppendMenuW(view, MF_STRING, IdmViewVsync, L"&VSync\tF2");
    AppendMenuW(view, MF_STRING, IdmViewOverlay, L"Debug &Overlay\tF3");
    AppendMenuW(view, MF_STRING, IdmStatusBar, L"Status &Bar");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IdmPrevCommand, L"Previous Comman&d\tCtrl+Up");
    AppendMenuW(view, MF_STRING, IdmNextCommand, L"Ne&xt Command\tCtrl+Down");
    AppendMenuW(view, MF_STRING, IdmPasteGuard, L"Confirm &Multi-line Paste");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IdmFoldToggle, L"&Fold Output at Cursor\tCtrl+Shift+O");
    AppendMenuW(view, MF_STRING, IdmFoldAll, L"Fold &All Output");
    AppendMenuW(view, MF_STRING, IdmFoldNone, L"&Expand All Output");
    // ---- the command block at the cursor --------------------------------
    {
        HMENU blk = CreatePopupMenu();
        AppendMenuW(blk, MF_STRING, IdmBlockCopyCommand, L"Copy &Command");
        AppendMenuW(blk, MF_STRING, IdmBlockCopyOutput, L"Copy &Output");
        AppendMenuW(blk, MF_STRING, IdmBlockCopyBoth, L"Copy &Both");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(blk, MF_STRING, IdmBlockFold, L"&Fold / Expand");
        AppendMenuW(blk, MF_STRING, IdmBlockSearch, L"&Search in Output...");
        AppendMenuW(blk, MF_STRING, IdmBlockSnippet, L"Save as S&nippet...");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(blk, MF_STRING, IdmBlockBookmark, L"Boo&kmark");
        AppendMenuW(blk, MF_STRING, IdmBlockPrevBookmark, L"Previous Bookmark");
        AppendMenuW(blk, MF_STRING, IdmBlockNextBookmark, L"Next Bookmark");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        // Type-without-running first, and named so, because it is the one
        // that cannot do anything by itself.
        AppendMenuW(blk, MF_STRING, IdmBlockRerun, L"&Type the Command (does not run it)");
        AppendMenuW(blk, MF_STRING, IdmBlockRerunNow, L"&Run the Command Now");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(blk, MF_STRING, IdmBlockNotify, L"Notify When &This One Finishes");
        HMENU note = CreatePopupMenu();
        AppendMenuW(note, MF_STRING, IdmBlockNotifyFirst + 0, L"Off");
        AppendMenuW(note, MF_STRING, IdmBlockNotifyFirst + 1, L"Success only");
        AppendMenuW(note, MF_STRING, IdmBlockNotifyFirst + 2, L"Failure only");
        AppendMenuW(note, MF_STRING, IdmBlockNotifyFirst + 3, L"Success and failure");
        AppendMenuW(note, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(note, MF_STRING, IdmBlockNotifyAfter, L"Only after N seconds...");
        AppendMenuW(blk, MF_POPUP, reinterpret_cast<UINT_PTR>(note),
                    L"Notify on &Long Commands");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(blk, MF_STRING, IdmBlockSummaryCwd,
                    L"Folded Summary Shows the &Directory");
        AppendMenuW(blk, MF_STRING, IdmBlockSummaryFirstLine,
                    L"Folded Summary Shows the First &Line");
        AppendMenuW(blk, MF_STRING, IdmBlockGutter, L"Show the Block &Gutter");
        AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(blk),
                    L"Command &Block");
    }
    AppendMenuW(view, MF_STRING, IdmViewSyntaxTint, L"Syntax &Tint");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(faceSub),
                L"Font F&ace");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(fontSub),
                L"Font &Style");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(paletteSub),
                L"ANSI &Palette");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(termSub),
                L"&Terminal Type (new sessions)");
    AppendMenuW(view, MF_STRING | MF_DISABLED, IdmTruecolorInfo,
                L"Truecolor: advertised (COLORTERM)");
    AppendMenuW(view, MF_STRING, IdmViewDiag, L"Color &Diagnostic Screen");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IdmViewFontLarger, L"Font &Larger\tCtrl+Plus");
    AppendMenuW(view, MF_STRING, IdmViewFontSmaller, L"Font S&maller\tCtrl+Minus");

    // The menus live in one popup opened by the title-bar hamburger
    // (Termius-style) instead of a classic menu bar — so no SetMenu.
    m_menu = CreatePopupMenu();
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(motion), L"&Motion");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(density), L"&Density");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(effects), L"&Effects");
    AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    ThemeMenuBar(m_menu, 1);   // owner-draw every popup item (theme colours)
    ApplyTheme();
    UpdateMenuChecks();
}

void App::OpenAppMenu()
{
    if (!m_menu)
        return;
    RECT menu, mn, mx, cl;
    CaptionLayout(menu, mn, mx, cl);
    POINT p = { menu.left, menu.bottom };
    ClientToScreen(m_hwnd, &p);
    UpdateMenuChecks();
    TrackPopupMenuEx(m_menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                     p.x, p.y, m_hwnd, nullptr);
}

void App::UpdateMenuChecks()
{
    if (!m_menu)
        return;

    uint32_t style = static_cast<uint32_t>(std::clamp(m_motionStyle, 0, kMotionStyleCount - 1));
    CheckMenuRadioItem(m_menu, IdmMotionFirst, IdmMotionLast,
                       IdmMotionFirst + static_cast<int>(style), MF_BYCOMMAND);
    CheckMenuItem(m_menu, IdmReducedMotion,
                  MF_BYCOMMAND | (m_particles.tun.reducedMotion ? MF_CHECKED
                                                                : MF_UNCHECKED));
    CheckMenuItem(m_menu, IdmJournalCapture,
                  MF_BYCOMMAND | (m_journalOn ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m_menu, IdmDepartFirst, IdmDepartLast,
                       IdmDepartFirst + std::clamp(m_departStyle, 0, 4),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmNightFirst, IdmNightLast,
                       IdmNightFirst + std::clamp(m_nightShift, 0, 2),
                       MF_BYCOMMAND);
    CheckMenuItem(m_menu, IdmFxParallax,
                  MF_BYCOMMAND | (m_fxParallax ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menu, IdmFxSlosh,
                  MF_BYCOMMAND | (m_fxSlosh ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menu, IdmFxWarmup,
                  MF_BYCOMMAND | (m_fxWarmup ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menu, IdmFxSpotlight,
                  MF_BYCOMMAND | (m_fxSpotlight ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m_menu, IdmSpeedFirst, IdmSpeedLast,
                       (m_speedLevel >= 4) ? IdmSpeedCustom
                                           : IdmSpeedFirst + std::clamp(m_speedLevel, 0, 3),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmFontStyleFirst, IdmFontStyleLast,
                       IdmFontStyleFirst + std::clamp(m_fontStyle, 0, 2),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmFontFaceFirst, IdmFontFaceLast,
                       IdmFontFaceFirst +
                           std::clamp(m_fontFace, 0, kFontFaceCount - 1),
                       MF_BYCOMMAND);

    int densityIdx = -1;
    for (int i = 0; i < 5; ++i)
        if (kDensitySteps[i] == m_densityPpc)
            densityIdx = i;
    int densitySel = m_densityAuto ? IdmDensityAuto
                     : (densityIdx >= 0 ? IdmDensityFirst + densityIdx
                                        : IdmDensityCustom);
    CheckMenuRadioItem(m_menu, IdmDensityAuto, IdmDensityLast, densitySel,
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmFxCrispCore, IdmFxParticlesOnly,
                       m_crispCore ? IdmFxCrispCore : IdmFxParticlesOnly,
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmBloomFirst, IdmBloomLast,
                       IdmBloomFirst + std::clamp(m_bloomLevel, 0, 2),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmTwinkleFirst, IdmTwinkleLast,
                       IdmTwinkleFirst + std::clamp(m_twinkleLevel, 0, 2),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmTrailFirst, IdmTrailLast,
                       IdmTrailFirst + std::clamp(m_trailLevel, 0, 3),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmPaletteFirst, IdmPaletteLast,
                       IdmPaletteFirst + std::clamp(m_paletteId, 0, 1),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmTermFirst, IdmTermLast,
                       IdmTermFirst + std::clamp(m_termTypeId, 0, 2),
                       MF_BYCOMMAND);

    auto check = [&](int id, bool on)
    {
        CheckMenuItem(m_menu, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    };
    check(IdmFxBloom, m_fxBloom);
    check(IdmFxScanlines, m_fxScanlines);
    check(IdmFxVignette, m_fxVignette);
    check(IdmFxDrift, m_fxDrift);
    check(IdmFxMiami, m_miamiSelection);
    check(IdmFxShockwave, m_fxShockwave);
    check(IdmFxCascade, m_fxCascade);
    check(IdmFxBell, m_fxBell);
    check(IdmBroadcast, m_broadcast);
    check(IdmFxHeat, m_fxHeat);
    check(IdmFxGhost, m_fxGhost);
    check(IdmFxAudio, m_fxAudio);
    check(IdmFxBoot, m_fxBoot);
    check(IdmHelloUnlock, m_helloUnlock);
    check(IdmRecordCast, HasSession() && Cur().castFile != nullptr);
    check(IdmVitals, m_vitalsOn);
    check(IdmQuakeMode, m_quake);
    check(IdmFxLive, m_fxLive);
    check(IdmFxPersist, m_fxPersist);
    check(IdmFxCube, m_fxCube);
    check(IdmCloak, m_cloak.enabled);
    check(IdmCloakAddrs, m_cloak.ipAddresses);
    check(IdmCloakHome, m_cloak.homeDirectories);
    EnableMenuItem(m_menu, IdmCloakAddrs,
                   MF_BYCOMMAND | (m_cloak.enabled ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_menu, IdmCloakHome,
                   MF_BYCOMMAND | (m_cloak.enabled ? MF_ENABLED : MF_GRAYED));
    CheckMenuRadioItem(m_menu, IdmRiskFirst, IdmRiskLast,
                       IdmRiskFirst + std::clamp(static_cast<int>(m_riskPolicy), 0, 4),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmSharpFirst, IdmSharpLast,
                       IdmSharpFirst + std::clamp(m_sharpness, 0, 2), MF_BYCOMMAND);
    {
        static const int kSaverSecs[4] = { 0, 60, 300, 900 };
        int si = 0;
        for (int i = 0; i < 4; ++i)
            if (kSaverSecs[i] == m_saverSecs) si = i;
        CheckMenuRadioItem(m_menu, IdmSaverFirst, IdmSaverLast,
                           IdmSaverFirst + si, MF_BYCOMMAND);
    }
    check(IdmLogSession, HasSession() && Cur().logFile != nullptr);
    CheckMenuRadioItem(m_menu, IdmThemeFirst, IdmThemeLast,
                       IdmThemeFirst + std::clamp(m_themeId, 0, kThemeCount - 1),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmBgFirst, IdmBgLast,
                       IdmBgFirst + std::clamp(m_bgStyle, 0, 3), MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmAppearanceFirst, IdmAppearanceLast,
                       IdmAppearanceFirst + std::clamp(m_appearance, 0, 3),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(m_menu, IdmShadowFirst, IdmShadowLast,
                       IdmShadowFirst + std::clamp(m_shadowLevel, 0, 3),
                       MF_BYCOMMAND);
    check(IdmFxPointerForce, m_fxPointerForce);
    check(IdmViewFullscreen, m_fullscreen);
    CheckMenuRadioItem(m_menu, IdmChromeFirst, IdmChromeFirst + amber::kChromeCount - 1,
                       IdmChromeFirst + std::clamp(m_chromeId, 0, amber::kChromeCount - 1),
                       MF_BYCOMMAND);
    check(IdmViewVsync, m_vsync);
    check(IdmViewOverlay, m_showOverlay);
    check(IdmStatusBar, m_statusBar);
    check(IdmPasteGuard, m_pasteGuard);
    check(IdmViewSyntaxTint, m_syntaxTint);
    check(IdmBlockSummaryCwd, m_foldShowCwd);
    check(IdmBlockSummaryFirstLine, m_foldFirstLine);
    check(IdmBlockGutter, m_blockGutter);
    check(IdmBlockNotify, HasSession() && Foc().notifyRunning);
    CheckMenuRadioItem(m_menu, IdmBlockNotifyFirst, IdmBlockNotifyLast,
                       IdmBlockNotifyFirst + std::clamp(m_notifyCommands, 0, 3),
                       MF_BYCOMMAND);
    // Bookmark reflects the block the cursor is in, so the item reports as
    // well as acts.
    if (HasSession())
    {
        amber::CommandBlock* b = BlockAtCursor(Foc());
        check(IdmBlockBookmark, b && b->bookmarked);
    }
    DrawMenuBar(m_hwnd);
}

bool App::HandleMenuCommand(int id)
{
    if (id >= IdmRiskFirst && id <= IdmRiskLast)
    {
        m_riskPolicy = static_cast<amber::RiskPolicy>(id - IdmRiskFirst);
        SaveSettings();
        UpdateMenuChecks();
        SetStatus(m_riskPolicy == amber::RiskPolicy::Off
                      ? std::string("Risky commands are no longer confirmed.")
                      : std::string("Confirm risky commands: ") +
                            amber::RiskPolicyName(m_riskPolicy));
        return true;
    }
    if (id >= IdmThemeFirst && id <= IdmThemeLast)
    {
        m_themeId = id - IdmThemeFirst;
        ApplyTheme();
        SaveSettings();
        UpdateMenuChecks();
        SetStatus(m_themeId < 6
                      ? "Theme: " + Utf8FromWide(kThemes[m_themeId].name)
                      : std::string("Theme: Custom"));
        return true;
    }
    switch (id)
    {
    case IdmThemeEdit:
        if (PromptCustomTheme())
        {
            m_themeId = 6;
            ApplyTheme();
            SaveSettings();
            UpdateMenuChecks();
            SetStatus("Theme: Custom (edited)");
        }
        return true;
    case IdmCloak:
        m_cloak.enabled = !m_cloak.enabled;
        InvalidateCloak();
        SaveSettings();
        UpdateMenuChecks();
        // The one wording the feature is allowed to use. It never claims the
        // session is safe to share, because pattern matching cannot know that.
        SetStatus(m_cloak.enabled ? amber::CloakStatusText()
                                  : "Privacy Cloak off.",
                  m_cloak.enabled ? 8.0 : 3.0);
        return true;
    case IdmCloakAddrs:
        m_cloak.ipAddresses = !m_cloak.ipAddresses;
        InvalidateCloak();
        SaveSettings();
        UpdateMenuChecks();
        SetStatus(m_cloak.ipAddresses ? "Cloak: IP addresses are masked too."
                                      : "Cloak: IP addresses are shown.");
        return true;
    case IdmCloakHome:
        m_cloak.homeDirectories = !m_cloak.homeDirectories;
        InvalidateCloak();
        SaveSettings();
        UpdateMenuChecks();
        SetStatus(m_cloak.homeDirectories
                      ? "Cloak: home directory names are masked too."
                      : "Cloak: home directory names are shown.");
        return true;
    case IdmXServerReport:    ReportXServers();         return true;
    case IdmXServerStart:     StartXServer();           return true;
    case IdmRemoteApp:        LaunchRemoteApp(true);    return true;
    case IdmRemoteAppTunnel:  LaunchRemoteApp(false);   return true;
    case IdmRemoteDisplayDocs:
    {
        // The runbook next to the binary, or the one in the source tree when
        // running from a build directory.
        std::filesystem::path doc =
            std::filesystem::path(ExeDir()) / "docs" / "REMOTE-DISPLAY.md";
        std::error_code ec;
        if (!std::filesystem::exists(doc, ec))
            doc = std::filesystem::path(ExeDir()).parent_path().parent_path() /
                  "docs" / "REMOTE-DISPLAY.md";
        ShellExecuteW(m_hwnd, L"open", doc.wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return true;
    }
    case IdmLogSession:       ToggleLogging();          return true;
    case IdmSearchScrollback: SearchScrollbackPrompt(); return true;
    case IdmSearchNext:       SearchNext();             return true;
    case IdmSftpPanel:        OpenSftpPanel();          return true;
    case IdmImportProfiles:   ImportProfiles();         return true;
    case IdmSplitVertical:    SplitPane(true);          return true;
    case IdmSplitHorizontal:  SplitPane(false);         return true;
    case IdmSplitClose:       CloseSplit();             return true;
    case IdmBroadcast:
        // The old toggle now opens the picker: "broadcast to both panes" has
        // no meaning once a tab can have six, and a toggle that silently
        // targets everything is the misuse this stage exists to prevent.
        PickBroadcastTargets();
        return true;
    case IdmCommandPalette:   TogglePalette();          return true;
    case IdmJournal:          ToggleJournal();          return true;
    case IdmVncRefresh:
    case IdmVncViewOnly:
    case IdmVncCtrlAltDel:
    case IdmVncSendClipboard:
        VncCommand(id);
        return true;
    case IdmPasteGuard:
        m_pasteGuard = !m_pasteGuard;
        SetStatus(m_pasteGuard ? "Multi-line pastes are confirmed first"
                               : "Paste guard off — pasted newlines run immediately",
                  4.0);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmPrevCommand:      JumpToMark(-1);           return true;
    case IdmNextCommand:      JumpToMark(1);            return true;
    case IdmFoldToggle:       ToggleFoldAtCursor();     return true;
    case IdmFoldAll:          FoldAll(true);            return true;
    case IdmFoldNone:         FoldAll(false);           return true;
    case IdmPaste:            Paste();                  return true;
    case IdmTimeDialReset:
        m_timeDial = 1.0f;
        SetStatus("Time dial 1.00x (normal)");
        return true;
    case IdmFxParallax:
        m_fxParallax = !m_fxParallax;
        SetStatus(m_fxParallax ? "Depth parallax on" : "Depth parallax off");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmFxSlosh:
        m_fxSlosh = !m_fxSlosh;
        SetStatus(m_fxSlosh ? "Window slosh on" : "Window slosh off");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmFxWarmup:
        m_fxWarmup = !m_fxWarmup;
        SetStatus(m_fxWarmup ? "Phosphor warms up on connect"
                             : "Phosphor starts at temperature");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmFxSpotlight:
        m_fxSpotlight = !m_fxSpotlight;
        SetStatus(m_fxSpotlight ? "Spotlight on the running command"
                                : "Spotlight off");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmWorkspaceSave:    SaveWorkspaceAs();        return true;
    case IdmWorkspaceDelete:
    {
        m_workspaces.Load();
        std::string name;
        if (!m_workspaceNames.empty())
            name = m_workspaceNames.front();
        if (PromptText("Delete which workspace?", name) && !name.empty())
            DeleteWorkspace(name);
        return true;
    }
    case IdmStatusBar:
        m_statusBar = !m_statusBar;
        UpdateGridDims();             // the bar reserves height, so re-layout
        SetStatus(m_statusBar ? "Status bar on" : "Status bar off");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmJournalCapture:
        m_journalOn = !m_journalOn;
        SetStatus(m_journalOn ? "Command journal: recording"
                              : "Command journal: recording off");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    case IdmJournalClear:
        if (MessageBoxW(m_hwnd,
                        L"Forget every command recorded in the journal?\n"
                        L"This cannot be undone.",
                        L"Clear Command Journal", MB_YESNO | MB_ICONWARNING) == IDYES)
        {
            m_journal.Load();
            m_journal.Clear();
            FilterJournal();
            SetStatus("Command journal cleared");
        }
        return true;
    case IdmEditSnippets:     EditUserFile("snippets.txt", kSnippetsTemplate); return true;
    case IdmEditTriggers:     EditUserFile("triggers.txt", kTriggersTemplate); return true;
    case IdmRecordCast:       ToggleRecording();        return true;
    case IdmPlayCast:         PlayRecording();          return true;
    case IdmForwardsEdit:     EditForwards();           return true;
    case IdmFxPersist:
        m_fxPersist = !m_fxPersist;
        SetStatus(m_fxPersist ? "Phosphor persistence on — erased text lingers, replaced text ghosts"
                              : "Phosphor persistence off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmNextTab:          CycleTab(1);              return true;
    case IdmPrevTab:          CycleTab(-1);             return true;
    case IdmFxCube:
        m_fxCube = !m_fxCube;
        SetStatus(m_fxCube ? "Cube session switch on - Ctrl+Tab / Ctrl+Shift+Tab rotate the cube"
                           : "Cube session switch off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmFxLive:
        m_fxLive = !m_fxLive;
        SetStatus(m_fxLive ? "Live effects on — command tide marks, run pulse, echo ring, comet cursor, link weather"
                           : "Live effects off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmVitals:
        m_vitalsOn = !m_vitalsOn;
        SyncVitals();
        SetStatus(m_vitalsOn ? "Remote vitals on — CPU / memory / network in the title bar"
                             : "Remote vitals off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmQuakeMode:
        m_quake = !m_quake;
        ApplyQuakeHotkey();
        SetStatus(m_quake ? "Quake mode on — Ctrl+` drops the terminal from the top"
                          : "Quake mode off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmHelloUnlock:
        m_helloUnlock = !m_helloUnlock;
        SetStatus(m_helloUnlock ? "Windows Hello required before stored secrets are used"
                                : "Windows Hello gate off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmFxHeat:
        m_fxHeat = !m_fxHeat;
        SetStatus(m_fxHeat ? "Activity heat map on — fresh output runs hot"
                           : "Activity heat map off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmFxGhost:
        m_fxGhost = !m_fxGhost;
        SetStatus(m_fxGhost ? "Latency ghosting on — cursor smears with RTT"
                            : "Latency ghosting off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmFxAudio:
        m_fxAudio = !m_fxAudio;
        if (m_fxAudio) m_audio.Start(); else m_audio.Stop();
        SetStatus(m_fxAudio ? "Audio-reactive turbulence on — play something"
                            : "Audio-reactive turbulence off");
        SaveSettings(); UpdateMenuChecks(); return true;
    case IdmFxBoot:
        m_fxBoot = !m_fxBoot;
        SetStatus(m_fxBoot ? "Boot sequence on — connects warm up like a tube"
                           : "Boot sequence off");
        SaveSettings(); UpdateMenuChecks(); return true;
    default:
        break;
    }

    if (id >= IdmSharpFirst && id <= IdmSharpLast)
    {
        m_sharpness = id - IdmSharpFirst;
        static const char* kNames[3] = { "Soft", "Crisp", "Razor" };
        SetStatus(std::string("Text sharpness: ") + kNames[m_sharpness]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmSnippetFirst && id <= IdmSnippetLast)
    {
        RunSnippet(id - IdmSnippetFirst);
        return true;
    }
    if (id >= IdmSaverFirst && id <= IdmSaverLast)
    {
        static const int kSaverSecs[4] = { 0, 60, 300, 900 };
        m_saverSecs = kSaverSecs[id - IdmSaverFirst];
        SetStatus(m_saverSecs ? "Screensaver: digital rain after " +
                                    std::to_string(m_saverSecs / 60) + " min idle"
                              : "Screensaver off");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmMotionFirst && id <= IdmMotionLast)
    {
        m_motionStyle = id - IdmMotionFirst;
        SetStatus(std::string("Motion: ") +
                  MotionStyleAt(static_cast<uint32_t>(m_motionStyle)).name +
                  (m_particles.tun.reducedMotion ? " (reduced motion on)" : ""));
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmBlockNotifyFirst && id <= IdmBlockNotifyLast)
    {
        m_notifyCommands = id - IdmBlockNotifyFirst;
        SetStatus(std::string("Long-command notifications: ") +
                  amber::NotifyOnName(static_cast<amber::NotifyOn>(m_notifyCommands)));
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmLocalFirst && id <= IdmLocalLast)
    {
        const std::vector<amber::LocalShell> shells = amber::DiscoverLocalShells();
        size_t i = static_cast<size_t>(id - IdmLocalFirst);
        if (i < shells.size())
            NewLocalSession(shells[i].key);
        return true;
    }
    if (id >= IdmWorkspaceFirst && id <= IdmWorkspaceLast)
    {
        size_t i = static_cast<size_t>(id - IdmWorkspaceFirst);
        if (i < m_workspaceNames.size())
            OpenWorkspace(m_workspaceNames[i]);
        return true;
    }
    // Interface skins are handled as a RANGE, so adding a skin to the table
    // never needs a new case.
    if (id >= IdmChromeFirst && id < IdmChromeFirst + amber::kChromeCount)
    {
        m_chromeId = id - IdmChromeFirst;
        amber::SetChrome(m_chromeId);
        // The menu palette is derived from the skin, so it has to be rebuilt
        // here or the dropdown keeps the previous skin's colours.
        ApplyTheme();
        SetStatus(std::string("Interface style: ") + amber::Chrome().name, 3.0);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmDepartFirst && id <= IdmDepartLast)
    {
        m_departStyle = id - IdmDepartFirst;
        static const char* kNames[5] = { "Fade", "Ash", "Smoke", "Sand", "Shatter" };
        SetStatus(std::string("Departure: ") + kNames[m_departStyle]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmNightFirst && id <= IdmNightLast)
    {
        m_nightShift = id - IdmNightFirst;
        static const char* kNames[3] = { "off", "after dark", "always warm" };
        SetStatus(std::string("Night shift: ") + kNames[m_nightShift]);
        ApplyTheme();
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id == IdmAbout)
    {
        amber::ShowAboutDialog(m_hwnd);
        return true;
    }
    if (id == IdmReducedMotion)
    {
        m_particles.tun.reducedMotion = !m_particles.tun.reducedMotion;
        SetStatus(m_particles.tun.reducedMotion
                      ? "Reduced motion on — short direct morph"
                      : std::string("Reduced motion off — motion: ") +
                            MotionStyleAt(static_cast<uint32_t>(m_motionStyle)).name);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id == IdmSpeedCustom)
    {
        float v = PromptSpeedValue(m_speedLevel >= 4 ? m_customSpeed
                                                     : CurrentSpeed());
        if (v > 0.0f)
        {
            m_customSpeed = v;
            m_speedLevel = 4;
            ApplyEffectSettings();
            char b[48];
            snprintf(b, sizeof(b), "Motion speed: custom — %.2fx", v);
            SetStatus(b);
            SaveSettings();
            UpdateMenuChecks();
        }
        return true;
    }
    if (id >= IdmSpeedFirst && id < IdmSpeedCustom)
    {
        m_speedLevel = id - IdmSpeedFirst;
        ApplyEffectSettings();
        static const char* names[] = { "0.5x", "1x", "1.5x", "2x" };
        SetStatus(std::string("Motion speed: ") + names[m_speedLevel]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmFontStyleFirst && id <= IdmFontStyleLast)
    {
        m_fontStyle = id - IdmFontStyleFirst;
        m_sampler.SetTemplateStyle(
            static_cast<GlyphSampler::TemplateStyle>(m_fontStyle));
        UpdateFontMetrics(m_fontPx);   // rebuilds atlas + reseeds particles
        static const char* names[] = { "Modern", "Dot Matrix 8-pin",
                                       "Dot Matrix 12-pin" };
        SetStatus(std::string("Font style: ") + names[m_fontStyle] +
                  " (crisp core)");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmFontFaceFirst && id <= IdmFontFaceLast)
    {
        int face = id - IdmFontFaceFirst;
        if (!m_sampler.SetFontFamily(kFontFaces[face]))
        {
            // Not installed and not bundled — fetch it on demand.
            DownloadFont(face);
            return true;
        }
        m_fontFace = face;
        UpdateFontMetrics(m_fontPx);   // cell metrics change with the face
        SetStatus("Font face: " + Utf8FromWide(kFontFaces[face]));
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id == IdmDensityAuto)
    {
        m_densityAuto = true;
        m_autoPpc = 96;
        UpdateGridDims();
        SetStatus("Density: Auto (adapts 48-128 by frame time)");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id == IdmDensityCustom)
    {
        uint32_t v = PromptDensity(RequestedDensity());
        if (v != 0)
        {
            m_densityAuto = false;
            m_densityPpc = std::clamp(v, 8u, kMaxParticlesPerCell);
            UpdateGridDims();
            SetStatus("Density: custom — " + std::to_string(m_densityPpc) +
                      " per glyph");
            SaveSettings();
            UpdateMenuChecks();
        }
        return true;
    }
    if (id >= IdmDensityFirst && id < IdmDensityCustom)
    {
        m_densityAuto = false;
        m_densityPpc = kDensitySteps[id - IdmDensityFirst];
        UpdateGridDims();   // re-derives effective density and particle count
        SetStatus(std::string("Density: ") + kDensityNames[id - IdmDensityFirst] +
                  " — " + std::to_string(m_densityPpc) + " per glyph");
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmBloomFirst && id <= IdmBloomLast)
    {
        m_bloomLevel = id - IdmBloomFirst;
        ApplyEffectSettings();
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmTwinkleFirst && id <= IdmTwinkleLast)
    {
        m_twinkleLevel = id - IdmTwinkleFirst;
        ApplyEffectSettings();
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmTrailFirst && id <= IdmTrailLast)
    {
        m_trailLevel = id - IdmTrailFirst;
        ApplyEffectSettings();
        static const char* names[] = { "off", "low", "medium", "high" };
        SetStatus(std::string("Trails: ") + names[m_trailLevel]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmBgFirst && id <= IdmBgLast)
    {
        m_bgStyle = id - IdmBgFirst;
        static const char* names[] = { "off", "Embers", "Starfield",
                                       "Cosmic Dust" };
        SetStatus(std::string("Background depth: ") + names[m_bgStyle]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmAppearanceFirst && id <= IdmAppearanceLast)
    {
        m_appearance = id - IdmAppearanceFirst;
        ApplyAppearance();
        static const char* names[] = { "Dark", "Light (paper)",
                                       "Paperwhite (e-ink)",
                                       "Pixel Art (16-color)" };
        SetStatus(std::string("Appearance: ") + names[m_appearance]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmShadowFirst && id <= IdmShadowLast)
    {
        m_shadowLevel = id - IdmShadowFirst;
        static const char* names[] = { "off", "soft", "medium", "strong" };
        SetStatus(std::string("Text shadow: ") + names[m_shadowLevel]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmPaletteFirst && id <= IdmPaletteLast)
    {
        m_paletteId = id - IdmPaletteFirst;
        SetStatus(std::string("ANSI palette: ") + Pal().name);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }
    if (id >= IdmTermFirst && id <= IdmTermLast)
    {
        m_termTypeId = id - IdmTermFirst;
        SetStatus(std::string("Terminal type for new sessions: ") +
                  kTermTypes[m_termTypeId]);
        SaveSettings();
        UpdateMenuChecks();
        return true;
    }

    switch (id)
    {
    case IdmNewConnection:
        if (ShowConnectionDialog())
            UpdateGridDims();
        return true;
    case IdmCloseTab:
        if (HasSession())
            CloseSession(m_active);
        return true;
    case IdmDisconnect:
        if (HasSession())
        {
            // Tell the guardian FIRST. Without this, asking to disconnect a
            // profile set to reconnect automatically would immediately bring
            // the session back — the menu item would appear not to work.
            Cur().userClosed = true;
            Cur().guardian.OnUserDisconnect();
            Cur().ssh.Disconnect();
        }
        return true;
    case IdmGuardianStop:
        if (HasSession())
            GuardianStop(Foc());
        return true;
    case IdmGuardianRetry:
        if (HasSession())
            GuardianRetryNow(Foc());
        return true;

    case IdmBlockCopyCommand:
    case IdmBlockCopyOutput:
    case IdmBlockCopyBoth:
    case IdmBlockFold:
    case IdmBlockSnippet:
    case IdmBlockBookmark:
    case IdmBlockSearch:
    case IdmBlockRerun:
    case IdmBlockRerunNow:
    case IdmBlockNotify:
    case IdmBlockPrevBookmark:
    case IdmBlockNextBookmark:
        BlockAction(id);
        return true;
    case IdmBlockSummaryCwd:
        m_foldShowCwd = !m_foldShowCwd;
        if (HasSession())
            ForEachSession([&](amber::Session& s) { RebuildBlockSummaries(s); });
        SetStatus(m_foldShowCwd ? "Folded summaries show the directory"
                                : "Folded summaries omit the directory");
        SaveSettings();
        return true;
    case IdmBlockSummaryFirstLine:
        m_foldFirstLine = !m_foldFirstLine;
        if (HasSession())
            ForEachSession([&](amber::Session& s) { RebuildBlockSummaries(s); });
        SetStatus(m_foldFirstLine ? "Folded summaries show the first output line"
                                  : "Folded summaries omit the first output line");
        SaveSettings();
        return true;
    // ---- panes -----------------------------------------------------------
    case IdmPaneFocusLeft:  FocusPane(amber::PaneLayout::Dir::Left);  return true;
    case IdmPaneFocusRight: FocusPane(amber::PaneLayout::Dir::Right); return true;
    case IdmPaneFocusUp:    FocusPane(amber::PaneLayout::Dir::Up);    return true;
    case IdmPaneFocusDown:  FocusPane(amber::PaneLayout::Dir::Down);  return true;
    case IdmPaneFocusNext:  FocusPaneCycle(1);                        return true;
    case IdmPaneFocusPrev:  FocusPaneCycle(-1);                       return true;
    case IdmPaneMoveLeft:   MovePane(amber::PaneLayout::Dir::Left);   return true;
    case IdmPaneMoveRight:  MovePane(amber::PaneLayout::Dir::Right);  return true;
    case IdmPaneMoveUp:     MovePane(amber::PaneLayout::Dir::Up);     return true;
    case IdmPaneMoveDown:   MovePane(amber::PaneLayout::Dir::Down);   return true;
    case IdmPaneSwapLeft:   SwapPaneWith(amber::PaneLayout::Dir::Left);  return true;
    case IdmPaneSwapRight:  SwapPaneWith(amber::PaneLayout::Dir::Right); return true;
    case IdmPaneSwapUp:     SwapPaneWith(amber::PaneLayout::Dir::Up);    return true;
    case IdmPaneSwapDown:   SwapPaneWith(amber::PaneLayout::Dir::Down);  return true;
    case IdmPaneGrow:       ResizePane(amber::PaneLayout::Dir::Right); return true;
    case IdmPaneShrink:     ResizePane(amber::PaneLayout::Dir::Left);  return true;
    case IdmPaneGrowV:      ResizePane(amber::PaneLayout::Dir::Down);  return true;
    case IdmPaneShrinkV:    ResizePane(amber::PaneLayout::Dir::Up);    return true;
    case IdmPaneRotate:     RotatePane();                             return true;
    case IdmPaneZoom:       ToggleZoomPane();                         return true;
    case IdmPaneReadOnly:   ToggleReadOnlyPane();                     return true;
    case IdmPaneClose:      CloseSplit();                             return true;
    case IdmBroadcastPick:  PickBroadcastTargets();                   return true;
    case IdmBroadcastStop:  StopBroadcast();                          return true;
    case IdmBroadcastAll:   BroadcastAll();                           return true;

    case IdmBlockGutter:
        m_blockGutter = !m_blockGutter;
        SetStatus(m_blockGutter ? "Command block gutter on"
                                : "Command block gutter off");
        SaveSettings();
        return true;
    case IdmBlockNotifyAfter:
    {
        std::string secs = std::to_string(m_notifyAfterSecs);
        if (!PromptText("Notify only after a command has run this many seconds", secs))
            return true;
        m_notifyAfterSecs = std::clamp(atoi(secs.c_str()), 0, 86400);
        SetStatus("Long-command notifications after " +
                  std::to_string(m_notifyAfterSecs) + "s");
        SaveSettings();
        return true;
    }
    case IdmExit:
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return true;

    case IdmFxCrispCore:
        m_crispCore = true;
        SetStatus("Rendering: Particle + Crisp Core");
        break;
    case IdmFxParticlesOnly:
        m_crispCore = false;
        SetStatus("Rendering: Particles Only");
        break;
    case IdmFxBloom:
        m_fxBloom = !m_fxBloom;
        break;
    case IdmFxScanlines:
        m_fxScanlines = !m_fxScanlines;
        break;
    case IdmFxVignette:
        m_fxVignette = !m_fxVignette;
        break;
    case IdmFxDrift:
        m_fxDrift = !m_fxDrift;
        ApplyEffectSettings();
        break;
    case IdmFxMiami:
        m_miamiSelection = !m_miamiSelection;
        break;
    case IdmFxShockwave:
        m_fxShockwave = !m_fxShockwave;
        break;
    case IdmFxCascade:
        m_fxCascade = !m_fxCascade;
        SetStatus(m_fxCascade ? "Cascade reveal on — output draws at motion speed"
                              : "Cascade reveal off — output is instant");
        break;
    case IdmFxBell:
        m_fxBell = !m_fxBell;
        SetStatus(m_fxBell ? "Visual bell on — BEL rings a shockwave"
                           : "Visual bell off");
        break;
    case IdmFxPointerForce:
        m_fxPointerForce = !m_fxPointerForce;
        if (!m_fxPointerForce)
            m_particles.tun.mouseForce = 0.0f;
        break;
    case IdmViewDiag:
        if (HasSession())
            FeedDiagnostic(Cur());
        return true;

    case IdmViewFullscreen:
        ToggleFullscreen();
        return true;                 // ToggleFullscreen refreshes the checks
    case IdmViewVsync:
        m_vsync = !m_vsync;
        break;
    case IdmViewOverlay:
        m_showOverlay = !m_showOverlay;
        break;
    case IdmViewSyntaxTint:
        m_syntaxTint = !m_syntaxTint;
        break;
    case IdmViewFontLarger:
        UpdateFontMetrics(m_fontPx + 1.0f);
        return true;
    case IdmViewFontSmaller:
        UpdateFontMetrics(m_fontPx - 1.0f);
        return true;

    default:
        return false;
    }
    SaveSettings();
    UpdateMenuChecks();
    return true;
}

// ------------------------------------------------------- settings + helpers
const amber::Palette16& App::Pal() const
{
    int id = m_paletteId;
    if (m_active >= 0 && m_active < static_cast<int>(m_sessions.size()) &&
        m_sessions[static_cast<size_t>(m_active)]->profile.palette >= 0)
        id = m_sessions[static_cast<size_t>(m_active)]->profile.palette;   // Colours page
    return id == 1 ? amber::PaletteClassicXterm() : amber::PaletteAmberMiami();
}

const char* App::DensityLabel() const
{
    if (m_densityAuto)
        return "Auto";
    for (int i = 0; i < 5; ++i)
        if (kDensitySteps[i] == m_densityPpc)
            return kDensityNames[i];
    return "Custom";
}

uint32_t App::RequestedDensity() const
{
    return m_densityAuto ? m_autoPpc : m_densityPpc;
}

void App::UpdateAutoDensity()
{
    if (!m_densityAuto || m_lastFrameTime - m_autoLastAdjust < 2.5)
        return;
    m_autoLastAdjust = m_lastFrameTime;
    // Target ~16.6 ms. Density changes reseed the particles, so adjust with
    // wide hysteresis and only between the documented 48..128 bounds.
    float gpu = std::max(m_device.GpuFrameMs(), m_frameMs);
    uint32_t next = m_autoPpc;
    if (gpu > 15.5f && m_autoPpc > 48)
        next = m_autoPpc > 96 ? 96 : (m_autoPpc > 64 ? 64 : 48);
    else if (gpu < 9.0f && m_autoPpc < 128)
        next = m_autoPpc < 64 ? 64 : (m_autoPpc < 96 ? 96 : 128);
    if (next != m_autoPpc)
    {
        m_autoPpc = next;
        UpdateGridDims();
        SetStatus("Auto density: " + std::to_string(next) + " per glyph");
    }
}

float App::CurrentSpeed() const
{
    static const float spd[4] = { 0.5f, 1.0f, 1.5f, 2.0f };
    if (m_speedLevel >= 4)
        return std::clamp(m_customSpeed, 0.1f, 5.0f);
    return spd[std::clamp(m_speedLevel, 0, 3)];
}

void App::ApplyAppearance()
{
    // Light ink path only for paper / paperwhite; Pixel Art keeps the glow.
    m_particles.tun.lightMode =
        (m_appearance == 1 || m_appearance == 2) ? 1.0f : 0.0f;
    // Default-fg particle ink: warm near-black on paper, neutral dark grey for
    // the e-ink mode (the composite desaturates that anyway).
    if (m_appearance == 2)
    { m_particles.tun.inkColor[0] = m_particles.tun.inkColor[1] =
          m_particles.tun.inkColor[2] = 0.09f; }
    else
    { m_particles.tun.inkColor[0] = 0.06f; m_particles.tun.inkColor[1] = 0.05f;
      m_particles.tun.inkColor[2] = 0.04f; }
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::ApplyEffectSettings()
{
    m_bloomStrength = kBloomLevels[std::clamp(m_bloomLevel, 0, 2)];
    m_particles.tun.curlAmp = m_fxDrift ? 1.0f : 0.0f;
    static const float twinkA[3] = { 0.0f, 0.05f, 0.10f };
    static const float flickA[3] = { 0.0f, 0.02f, 0.04f };
    m_particles.tun.twinkleAmp = twinkA[std::clamp(m_twinkleLevel, 0, 2)];
    m_particles.tun.flickerAmp = flickA[std::clamp(m_twinkleLevel, 0, 2)];
    static const float trailS[4] = { 0.0f, 0.035f, 0.07f, 0.12f };
    m_particles.tun.trailScale = trailS[std::clamp(m_trailLevel, 0, 3)];

    // Motion tempo: scales the style forces' time bases, shockwave expansion,
    // shimmer, gas drift and glyph-morph duration together.
    float s = CurrentSpeed();
    m_particles.tun.effectSpeed = s;
    m_particles.tun.shimmerSpeed = 4.0f * s;
    m_particles.tun.noiseSpeed = 0.35f * s;
    m_particles.tun.transitionDur = 0.25f / s;
}

namespace
{
struct PromptState
{
    HWND edit = nullptr;
    bool accepted = false;
    amber::DialogPalette pal{};
    HBRUSH bg = nullptr;
    HBRUSH field = nullptr;
    HFONT font = nullptr;
};

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(
                              reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_ERASEBKGND:
        if (st)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, st->bg);
            return 1;
        }
        break;

    case WM_CTLCOLOREDIT:
        if (st)
        {
            SetTextColor(reinterpret_cast<HDC>(wp), st->pal.text);
            SetBkColor(reinterpret_cast<HDC>(wp), st->pal.field);
            return reinterpret_cast<LRESULT>(st->field);
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (st)
        {
            SetTextColor(reinterpret_cast<HDC>(wp), st->pal.textDim);
            SetBkColor(reinterpret_cast<HDC>(wp), st->pal.bg);
            return reinterpret_cast<LRESULT>(st->bg);
        }
        break;

    case WM_PAINT:
        if (st)
        {
            // Amber outline (focus-bright) around the edit well.
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            HWND ed = st->edit;
            RECT r;
            GetWindowRect(ed, &r);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&r), 2);
            InflateRect(&r, 2, 2);
            HBRUSH b = CreateSolidBrush(GetFocus() == ed ? st->pal.borderHot
                                                         : st->pal.border);
            FrameRect(dc, &r, b);
            DeleteObject(b);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_DRAWITEM:
        if (st)
        {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlType != ODT_BUTTON)
                break;
            const amber::DialogPalette& p = st->pal;
            const bool accent = dis->CtlID == IDOK;
            const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            RECT rc = dis->rcItem;
            FillRect(dis->hDC, &rc, st->bg);
            COLORREF fill = accent ? p.accent : p.field;
            if (pressed)
                fill = amber::ScaleSrgb(fill, 0.8f);
            HBRUSH fb = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, 1, accent ? p.borderHot : p.border);
            HGDIOBJ ob = SelectObject(dis->hDC, fb);
            HGDIOBJ op = SelectObject(dis->hDC, pen);
            int rad = MulDiv(8, rc.bottom - rc.top, 26);
            RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, rad, rad);
            SelectObject(dis->hDC, ob);
            SelectObject(dis->hDC, op);
            DeleteObject(fb);
            DeleteObject(pen);
            wchar_t text[32] = L"";
            GetWindowTextW(dis->hwndItem, text, 32);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, accent ? p.accentText : p.text);
            HGDIOBJ of = SelectObject(dis->hDC, st->font);
            DrawTextW(dis->hDC, text, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, of);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && st)
        {
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    // No PostQuitMessage in WM_DESTROY: the modal loop below watches
    // IsWindow(), and a posted WM_QUIT would leak into the app's main loop
    // and terminate the process (the connection-dialog lesson).
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

// Shared modal one-value prompt. Returns true when OK'd; the entered text
// lands in outBuf. numericOnly restricts input to digits (density); decimal
// values (speed) allow '.' through a plain edit control.
static bool RunPromptDialog(HWND ownerWnd, const wchar_t* title,
                            const wchar_t* label, const wchar_t* initial,
                            bool numericOnly, wchar_t* outBuf, int outLen)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // painted from the theme
        wc.lpszClassName = L"AmberSSHPrompt";
        if (!RegisterClassExW(&wc))
            return false;
        registered = true;
    }

    PromptState st;
    st.pal = amber::MakeDialogPalette();
    st.bg = CreateSolidBrush(st.pal.bg);
    st.field = CreateSolidBrush(st.pal.field);
    UINT dpi = GetDpiForWindow(ownerWnd);
    auto px = [&](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    st.font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    HFONT mono = CreateFontW(-px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Cascadia Mono");

    RECT owner;
    GetWindowRect(ownerWnd, &owner);
    int w = px(320), h = px(158);
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"AmberSSHPrompt", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        owner.left + ((owner.right - owner.left) - w) / 2,
        owner.top + ((owner.bottom - owner.top) - h) / 2, w, h,
        ownerWnd, nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg)
    {
        DeleteObject(st.bg); DeleteObject(st.field);
        DeleteObject(st.font); DeleteObject(mono);
        return false;
    }
    amber::ApplyWindowChrome(dlg);   // dark, themed, rounded corners

    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                     int x, int y, int cw, int ch, int cid, HFONT f)
    {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 px(x), px(y), px(cw), px(ch), dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(cid)),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return c;
    };

    child(L"STATIC", label, SS_LEFT, 16, 16, 280, 20, -1, st.font);
    // Borderless edit: the dialog paints the amber outline in WM_PAINT.
    DWORD editStyle = WS_TABSTOP | ES_AUTOHSCROLL | (numericOnly ? ES_NUMBER : 0);
    st.edit = child(L"EDIT", initial, editStyle, 16, 44, 140, 26, 100, mono);
    SendMessageW(st.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(px(6), px(6)));
    child(L"BUTTON", L"OK", WS_TABSTOP | BS_OWNERDRAW, 130, 92, 84, 30, IDOK,
          st.font);
    child(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, 222, 92, 84, 30,
          IDCANCEL, st.font);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    SetFocus(st.edit);
    EnableWindow(ownerWnd, FALSE);

    outBuf[0] = 0;
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        // Capture the text continually: by WM_COMMAND/IDOK the window tears
        // down and the edit control may already be gone.
        if (IsWindow(st.edit))
            GetWindowTextW(st.edit, outBuf, outLen);
        // The owner-drawn buttons carry no default-push style, so drive
        // Enter/Escape here.
        if (msg.message == WM_KEYDOWN &&
            (msg.hwnd == dlg || IsChild(dlg, msg.hwnd)))
        {
            if (msg.wParam == VK_RETURN)
            {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE)
            {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED),
                             0);
                continue;
            }
        }
        if (!IsDialogMessageW(dlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(ownerWnd, TRUE);
    SetForegroundWindow(ownerWnd);
    DeleteObject(st.bg);
    DeleteObject(st.field);
    DeleteObject(st.font);
    DeleteObject(mono);
    return st.accepted;
}

uint32_t App::PromptDensity(uint32_t current)
{
    wchar_t cur[16], buf[16];
    swprintf_s(cur, L"%u", current);
    if (!RunPromptDialog(m_hwnd, L"Custom Particle Density",
                         L"Particles per glyph (8 - 256):", cur, true, buf, 16))
        return 0;
    int v = _wtoi(buf);
    if (v <= 0)
        return 0;
    return static_cast<uint32_t>(std::clamp(v, 8,
                                            static_cast<int>(kMaxParticlesPerCell)));
}

float App::PromptSpeedValue(float current)
{
    wchar_t cur[16], buf[16];
    swprintf_s(cur, L"%.2f", current);
    if (!RunPromptDialog(m_hwnd, L"Custom Motion Speed",
                         L"Speed multiplier (0.1 - 5.0):", cur, false, buf, 16))
        return 0.0f;
    float v = static_cast<float>(_wtof(buf));
    if (v <= 0.0f)
        return 0.0f;
    return std::clamp(v, 0.1f, 5.0f);
}

void App::TriggerShockwave()
{
    if (!m_fxShockwave || !HasSession())
        return;
    int r, c;
    bool vis;
    Cur().grid.CursorViewPos(r, c, vis);
    m_particles.tun.shockX = m_gm.originX + (c + 0.5f) * m_gm.cellW;
    m_particles.tun.shockY = m_gm.originY + (r + 0.5f) * m_gm.cellH;
    m_particles.tun.shockTime = m_time;
}

void App::LoadSettings()
{
    FILE* f = _wfopen(amber::SettingsFile().c_str(), L"rb");
    if (!f)
        return;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    // Deliberately simple key scanning over our own flat JSON file.
    auto readInt = [&](const char* key, int def)
    {
        const char* p = strstr(buf, key);
        if (!p)
            return def;
        p = strchr(p, ':');
        return p ? atoi(p + 1) : def;
    };
    auto readStr = [&](const char* key) -> std::string
    {
        const char* p = strstr(buf, key);
        if (!p)
            return {};
        p = strchr(p, ':');
        if (!p)
            return {};
        p = strchr(p, '"');
        if (!p)
            return {};
        const char* e = strchr(p + 1, '"');
        return e ? std::string(p + 1, e) : std::string();
    };
    m_densityAuto = readInt("\"densityAuto\"", 0) != 0;
    // Any value 8..256 is valid now that Custom exists.
    m_densityPpc = static_cast<uint32_t>(
        std::clamp(readInt("\"densityPpc\"", 96), 8,
                   static_cast<int>(kMaxParticlesPerCell)));
    m_crispCore = readInt("\"crispCore\"", 1) != 0;
    m_paletteId = std::clamp(readInt("\"palette\"", 0), 0, 1);
    m_termTypeId = std::clamp(readInt("\"termType\"", 0), 0, 2);
    m_bloomLevel = std::clamp(readInt("\"bloomLevel\"", 1), 0, 2);
    m_twinkleLevel = std::clamp(readInt("\"twinkleLevel\"", 1), 0, 2);
    m_trailLevel = std::clamp(readInt("\"trailLevel\"", 1), 0, 3);
    m_miamiSelection = readInt("\"miamiSelection\"", 1) != 0;
    m_speedLevel = std::clamp(readInt("\"speedLevel\"", 1), 0, 4);
    m_customSpeed = std::clamp(readInt("\"speedCustomPct\"", 100), 10, 500) / 100.0f;
    m_fontStyle = std::clamp(readInt("\"fontStyle\"", 0), 0, 2);
    m_fontFace = std::clamp(readInt("\"fontFace\"", 0), 0, kFontFaceCount - 1);
    m_fxShockwave = readInt("\"fxShockwave\"", 1) != 0;
    m_fxCascade = readInt("\"fxCascade\"", 1) != 0;
    m_motionStyle = std::clamp(readInt("\"animStyle\"", 0), 0, kMotionStyleCount - 1);
    // Reduced motion defaults to whatever the OS accessibility setting says:
    // with "Show animations in Windows" off, AmberSSH starts calm.
    BOOL osAnim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &osAnim, 0);
    m_particles.tun.reducedMotion =
        readInt("\"reducedMotion\"", osAnim ? 0 : 1) != 0;
    m_journalOn = readInt("\"journal\"", 1) != 0;
    m_statusBar = readInt("\"statusBar\"", 1) != 0;
    m_blockGutter = readInt("\"blockGutter\"", 1) != 0;
    m_foldShowCwd = readInt("\"blockSummaryCwd\"", 0) != 0;
    m_foldFirstLine = readInt("\"blockSummaryFirstLine\"", 1) != 0;
    m_notifyCommands = std::clamp(readInt("\"notifyCommands\"", 0), 0, 3);
    m_notifyAfterSecs = std::clamp(readInt("\"notifyAfterSecs\"", 30), 0, 86400);
    m_pasteGuard = readInt("\"pasteGuard\"", 1) != 0;
    m_cloak.enabled = readInt("\"cloak\"", 0) != 0;
    m_cloak.ipAddresses = readInt("\"cloakAddrs\"", 0) != 0;
    m_cloak.homeDirectories = readInt("\"cloakHome\"", 0) != 0;
    // Off by default is deliberate: the risk policy is not, because a
    // confirmation the user never asked for is far cheaper than the command it
    // stops. High and critical only, so the box stays rare enough to read.
    m_riskPolicy = static_cast<amber::RiskPolicy>(
        std::clamp(readInt("\"riskPolicy\"", 2), 0, 4));
    m_departStyle = std::clamp(readInt("\"departStyle\"", 0), 0, 4);
    m_fxParallax = readInt("\"fxParallax\"", 0) != 0;
    m_fxSlosh = readInt("\"fxSlosh\"", 0) != 0;
    m_fxWarmup = readInt("\"fxWarmup\"", 1) != 0;
    m_fxSpotlight = readInt("\"fxSpotlight\"", 0) != 0;
    m_nightShift = std::clamp(readInt("\"nightShift\"", 0), 0, 2);
    m_hostMotion = readStr("\"hostMotion\"");
    // Read the history before any command can be recorded: Add() rewrites the
    // whole file, so writing before reading would truncate it.
    m_journal.Load();
    m_fxBell = readInt("\"fxBell\"", 1) != 0;
    m_fxHeat = readInt("\"fxHeat\"", 1) != 0;
    m_fxGhost = readInt("\"fxGhost\"", 1) != 0;
    m_fxAudio = readInt("\"fxAudio\"", 0) != 0;
    m_fxBoot = readInt("\"fxBoot\"", 1) != 0;
    m_saverSecs = std::clamp(readInt("\"saverSecs\"", 300), 0, 3600);
    m_helloUnlock = readInt("\"helloUnlock\"", 0) != 0;
    m_vitalsOn = readInt("\"vitals\"", 0) != 0;
    m_quake = readInt("\"quakeMode\"", 0) != 0;
    m_sharpness = std::clamp(readInt("\"sharpness\"", 1), 0, 2);
    m_fxLive = readInt("\"fxLive\"", 1) != 0;
    m_fxPersist = readInt("\"fxPersist\"", 1) != 0;
    m_fxCube = readInt("\"fxCube\"", 1) != 0;
    m_chromeId = std::clamp(readInt("\"chrome\"", 0), 0, amber::kChromeCount - 1);
    amber::SetChrome(m_chromeId);
    m_bgStyle = std::clamp(readInt("\"bgStyle\"", 0), 0, 3);
    m_appearance = std::clamp(readInt("\"appearance\"", 0), 0, 3);
    m_shadowLevel = std::clamp(readInt("\"shadowLevel\"", 2), 0, 3);
    m_themeId = std::clamp(readInt("\"themeId\"", 0), 0, 6);
    m_hostThemes = readStr("\"hostThemes\"");
    m_customTheme[0] = static_cast<uint32_t>(
        readInt("\"themeC0\"", 0x7A5500)) & 0xFFFFFF;
    m_customTheme[1] = static_cast<uint32_t>(
        readInt("\"themeC1\"", 0xFFB000)) & 0xFFFFFF;
    m_customTheme[2] = static_cast<uint32_t>(
        readInt("\"themeC2\"", 0xFFF3C4)) & 0xFFFFFF;
    m_fxBloom = readInt("\"fxBloom\"", 1) != 0;
    m_fxScanlines = readInt("\"fxScanlines\"", 1) != 0;
    m_fxVignette = readInt("\"fxVignette\"", 1) != 0;
    m_fxDrift = readInt("\"fxDrift\"", 1) != 0;
    m_fxPointerForce = readInt("\"fxPointerForce\"", 1) != 0;
    m_syntaxTint = readInt("\"syntaxTint\"", 1) != 0;
}

void App::SaveSettings()
{
    FILE* f = _wfopen(amber::SettingsFile().c_str(), L"wb");
    if (!f)
        return;
    fprintf(f,
            "{\n"
            "  \"densityAuto\": %d,\n"
            "  \"densityPpc\": %u,\n"
            "  \"crispCore\": %d,\n"
            "  \"palette\": %d,\n"
            "  \"termType\": %d,\n"
            "  \"bloomLevel\": %d,\n"
            "  \"twinkleLevel\": %d,\n"
            "  \"trailLevel\": %d,\n"
            "  \"miamiSelection\": %d,\n"
            "  \"speedLevel\": %d,\n"
            "  \"speedCustomPct\": %d,\n"
            "  \"fontStyle\": %d,\n"
            "  \"fontFace\": %d,\n"
            "  \"fxShockwave\": %d,\n"
            "  \"fxCascade\": %d,\n"
            "  \"animStyle\": %d,\n"
            "  \"reducedMotion\": %d,\n"
            "  \"journal\": %d,\n"
            "  \"statusBar\": %d,\n"
            "  \"pasteGuard\": %d,\n"
            "  \"cloak\": %d,\n"
            "  \"cloakAddrs\": %d,\n"
            "  \"cloakHome\": %d,\n"
            "  \"riskPolicy\": %d,\n"
            "  \"departStyle\": %d,\n"
            "  \"fxParallax\": %d,\n"
            "  \"fxSlosh\": %d,\n"
            "  \"fxWarmup\": %d,\n"
            "  \"fxSpotlight\": %d,\n"
            "  \"nightShift\": %d,\n"
            "  \"fxBell\": %d,\n"
            "  \"bgStyle\": %d,\n"
            "  \"appearance\": %d,\n"
            "  \"shadowLevel\": %d,\n"
            "  \"themeId\": %d,\n"
            "  \"themeC0\": %u,\n"
            "  \"themeC1\": %u,\n"
            "  \"themeC2\": %u,\n"
            "  \"fxBloom\": %d,\n"
            "  \"fxScanlines\": %d,\n"
            "  \"fxVignette\": %d,\n"
            "  \"fxDrift\": %d,\n"
            "  \"fxPointerForce\": %d,\n"
            "  \"syntaxTint\": %d,\n"
            "  \"fxHeat\": %d,\n"
            "  \"fxGhost\": %d,\n"
            "  \"fxAudio\": %d,\n"
            "  \"fxBoot\": %d,\n"
            "  \"saverSecs\": %d,\n"
            "  \"helloUnlock\": %d,\n"
            "  \"vitals\": %d,\n"
            "  \"quakeMode\": %d,\n"
            "  \"sharpness\": %d,\n"
            "  \"fxLive\": %d,\n"
            "  \"fxPersist\": %d,\n"
            "  \"fxCube\": %d,\n"
            "  \"chrome\": %d,\n"
            "  \"blockGutter\": %d,\n"
            "  \"blockSummaryCwd\": %d,\n"
            "  \"blockSummaryFirstLine\": %d,\n"
            "  \"notifyCommands\": %d,\n"
            "  \"notifyAfterSecs\": %d,\n"
            "  \"hostThemes\": \"%s\"\n"
            "}\n",
            m_densityAuto ? 1 : 0, m_densityPpc, m_crispCore ? 1 : 0,
            m_paletteId, m_termTypeId, m_bloomLevel, m_twinkleLevel,
            m_trailLevel,
            m_miamiSelection ? 1 : 0, m_speedLevel,
            static_cast<int>(m_customSpeed * 100.0f + 0.5f),
            m_fontStyle, m_fontFace,
            m_fxShockwave ? 1 : 0, m_fxCascade ? 1 : 0,
            m_motionStyle,
            m_particles.tun.reducedMotion ? 1 : 0,
            m_journalOn ? 1 : 0,
            m_statusBar ? 1 : 0,
            m_pasteGuard ? 1 : 0,
            m_cloak.enabled ? 1 : 0, m_cloak.ipAddresses ? 1 : 0,
            m_cloak.homeDirectories ? 1 : 0, static_cast<int>(m_riskPolicy),
            m_departStyle,
            m_fxParallax ? 1 : 0, m_fxSlosh ? 1 : 0, m_fxWarmup ? 1 : 0,
            m_fxSpotlight ? 1 : 0, m_nightShift,
            m_fxBell ? 1 : 0, m_bgStyle, m_appearance, m_shadowLevel, m_themeId,
            m_customTheme[0], m_customTheme[1], m_customTheme[2],
            m_fxBloom ? 1 : 0, m_fxScanlines ? 1 : 0,
            m_fxVignette ? 1 : 0, m_fxDrift ? 1 : 0, m_fxPointerForce ? 1 : 0,
            m_syntaxTint ? 1 : 0,
            m_fxHeat ? 1 : 0, m_fxGhost ? 1 : 0, m_fxAudio ? 1 : 0,
            m_fxBoot ? 1 : 0, m_saverSecs, m_helloUnlock ? 1 : 0,
            m_vitalsOn ? 1 : 0, m_quake ? 1 : 0, m_sharpness, m_fxLive ? 1 : 0,
            m_fxPersist ? 1 : 0, m_fxCube ? 1 : 0, m_chromeId,
            m_blockGutter ? 1 : 0, m_foldShowCwd ? 1 : 0, m_foldFirstLine ? 1 : 0,
            m_notifyCommands, m_notifyAfterSecs, m_hostThemes.c_str());
    fclose(f);
}

// ------------------------------------------------------------ command palette
void App::TogglePalette()
{
    m_palOpen = !m_palOpen;
    if (m_palOpen)
    {
        m_palQuery.clear();
        m_palSel = 0;
        BuildPaletteItems();
        FilterPalette();
    }
}

void App::BuildPaletteItems()
{
    m_palAll.clear();
    auto add = [&](std::string label, int cmd) {
        m_palAll.push_back({ std::move(label), cmd });
    };
    auto onoff = [](bool v) { return v ? " (on)" : " (off)"; };

    // Actions
    add("New Connection...", IdmNewConnection);
    add("Close Tab", IdmCloseTab);
    add("Disconnect", IdmDisconnect);
    add("Reconnect Now", IdmGuardianRetry);
    // VNC — the desktop tab's own actions; each says what it does to the server
    add("VNC: Refresh Screen", IdmVncRefresh);
    add(std::string("VNC: View Only") + (VncActive() && VncActive()->session && VncActive()->session->ViewOnly() ? " (on)" : " (off)"),
        IdmVncViewOnly);
    add("VNC: Send Ctrl+Alt+Del", IdmVncCtrlAltDel);
    add("VNC: Send Clipboard to Server", IdmVncSendClipboard);
    add("Stop Reconnecting", IdmGuardianStop);
    // Command blocks — every action names the block it acts on so the
    // palette entry reads the same way the menu does.
    add("Block: Copy Command", IdmBlockCopyCommand);
    add("Block: Copy Output", IdmBlockCopyOutput);
    add("Block: Copy Command + Output", IdmBlockCopyBoth);
    add("Block: Fold / Expand", IdmBlockFold);
    add("Block: Search in Output...", IdmBlockSearch);
    add("Block: Save as Snippet...", IdmBlockSnippet);
    add("Block: Bookmark", IdmBlockBookmark);
    add("Block: Previous Bookmark", IdmBlockPrevBookmark);
    add("Block: Next Bookmark", IdmBlockNextBookmark);
    add("Block: Type the Command (does not run it)", IdmBlockRerun);
    add("Block: Run the Command Now", IdmBlockRerunNow);
    add("Block: Notify When This One Finishes", IdmBlockNotify);
    add(std::string("Block Gutter") + onoff(m_blockGutter), IdmBlockGutter);
    // Panes — every operation the spec lists, discoverable by name.
    add("Pane: Zoom / Restore", IdmPaneZoom);
    add("Pane: Rotate the Split", IdmPaneRotate);
    add("Pane: Toggle Read-only", IdmPaneReadOnly);
    add("Pane: Close", IdmPaneClose);
    add("Pane: Next", IdmPaneFocusNext);
    add("Pane: Previous", IdmPaneFocusPrev);
    add("Pane: Focus Left", IdmPaneFocusLeft);
    add("Pane: Focus Right", IdmPaneFocusRight);
    add("Pane: Focus Up", IdmPaneFocusUp);
    add("Pane: Focus Down", IdmPaneFocusDown);
    add("Pane: Move Left", IdmPaneMoveLeft);
    add("Pane: Move Right", IdmPaneMoveRight);
    add("Pane: Move Up", IdmPaneMoveUp);
    add("Pane: Move Down", IdmPaneMoveDown);
    add("Pane: Swap With Left", IdmPaneSwapLeft);
    add("Pane: Swap With Right", IdmPaneSwapRight);
    add("Pane: Swap With Above", IdmPaneSwapUp);
    add("Pane: Swap With Below", IdmPaneSwapDown);
    add("Pane: Wider", IdmPaneGrow);
    add("Pane: Narrower", IdmPaneShrink);
    add("Pane: Taller", IdmPaneGrowV);
    add("Pane: Shorter", IdmPaneShrinkV);
    add("Broadcast: Choose Targets...", IdmBroadcastPick);
    add("Broadcast: Select All Panes", IdmBroadcastAll);
    add("Broadcast: STOP", IdmBroadcastStop);
    add("Split Vertical", IdmSplitVertical);
    add("Split Horizontal", IdmSplitHorizontal);
    add("Close Split", IdmSplitClose);
    add(std::string("Broadcast Input to Both Panes") + onoff(m_broadcast), IdmBroadcast);
    add("Toggle Fullscreen", IdmViewFullscreen);
    {
        const std::vector<amber::LocalShell> shells = amber::DiscoverLocalShells();
        for (size_t i = 0; i < shells.size() && i < 32; ++i)
            add("New Local: " + shells[i].name, IdmLocalFirst + static_cast<int>(i));
    }
    add("SFTP Panel", IdmSftpPanel);
    add("Import Profiles (PuTTY / OpenSSH config)", IdmImportProfiles);
    add("Search Scrollback", IdmSearchScrollback);
    add("Log Session to File", IdmLogSession);
    add("Customize Theme...", IdmThemeEdit);
    add("Diagnostic Screen", IdmViewDiag);
    add("Font Size Larger", IdmViewFontLarger);
    add("Font Size Smaller", IdmViewFontSmaller);

    // Radio groups
    for (int i = 0; i < kThemeCount - 1; ++i)
        add("Theme: " + Utf8FromWide(kThemes[i].name), IdmThemeFirst + i);
    add("Theme: Custom", IdmThemeFirst + kThemeCount - 1);
    static const char* kAppear[4] = { "Dark", "Light", "Paperwhite", "Pixel Art" };
    for (int i = 0; i < 4; ++i)
        add(std::string("Appearance: ") + kAppear[i], IdmAppearanceFirst + i);
    for (int i = 0; i < kMotionStyleCount; ++i)
        add(std::string("Motion: ") + kMotionStyles[i].name, IdmMotionFirst + i);
    add("Motion: Reduced Motion (toggle)", IdmReducedMotion);
    {
        static const char* kDepart[5] = { "Fade", "Ash", "Smoke", "Sand", "Shatter" };
        for (int i = 0; i < 5; ++i)
            add(std::string("Departure: ") + kDepart[i], IdmDepartFirst + i);
    }
    add(std::string("Depth Parallax") + onoff(m_fxParallax), IdmFxParallax);
    add(std::string("Window Slosh") + onoff(m_fxSlosh), IdmFxSlosh);
    add(std::string("Phosphor Warm-up") + onoff(m_fxWarmup), IdmFxWarmup);
    add(std::string("Spotlight Running Command") + onoff(m_fxSpotlight), IdmFxSpotlight);
    {
        static const char* kNight[3] = { "Off", "After dark", "Always warm" };
        for (int i = 0; i < 3; ++i)
            add(std::string("Night Shift: ") + kNight[i], IdmNightFirst + i);
    }
    add("Reset Time Dial", IdmTimeDialReset);
    add("About Amber SSH", IdmAbout);
    add("Command Journal", IdmJournal);
    add("Command Journal: Record Commands (toggle)", IdmJournalCapture);
    add("Command Journal: Clear", IdmJournalClear);
    static const char* kSpeed[4] = { "Slow 0.5x", "Normal 1x", "Fast 1.5x", "Hyper 2x" };
    for (int i = 0; i < 4; ++i)
        add(std::string("Speed: ") + kSpeed[i], IdmSpeedFirst + i);
    for (int i = 0; i < kFontFaceCount; ++i)
        add("Font: " + Utf8FromWide(kFontFaces[i]), IdmFontFaceFirst + i);
    static const char* kFontStyle[3] = { "Modern", "Dot Matrix 8-pin", "Dot Matrix 12-pin" };
    for (int i = 0; i < 3; ++i)
        add(std::string("Font Style: ") + kFontStyle[i], IdmFontStyleFirst + i);
    static const char* kBg[4] = { "Off", "Embers", "Starfield", "Cosmic Dust" };
    for (int i = 0; i < 4; ++i)
        add(std::string("Background: ") + kBg[i], IdmBgFirst + i);
    static const char* kShadow[4] = { "Off", "Soft", "Medium", "Strong" };
    for (int i = 0; i < 4; ++i)
        add(std::string("Text Shadow: ") + kShadow[i], IdmShadowFirst + i);
    static const char* kBloom[3] = { "Low", "Medium", "High" };
    for (int i = 0; i < 3; ++i)
        add(std::string("Bloom Level: ") + kBloom[i], IdmBloomFirst + i);
    static const char* kTwinkle[3] = { "Off", "Subtle", "Full" };
    for (int i = 0; i < 3; ++i)
        add(std::string("Twinkle: ") + kTwinkle[i], IdmTwinkleFirst + i);
    static const char* kTrail[4] = { "Off", "Low", "Medium", "High" };
    for (int i = 0; i < 4; ++i)
        add(std::string("Trails: ") + kTrail[i], IdmTrailFirst + i);
    for (int i = 0; i < 5; ++i)
        add(std::string("Density: ") + kDensityNames[i], IdmDensityFirst + i);
    add("Density: Auto", IdmDensityAuto);
    add("Colors: Amber Miami", IdmPaletteFirst + 0);
    add("Colors: Classic xterm", IdmPaletteFirst + 1);
    add("Render: Particles + Crisp Core", IdmFxCrispCore);
    add("Render: Particles Only", IdmFxParticlesOnly);

    // Toggles
    add(std::string("Bloom") + onoff(m_fxBloom), IdmFxBloom);
    add(std::string("Scanlines") + onoff(m_fxScanlines), IdmFxScanlines);
    add(std::string("Vignette") + onoff(m_fxVignette), IdmFxVignette);
    add(std::string("Gas Drift") + onoff(m_fxDrift), IdmFxDrift);
    add(std::string("Pointer Force Field") + onoff(m_fxPointerForce), IdmFxPointerForce);
    add(std::string("Miami Sunset Selection") + onoff(m_miamiSelection), IdmFxMiami);
    add(std::string("Visual Bell") + onoff(m_fxBell), IdmFxBell);
    add(std::string("Shockwave on Enter") + onoff(m_fxShockwave), IdmFxShockwave);
    add(std::string("Cascade Output") + onoff(m_fxCascade), IdmFxCascade);
    add(std::string("Syntax Tint") + onoff(m_syntaxTint), IdmViewSyntaxTint);
    add(std::string("VSync") + onoff(m_vsync), IdmViewVsync);
    add(std::string("Stats Overlay") + onoff(m_showOverlay), IdmViewOverlay);
    add(std::string("Status Bar") + onoff(m_statusBar), IdmStatusBar);
    add(std::string("Confirm Multi-line Paste") + onoff(m_pasteGuard), IdmPasteGuard);
    add(std::string("Privacy Cloak") + onoff(m_cloak.enabled), IdmCloak);
    add(std::string("Privacy Cloak: Mask IP Addresses") + onoff(m_cloak.ipAddresses),
        IdmCloakAddrs);
    add(std::string("Privacy Cloak: Mask Home Directory Names") +
            onoff(m_cloak.homeDirectories),
        IdmCloakHome);
    for (int i = 0; i <= 4; ++i)
        add(std::string("Confirm Risky Commands: ") +
                amber::RiskPolicyName(static_cast<amber::RiskPolicy>(i)) +
                (static_cast<int>(m_riskPolicy) == i ? "  (current)" : ""),
            IdmRiskFirst + i);
    add("Remote Display: Find X Servers", IdmXServerReport);
    add("Remote Display: Start the X Server", IdmXServerStart);
    add("Remote Display: Wayland RemoteApp (start Weston)", IdmRemoteApp);
    add("Remote Display: Wayland RemoteApp (tunnel only)", IdmRemoteAppTunnel);
    add("Remote Display: Setup Guide", IdmRemoteDisplayDocs);
    add("Previous Command Mark", IdmPrevCommand);
    add("Next Command Mark", IdmNextCommand);
    add("Paste", IdmPaste);
    add("Fold Output at Cursor", IdmFoldToggle);
    add("Fold All Output", IdmFoldAll);
    add("Expand All Output", IdmFoldNone);
    add(std::string("Activity Heat Map") + onoff(m_fxHeat), IdmFxHeat);
    add(std::string("Latency Ghosting") + onoff(m_fxGhost), IdmFxGhost);
    add(std::string("Audio-Reactive Turbulence") + onoff(m_fxAudio), IdmFxAudio);
    add(std::string("Boot Sequence on Connect") + onoff(m_fxBoot), IdmFxBoot);
    static const char* kSaver[4] = { "Off", "1 minute", "5 minutes", "15 minutes" };
    for (int i = 0; i < 4; ++i)
        add(std::string("Screensaver (Digital Rain): ") + kSaver[i], IdmSaverFirst + i);

    // Snippets, triggers, recording, security.
    for (size_t i = 0; i < m_snippets.size(); ++i)
        add("Snippet: " + m_snippets[i].name, IdmSnippetFirst + static_cast<int>(i));
    add("Edit Snippets...", IdmEditSnippets);
    add("Edit Output Triggers...", IdmEditTriggers);
    add(std::string(HasSession() && Cur().castFile ? "Stop Recording (asciinema)"
                                                    : "Record Session (asciinema)..."),
        IdmRecordCast);
    add("Play Recording (.cast)...", IdmPlayCast);
    add(std::string("Windows Hello for Stored Secrets") + onoff(m_helloUnlock),
        IdmHelloUnlock);
    add("Port Forwarding / Jump Host...", IdmForwardsEdit);
    add(std::string("Remote Vitals in Title Bar") + onoff(m_vitalsOn), IdmVitals);
    add(std::string("Quake Mode (Ctrl+` dropdown)") + onoff(m_quake), IdmQuakeMode);
    static const char* kSharp[3] = { "Soft", "Crisp", "Razor" };
    for (int i = 0; i < 3; ++i)
        add(std::string("Text Sharpness: ") + kSharp[i], IdmSharpFirst + i);
    add(std::string("Live Effects (marks, pulse, echo, comet, weather)") + onoff(m_fxLive),
        IdmFxLive);
    add(std::string("Phosphor Persistence") + onoff(m_fxPersist), IdmFxPersist);
    add(std::string("Cube Session Switch (Compiz)") + onoff(m_fxCube), IdmFxCube);
    for (int i = 0; i < amber::kChromeCount; ++i)
        add(std::string("Interface Style: ") + amber::ChromeAt(i).name, IdmChromeFirst + i);
    add("Next Session (Ctrl+Tab)", IdmNextTab);
    add("Previous Session (Ctrl+Shift+Tab)", IdmPrevTab);
}

void App::FilterPalette()
{
    auto lower = [](std::string s) {
        for (char& ch : s)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string q = lower(m_palQuery);
    std::vector<std::pair<int, int>> scored;   // (score, index)
    for (size_t i = 0; i < m_palAll.size(); ++i)
    {
        const std::string l = lower(m_palAll[i].label);
        int score = 0;
        if (!q.empty())
        {
            // Subsequence match; adjacency and word-start hits score extra so
            // "mot glit" finds "Motion: Glitch" ahead of scattered matches.
            size_t from = 0;
            int prev = -2;
            bool ok = true;
            for (char qc : q)
            {
                if (qc == ' ')
                    continue;
                size_t f = l.find(qc, from);
                if (f == std::string::npos)
                {
                    ok = false;
                    break;
                }
                score += 10;
                if (static_cast<int>(f) == prev + 1)
                    score += 8;
                if (f == 0 || l[f - 1] == ' ' || l[f - 1] == ':')
                    score += 6;
                prev = static_cast<int>(f);
                from = f + 1;
            }
            if (!ok)
                continue;
            score -= static_cast<int>(l.size()) / 8;   // shorter wins ties
        }
        scored.push_back({ score, static_cast<int>(i) });
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    m_palHits.clear();
    for (const auto& s : scored)
        m_palHits.push_back(s.second);
    m_palSel = 0;
}

void App::ToggleJournal()
{
    m_jrnOpen = !m_jrnOpen;
    if (!m_jrnOpen)
        return;
    m_palOpen = false;              // one overlay at a time
    m_journal.Load();
    m_jrnQuery.clear();
    m_jrnSel = 0;
    FilterJournal();
    if (m_journal.Entries().empty())
        SetStatus("Command journal is empty — it fills as you run commands "
                  "on a host with shell integration (OSC 133)", 6.0);
}

void App::FilterJournal()
{
    m_jrnHits = m_journal.Search(m_jrnQuery);
    m_jrnSel = 0;
}

bool App::JournalKey(WPARAM vk)
{
    const int n = static_cast<int>(m_jrnHits.size());
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    auto selected = [&]() -> const amber::JournalEntry* {
        if (n <= 0 || m_jrnSel < 0 || m_jrnSel >= n)
            return nullptr;
        return &m_journal.Entries()[m_jrnHits[m_jrnSel]];
    };
    switch (vk)
    {
    case VK_ESCAPE:
        m_jrnOpen = false;
        m_swallowChar = true;
        return true;
    case VK_RETURN:
    {
        const amber::JournalEntry* e = selected();
        m_jrnOpen = false;
        m_swallowChar = true;
        if (!e || !HasSession() || !Cur().Live())
            return true;
        // Enter types the command at the prompt WITHOUT running it, so a
        // destructive line recalled by accident can still be read and edited.
        // Ctrl+Enter is the deliberate "run it" gesture.
        std::string text = e->command;
        if (ctrl)
            text += "\r";
        SendToShell(text);
        SetStatus(ctrl ? "Journal: ran command" : "Journal: inserted command");
        return true;
    }
    case 'C':
        if (ctrl)
        {
            if (const amber::JournalEntry* e = selected())
            {
                SetClipboardText(e->command);
                SetStatus("Journal: command copied");
            }
            m_swallowChar = true;
            return true;
        }
        return true;
    case 'J':
        // Jump to the live block this entry came from, when it is still on
        // screen. The link is only meaningful inside the session that
        // recorded it and only while its rows survive; anything else says so
        // rather than jumping somewhere arbitrary.
        if (ctrl)
        {
            m_swallowChar = true;
            const amber::JournalEntry* e = selected();
            if (!e || !HasSession())
                return true;
            if (e->blockId == 0)
            {
                SetStatus("That entry predates command blocks — no live block "
                          "to jump to.", 6.0);
                return true;
            }
            amber::Session& s = Foc();
            const std::string key = s.profile.id.empty() ? s.label : s.profile.id;
            if (!e->sessionKey.empty() && e->sessionKey != key)
            {
                SetStatus("That command ran in a different session.", 5.0);
                return true;
            }
            const amber::CommandBlock* b = amber::BlockById(s.blocks, e->blockId);
            if (!b)
            {
                SetStatus("That command has scrolled out of this session's "
                          "buffer.", 6.0);
                return true;
            }
            m_jrnOpen = false;
            RevealRow(s, b->promptRow);
            SetStatus("Jumped to \"" + e->command + "\"", 4.0);
            return true;
        }
        return true;
    case VK_DELETE:
        if (n > 0)
        {
            m_journal.Remove(m_jrnHits[m_jrnSel]);
            FilterJournal();
            m_jrnSel = std::min(m_jrnSel, std::max(0, static_cast<int>(m_jrnHits.size()) - 1));
        }
        return true;
    case VK_UP:    m_jrnSel = std::max(0, m_jrnSel - 1);            return true;
    case VK_DOWN:  m_jrnSel = std::min(std::max(0, n - 1), m_jrnSel + 1); return true;
    case VK_PRIOR: m_jrnSel = std::max(0, m_jrnSel - 10);           return true;
    case VK_NEXT:  m_jrnSel = std::min(std::max(0, n - 1), m_jrnSel + 10); return true;
    case VK_HOME:  m_jrnSel = 0;                                    return true;
    case VK_END:   m_jrnSel = std::max(0, n - 1);                   return true;
    case VK_BACK:
        while (!m_jrnQuery.empty() &&
               (static_cast<unsigned char>(m_jrnQuery.back()) & 0xC0) == 0x80)
            m_jrnQuery.pop_back();
        if (!m_jrnQuery.empty())
            m_jrnQuery.pop_back();
        FilterJournal();
        m_swallowChar = true;
        return true;
    default:
        return true;   // the overlay owns the keyboard; text arrives via OnChar
    }
}

bool App::PaletteKey(WPARAM vk)
{
    const int n = static_cast<int>(m_palHits.size());
    switch (vk)
    {
    case VK_ESCAPE:
        m_palOpen = false;
        m_swallowChar = true;
        return true;
    case VK_RETURN:
    {
        int cmd = (n > 0) ? m_palAll[m_palHits[m_palSel]].cmd : 0;
        m_palOpen = false;
        m_swallowChar = true;
        if (cmd)
            HandleMenuCommand(cmd);
        return true;
    }
    case VK_UP:    m_palSel = std::max(0, m_palSel - 1);            return true;
    case VK_DOWN:  m_palSel = std::min(std::max(0, n - 1), m_palSel + 1); return true;
    case VK_PRIOR: m_palSel = std::max(0, m_palSel - 10);           return true;
    case VK_NEXT:  m_palSel = std::min(std::max(0, n - 1), m_palSel + 10); return true;
    case VK_HOME:  m_palSel = 0;                                     return true;
    case VK_END:   m_palSel = std::max(0, n - 1);                    return true;
    case VK_BACK:
        // Drop one UTF-8 codepoint.
        while (!m_palQuery.empty() &&
               (static_cast<unsigned char>(m_palQuery.back()) & 0xC0) == 0x80)
            m_palQuery.pop_back();
        if (!m_palQuery.empty())
            m_palQuery.pop_back();
        FilterPalette();
        m_swallowChar = true;
        return true;
    default:
        return true;   // the palette owns the keyboard; text arrives via OnChar
    }
}

bool App::PasteGuardKey(WPARAM vk)
{
    switch (vk)
    {
    case VK_ESCAPE:
        m_pasteOpen = false;
        m_pastePending.clear();
        m_swallowChar = true;
        SetStatus("Paste cancelled");
        return true;
    case VK_RETURN:
    {
        std::string text = m_pastePending;
        m_pasteOpen = false;
        m_pastePending.clear();
        m_swallowChar = true;
        SendPasteText(text);
        SetStatus("Pasted " + std::to_string(m_pasteLines) + " line" +
                  (m_pasteLines == 1 ? "" : "s"));
        return true;
    }
    default:
        return true;   // the guard owns the keyboard until it is answered
    }
}

void App::SetPanelRect(float x, float y, float w, float h)
{
    m_panelRect = { x, y, w, h };
    m_particles.tun.panelX = x;
    m_particles.tun.panelY = y;
    m_particles.tun.panelW = w;
    m_particles.tun.panelH = h;
}

void App::DrawPasteGuard()
{
    if (!m_pasteOpen)
        return;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const float lineH = (m_gm.cellH > 0.0f) ? m_gm.cellH * 1.25f : 24.0f * dpi;
    const float pad = 12.0f * dpi;
    const float W = static_cast<float>(m_device.Width());
    const float boxW = std::min(W - 2.0f * pad, 820.0f * dpi);

    // Show the first few lines and say how many more there are: enough to see
    // what is about to run without turning the guard into a text editor.
    std::vector<std::string> preview;
    {
        std::string cur;
        for (char c : m_pastePending)
        {
            if (c == '\r')
            {
                preview.push_back(cur);
                cur.clear();
                if (preview.size() >= 8)
                    break;
            }
            else
                cur.push_back(c);
        }
        if (preview.size() < 8 && !cur.empty())
            preview.push_back(cur);
    }
    const int shown = static_cast<int>(preview.size());
    const int hidden = std::max(0, m_pasteLines - shown);
    const float boxH = pad * 2.0f + lineH * static_cast<float>(shown + 3) +
                       8.0f * dpi;
    const float x0 = (W - boxW) * 0.5f;
    const float y0 = m_titleBarH + 40.0f * dpi;
    const float bw = std::max(1.0f, 1.5f * dpi);

    float bg[4] = { 0.03f, 0.012f, 0.004f, 1.0f };   // fully opaque
    m_prims.AddRectRgba(x0, y0, boxW, boxH, bg, 0.0f, PrimLayer::OverBlend);
    SetPanelRect(x0, y0, boxW, boxH);
    // The frame is the danger colour: this is a confirmation, not a menu.
    float warn[3];
    if (amber::ChromeSkinned())
        SrgbToLinear(amber::Chrome().danger, warn);
    else
    { warn[0] = 1.0f; warn[1] = 0.42f; warn[2] = 0.12f; }
    float border[4] = { warn[0], warn[1], warn[2], 0.95f };
    m_prims.AddRectRgba(x0, y0, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0 + boxH - bw, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0, bw, boxH, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0 + boxW - bw, y0, bw, boxH, border, 0.0f, PrimLayer::Over);

    const float tx = x0 + pad;
    float y = y0 + pad;
    char head[160];
    snprintf(head, sizeof(head),
             "Paste %d line%s into %s?", m_pasteLines,
             m_pasteLines == 1 ? "" : "s",
             HasSession() ? Cur().Caption().c_str() : "this session");
    m_prims.AddTextRgb(tx, y, head, warn, m_sampler);
    y += lineH + 4.0f * dpi;

    for (const std::string& l : preview)
    {
        std::string s = l;
        const float room = boxW - pad * 2.0f;
        if (m_prims.MeasureText(s, m_sampler) > room)
        {
            while (!s.empty() &&
                   m_prims.MeasureText(s + "\xE2\x80\xA6", m_sampler) > room)
            {
                s.pop_back();
                while (!s.empty() &&
                       (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
                    s.pop_back();
            }
            s += "\xE2\x80\xA6";
        }
        m_prims.AddText(tx, y, s.empty() ? " " : s, 0.80f, m_sampler);
        y += lineH;
    }
    if (hidden > 0)
    {
        char more[64];
        snprintf(more, sizeof(more), "... and %d more line%s", hidden,
                 hidden == 1 ? "" : "s");
        m_prims.AddText(tx, y, more, 0.45f, m_sampler);
    }
    m_prims.AddText(tx, y0 + boxH - pad - lineH * 0.9f,
                    "enter paste   esc cancel", 0.45f, m_sampler);
}

// Session Guardian annotations. These are drawn OVER the grid at an absolute
// row id — the same anchoring the tide marks and the error embers use — and
// are never fed to the parser, so a "connection lost" line cannot end up in a
// selection, a scrollback search or a session log. The grid stays the record
// of what the server actually sent.
//
// Skinned like every other surface: the colours come from the active
// ChromeSpec, and on a light ground the additive text path is swapped for
// opaque core glyphs, because adding light to paper does nothing.
void App::DrawNotices()
{
    if (!HasSession() || m_minimized)
        return;
    amber::Session& F = Foc();
    if (F.notices.empty())
        return;

    int co = 0, ro = 0;
    PaneOffset(F, co, ro);
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const int rows = F.grid.Rows(), cols = F.grid.Cols();
    const float paneX = m_gm.originX + static_cast<float>(co) * m_gm.cellW;
    const float paneW = static_cast<float>(cols) * m_gm.cellW;
    const int64_t top = static_cast<int64_t>(F.grid.TotalPushed()) - F.grid.ViewOffset();

    // A reconnect resets the grid, and every row id from before it then
    // points nowhere. Re-anchor rather than discard: "the link went away" is
    // exactly the annotation worth keeping across the reset.
    const uint64_t pushed = F.grid.TotalPushed();
    if (pushed < F.noticesPushedSeen)
        for (amber::Session::Notice& n : F.notices)
            n.rowId = pushed;
    F.noticesPushedSeen = pushed;

    // Notices whose row has fallen out of the scrollback have nothing left to
    // point at.
    const uint64_t oldest = pushed - static_cast<uint64_t>(F.grid.ScrollbackSize());
    F.notices.erase(std::remove_if(F.notices.begin(), F.notices.end(),
                                   [oldest](const amber::Session::Notice& n)
                                   { return n.rowId + 1 < oldest; }),
                    F.notices.end());

    const amber::ChromeSpec& ch = amber::Chrome();
    const bool skin = amber::ChromeSkinned();
    const bool lightGround = m_appearance == 1 || m_appearance == 2;
    // Several notices can land on one row — a drop and its recovery bracket a
    // grid reset, so both end up anchored to the same line. They stack
    // upwards from it instead of overprinting each other.
    std::map<int64_t, int> slots;

    for (const amber::Session::Notice& n : F.notices)
    {
        const int64_t vr = static_cast<int64_t>(n.rowId) - top;
        if (vr < 0 || vr >= rows)
            continue;
        const int slot = slots[vr]++;

        // Warning takes the skin's danger colour, recovery its primary accent,
        // plain information its secondary one — so every style says the same
        // three things in its own palette.
        float rgb[3];
        if (skin)
            SrgbToLinear(n.kind == 1 ? ch.danger : n.kind == 2 ? ch.neonA : ch.neonB,
                         rgb);
        else if (n.kind == 1)
        { rgb[0] = 1.00f; rgb[1] = 0.38f; rgb[2] = 0.10f; }
        else if (n.kind == 2)
        { rgb[0] = 0.20f; rgb[1] = 0.95f; rgb[2] = 0.45f; }
        else
            AmberRampCpu(0.75f, rgb);

        // A skin's accent is chosen to read on the CHROME's ground, which is
        // not this one. Letterpress's near-black red on the terminal's black
        // is invisible; lift (or on paper, deepen) it until it carries.
        float ink3[3] = { rgb[0], rgb[1], rgb[2] };
        {
            const float luma = 0.2126f * ink3[0] + 0.7152f * ink3[1] + 0.0722f * ink3[2];
            if (!lightGround && luma < 0.20f && luma > 0.0f)
            {
                const float k = std::min(6.0f, 0.20f / luma);
                for (float& c : ink3)
                    c = std::min(1.0f, c * k);
            }
            else if (lightGround && luma > 0.55f)
            {
                const float k = 0.55f / luma;
                for (float& c : ink3)
                    c *= k;
            }
        }

        // A notice cools to a scar after half a minute rather than vanishing:
        // it is the record of where the link broke.
        const float age = static_cast<float>(m_time - n.t);
        const float a = age < 30.0f ? 1.0f - 0.55f * (age / 30.0f) : 0.45f;
        const float y = m_gm.originY + static_cast<float>(vr + ro) * m_gm.cellH;
        const float hair = std::max(1.0f, 1.4f * dpi);
        const PrimLayer ink = lightGround ? PrimLayer::OverBlend : PrimLayer::Over;

        // The label sits at the right-hand end of the rule, on its own ground,
        // so it never has to fight the terminal text underneath it.
        const float room = paneW * 0.92f;
        std::string body = n.text;
        if (m_prims.MeasureText(" " + body + " ", m_sampler) > room)
        {
            // Trim whole code points off the end, then mark the cut.
            while (!body.empty() &&
                   m_prims.MeasureText(" " + body + "\xE2\x80\xA6 ", m_sampler) > room)
            {
                body.pop_back();
                while (!body.empty() &&
                       (static_cast<unsigned char>(body.back()) & 0xC0) == 0x80)
                    body.pop_back();
            }
            body += "\xE2\x80\xA6";
        }
        const std::string label = " " + body + " ";
        const float tw = m_prims.MeasureText(label, m_sampler);
        const float lx = paneX + paneW - tw;
        // Stacked DOWNWARDS from the row. Upwards loses the newest notice off
        // the top of the pane, and the newest one — "reconnected" — is the one
        // the reader most wants.
        const float ly = y - m_gm.cellH * 0.5f +
                         m_gm.cellH * static_cast<float>(slot);
        if (ly + m_gm.cellH > m_gm.originY + static_cast<float>(rows) * m_gm.cellH)
            continue;                     // no room left below: drop this one

        float rule[4] = { ink3[0], ink3[1], ink3[2], 0.55f * a };
        // The rule stops where the label starts. Drawn across it, the
        // additive Over pass lands on top of the blended label ground and
        // strikes the text through.
        const float ruleW = std::max(0.0f, lx - paneX - 6.0f * dpi);
        if (slot == 0 && ruleW > 0.0f)
        {
            m_prims.AddRectRgba(paneX, y, ruleW, hair, rule, 0.0f, ink);
            // A stub in the left margin, matching the tide marks, so the eye
            // finds the row even when the label is trimmed away.
            m_prims.AddRectRgba(paneX - 5.0f * dpi, y - 2.0f * dpi, 4.0f * dpi,
                                hair + 4.0f * dpi, rule, 0.0f, ink);
        }

        // The label's ground is the TERMINAL's, not the chrome's: the notice
        // sits over the grid, not on a strip. Painting it with ChromeSpec::bg
        // put a near-white plate under a dark-skin accent on the light skins
        // and swallowed the text whole.
        float pad[4] = { 0.0f, 0.0f, 0.0f, lightGround ? 0.92f : 0.88f };
        if (lightGround)
        { pad[0] = 0.92f; pad[1] = 0.91f; pad[2] = 0.88f; }
        m_prims.AddRectRgba(lx, ly, tw, m_gm.cellH, pad, 0.0f, PrimLayer::OverBlend);
        if (lightGround)
            m_prims.AddTextCore(lx, ly, label, ink3, 1.0f, m_sampler);
        else
            m_prims.AddTextRgb(lx, ly, label, ink3, m_sampler);
    }
}

// Command blocks, as a gutter. A hairline bar in the left margin spanning
// each block's output, coloured by outcome; the block under the pointer
// brightens and shows its metadata at the right-hand end of its first row.
//
// Restrained on purpose: this is an annotation layer over the grid, drawn
// from block metadata. Nothing is inserted into the terminal stream, so the
// summary line cannot appear in a copy, a search or a session log — the same
// rule the guardian notices follow.
void App::DrawBlockGutter()
{
    if (!m_blockGutter || !HasSession() || m_minimized)
        return;
    amber::Session& F = Foc();
    if (F.blocks.empty() || F.grid.AltActive())
        return;

    int co = 0, ro = 0;
    PaneOffset(F, co, ro);
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const int rows = F.grid.Rows(), cols = F.grid.Cols();
    const float paneX = m_gm.originX + static_cast<float>(co) * m_gm.cellW;
    const float paneW = static_cast<float>(cols) * m_gm.cellW;
    const int64_t top = static_cast<int64_t>(F.grid.TotalPushed()) -
                        F.grid.ViewOffset();

    const amber::ChromeSpec& ch = amber::Chrome();
    const bool skin = amber::ChromeSkinned();
    const bool lightGround = m_appearance == 1 || m_appearance == 2;
    const PrimLayer ink = lightGround ? PrimLayer::OverBlend : PrimLayer::Over;

    // Which block owns each DISPLAY row. Going through the fold map rather
    // than computing view rows arithmetically is what makes the bar follow
    // what is actually drawn: a collapsed block occupies one row on screen,
    // not the hundred-and-twenty it references.
    //
    // Merged walk, not a search per row. Blocks and display rows both ascend,
    // so one cursor over each answers every row in a single pass — a
    // BlockIndexAtRow per row would be rows x blocks every frame, which is
    // 200,000 comparisons once a session has a few thousand of them.
    std::vector<int> rowBlock(static_cast<size_t>(std::max(0, rows)), -1);
    {
        size_t cur = 0;
        int64_t prevSrc = -1;
        for (int d = 0; d < rows; ++d)
        {
            const int src = (d < static_cast<int>(F.rowMap.size())) ? F.rowMap[d].src : d;
            const uint64_t abs = static_cast<uint64_t>(top + src);
            // The fold map is monotonic, but be defensive: a row that went
            // backwards restarts the cursor rather than silently mismatching.
            if (src < prevSrc)
                cur = 0;
            prevSrc = src;
            while (cur < F.blocks.size())
            {
                const amber::CommandBlock& b = F.blocks[cur];
                const uint64_t last = b.hasOutput ? b.outputLast
                                                  : std::max(b.promptRow, b.inputRow);
                if (last >= abs)
                    break;
                ++cur;
            }
            if (cur < F.blocks.size() && abs >= F.blocks[cur].promptRow)
                rowBlock[static_cast<size_t>(d)] = static_cast<int>(cur);
        }
    }

    // The block under the pointer, in the same display-row space.
    int hoverIdx = -1;
    if (m_gm.cellH > 0.0f)
    {
        const int hd = static_cast<int>((static_cast<float>(m_lastMousePy) -
                                         m_gm.originY) / m_gm.cellH) - ro;
        const float hx = static_cast<float>(m_lastMousePx);
        if (hd >= 0 && hd < rows && hx >= paneX - 8.0f * dpi && hx <= paneX + paneW)
            hoverIdx = rowBlock[static_cast<size_t>(hd)];
    }

    auto outcomeColour = [&](const amber::CommandBlock& b, float out[3]) {
        if (b.running)
        {
            if (skin) SrgbToLinear(ch.neonB, out);
            else AmberRampCpu(0.85f, out);
        }
        else if (b.Failed())
        {
            if (skin) SrgbToLinear(ch.danger, out);
            else { out[0] = 1.0f; out[1] = 0.30f; out[2] = 0.10f; }
        }
        else if (b.Unknown())
        {
            // Neither succeeded nor failed. A third colour, because painting
            // it as either would be a claim AmberSSH cannot make.
            out[0] = out[1] = out[2] = 0.62f;
        }
        else
        {
            if (skin) SrgbToLinear(ch.neonA, out);
            else AmberRampCpu(0.70f, out);
        }
    };

    // Walk the display rows, emitting one bar per run of rows owned by the
    // same block.
    for (int d = 0; d < rows;)
    {
        const int idx = rowBlock[static_cast<size_t>(d)];
        if (idx < 0)
        {
            ++d;
            continue;
        }
        int e = d;
        while (e + 1 < rows && rowBlock[static_cast<size_t>(e + 1)] == idx)
            ++e;
        const amber::CommandBlock& b = F.blocks[static_cast<size_t>(idx)];
        const int64_t a = d, z = e;
        const size_t i = static_cast<size_t>(idx);
        d = e + 1;

        float rgb[3];
        outcomeColour(b, rgb);
        const bool hot = static_cast<int>(i) == hoverIdx;
        const float alpha = b.bookmarked ? 0.95f : hot ? 0.85f : 0.45f;
        const float w = (b.bookmarked || hot) ? 3.0f * dpi : 2.0f * dpi;
        const float y0 = m_gm.originY + static_cast<float>(a + ro) * m_gm.cellH;
        const float y1 = m_gm.originY + static_cast<float>(z + 1 + ro) * m_gm.cellH;
        float bar[4] = { rgb[0], rgb[1], rgb[2], alpha };
        m_prims.AddRectRgba(paneX - 6.0f * dpi, y0, w, y1 - y0, bar, 0.0f, ink);

        // The metadata line: only for the block under the pointer, and only
        // for one that has finished. A running block gets the elapsed clock
        // below instead.
        if (!hot || b.running)
            continue;
        std::string meta = amber::FormatOutcome(b);
        meta += "  " + amber::FormatDuration(b.durationSec);
        if (b.lines > 0)
            meta += "  " + std::to_string(b.lines) +
                    (b.lines == 1 ? " line" : " lines");
        if (b.bytes > 0)
            meta += "  " + amber::FormatBytes(b.bytes);
        if (!b.cwd.empty())
            meta += "  " + b.cwd;
        if (b.startedAt > 0)
        {
            const std::time_t t = static_cast<std::time_t>(b.startedAt);
            std::tm tmv{};
            if (localtime_s(&tmv, &t) == 0)
            {
                char clock[16];
                snprintf(clock, sizeof(clock), "  %02d:%02d", tmv.tm_hour, tmv.tm_min);
                meta += clock;
            }
        }
        meta = " " + meta + " ";

        float tw = m_prims.MeasureText(meta, m_sampler);
        if (tw > paneW * 0.9f)
            continue;                     // no room: the bar alone will do
        const float lx = paneX + paneW - tw;
        const float ly = m_gm.originY + static_cast<float>(a + ro) * m_gm.cellH;
        float pad[4] = { 0.0f, 0.0f, 0.0f, lightGround ? 0.92f : 0.88f };
        if (lightGround)
        { pad[0] = 0.92f; pad[1] = 0.91f; pad[2] = 0.88f; }
        m_prims.AddRectRgba(lx, ly, tw, m_gm.cellH, pad, 0.0f, PrimLayer::OverBlend);
        if (lightGround)
            m_prims.AddTextCore(lx, ly, meta, rgb, 1.0f, m_sampler);
        else
            m_prims.AddTextRgb(lx, ly, meta, rgb, m_sampler);
    }

    // A running command counts up, at the right-hand end of the row its
    // output started on. This is the "show elapsed time" the spec asks for,
    // and it costs the shell nothing: the prompt is never touched.
    if (F.cmdRunning && F.outputStartRow > 0)
    {
        const int64_t vr = static_cast<int64_t>(F.outputStartRow) - top;
        if (vr >= 0 && vr < rows)
        {
            std::string t = " " + amber::FormatDuration(m_time - F.cmdStart);
            if (F.notifyRunning)
                t += "  will report";
            t += " ";
            const float tw = m_prims.MeasureText(t, m_sampler);
            const float lx = paneX + paneW - tw;
            const float ly = m_gm.originY + static_cast<float>(vr + ro) * m_gm.cellH;
            float rgb[3];
            if (skin) SrgbToLinear(ch.neonB, rgb);
            else AmberRampCpu(0.85f, rgb);
            float pad[4] = { 0.0f, 0.0f, 0.0f, lightGround ? 0.92f : 0.88f };
            if (lightGround)
            { pad[0] = 0.92f; pad[1] = 0.91f; pad[2] = 0.88f; }
            m_prims.AddRectRgba(lx, ly, tw, m_gm.cellH, pad, 0.0f, PrimLayer::OverBlend);
            if (lightGround)
                m_prims.AddTextCore(lx, ly, t, rgb, 1.0f, m_sampler);
            else
                m_prims.AddTextRgb(lx, ly, t, rgb, m_sampler);
        }
    }
}

// Read-only and broadcast markers, drawn over each pane from pane state —
// never written into any grid, like every other annotation in this build.
//
// A read-only pane has to SAY so: the whole point is that the user pointed it
// at a production log precisely because they do not trust themselves to be
// careful, and a lock they cannot see is not a lock they can rely on.
void App::DrawPaneBadges()
{
    if (!HasSession() || m_minimized)
        return;
    amber::Session& tab = Cur();
    if (tab.layout.Empty())
        return;
    const int gc = static_cast<int>(m_gm.cols), gr = static_cast<int>(m_gm.rows);
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const amber::ChromeSpec& ch = amber::Chrome();
    const bool skin = amber::ChromeSkinned();
    const bool lightGround = m_appearance == 1 || m_appearance == 2;

    for (const auto& [id, r] : tab.layout.Rects(gc, gr))
    {
        if (r.cols <= 0 || r.rows <= 0)
            continue;
        const amber::Session* p = PaneById(tab, id);
        if (!p)
            continue;
        const bool bcast = std::find(tab.broadcast.begin(), tab.broadcast.end(),
                                     id) != tab.broadcast.end();
        if (!p->readOnly && !bcast)
            continue;
        std::string label;
        float rgb[3];
        if (p->readOnly)
        {
            label = " READ-ONLY ";
            if (skin)
                SrgbToLinear(ch.textDim, rgb);
            else
                AmberRampCpu(0.55f, rgb);
        }
        else
        {
            label = " BROADCAST ";
            // The danger colour, because that is what this is.
            if (skin)
                SrgbToLinear(ch.danger, rgb);
            else
            { rgb[0] = 1.0f; rgb[1] = 0.35f; rgb[2] = 0.10f; }
        }
        // Lift a colour too dark for the terminal's ground, the same way the
        // guardian notices do.
        {
            const float luma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
            if (!lightGround && luma > 0.0f && luma < 0.20f)
            {
                const float k = std::min(6.0f, 0.20f / luma);
                for (float& c : rgb)
                    c = std::min(1.0f, c * k);
            }
        }
        const float tw = m_prims.MeasureText(label, m_sampler);
        if (tw > static_cast<float>(r.cols) * m_gm.cellW)
            continue;
        const float x = m_gm.originX + static_cast<float>(r.col + r.cols) * m_gm.cellW - tw;
        const float y = m_gm.originY + static_cast<float>(r.row) * m_gm.cellH;
        float pad[4] = { 0.0f, 0.0f, 0.0f, lightGround ? 0.92f : 0.88f };
        if (lightGround)
        { pad[0] = 0.92f; pad[1] = 0.91f; pad[2] = 0.88f; }
        m_prims.AddRectRgba(x, y, tw, m_gm.cellH, pad, 0.0f, PrimLayer::OverBlend);
        if (lightGround)
            m_prims.AddTextCore(x, y, label, rgb, 1.0f, m_sampler);
        else
            m_prims.AddTextRgb(x, y, label, rgb, m_sampler);
        // A broadcasting pane also gets a border, so the count in the status
        // bar is not the only thing standing between the user and four hosts.
        if (bcast)
        {
            float edge[4] = { rgb[0], rgb[1], rgb[2], 0.75f };
            const float px0 = m_gm.originX + static_cast<float>(r.col) * m_gm.cellW;
            const float py0 = y;
            const float pw = static_cast<float>(r.cols) * m_gm.cellW;
            const float ph = static_cast<float>(r.rows) * m_gm.cellH;
            const float t = std::max(1.0f, 1.5f * dpi);
            const PrimLayer ink = lightGround ? PrimLayer::OverBlend : PrimLayer::Over;
            m_prims.AddRectRgba(px0, py0, pw, t, edge, 0.0f, ink);
            m_prims.AddRectRgba(px0, py0 + ph - t, pw, t, edge, 0.0f, ink);
            m_prims.AddRectRgba(px0, py0, t, ph, edge, 0.0f, ink);
            m_prims.AddRectRgba(px0 + pw - t, py0, t, ph, edge, 0.0f, ink);
        }
    }
}

// ---- the Remote Apps shelf (AmberX, Phase 7) --------------------------------
// Every window the session's AmberX host is showing, with what it is doing and
// what can be done to it. Drawn in the terminal's own surface rather than as a
// separate window, so it takes the interface skin the way the palette and the
// journal do, and so it cannot be covered by a remote window: nothing a
// forwarded application draws reaches this layer.
void App::ToggleRemoteApps()
{
    m_appsOpen = !m_appsOpen;
    if (!m_appsOpen)
        return;
    m_palOpen = false;              // one overlay at a time
    m_jrnOpen = false;
    m_appsSel = 0;
}

// The windows worth listing: override-redirect ones are menus and tooltips,
// which come and go by the dozen and are not applications.
static std::vector<RemoteAppWindow> ShelfWindows(const RemoteAppReport& r)
{
    std::vector<RemoteAppWindow> out;
    for (const RemoteAppWindow& w : r.list)
        if (!(w.flags & 8u))
            out.push_back(w);
    return out;
}

bool App::RemoteAppsKey(WPARAM vk)
{
    if (!m_appsOpen)
        return false;
    const RemoteAppReport r = HasSession() ? Cur().ssh.AmberXReport() : RemoteAppReport{};
    const std::vector<RemoteAppWindow> wins = ShelfWindows(r);
    const int n = static_cast<int>(wins.size());
    auto act = [&](int action, const char* said) {
        if (n <= 0 || m_appsSel < 0 || m_appsSel >= n || !HasSession())
            return;
        Cur().ssh.RemoteAppAction(wins[static_cast<size_t>(m_appsSel)].xid, action);
        SetStatus(said);
    };
    switch (vk)
    {
    case VK_ESCAPE:
        m_appsOpen = false;
        m_swallowChar = true;
        return true;
    case VK_UP:
        if (m_appsSel > 0)
            --m_appsSel;
        return true;
    case VK_DOWN:
        if (m_appsSel + 1 < n)
            ++m_appsSel;
        return true;
    case VK_RETURN:
        act(0, "Remote app: shown");
        m_appsOpen = false;
        m_swallowChar = true;
        return true;
    case 'M':
        act(1, "Remote app: minimized");
        m_swallowChar = true;
        return true;
    case VK_DELETE:
    case 'C':
        // Asks the application to close, the same request its own close
        // button makes. It is never a kill: an editor with unsaved work gets
        // to say no, exactly as it would on the remote desktop.
        act(2, "Remote app: asked to close");
        m_swallowChar = true;
        return true;
    default:
        return true;                // the shelf owns the keyboard while open
    }
}

void App::DrawRemoteApps()
{
    if (!m_appsOpen)
        return;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const float lineH = (m_gm.cellH > 0.0f) ? m_gm.cellH * 1.25f : 24.0f * dpi;
    const float pad = 12.0f * dpi;
    const float W = static_cast<float>(m_device.Width());
    const float boxW = std::min(W - 2.0f * pad, 900.0f * dpi);

    const RemoteAppReport r = HasSession() ? Cur().ssh.AmberXReport() : RemoteAppReport{};
    const std::vector<RemoteAppWindow> wins = ShelfWindows(r);
    const int n = static_cast<int>(wins.size());
    const int rows = std::max(1, std::min(12, n));
    const float boxH = pad * 2.0f + lineH * static_cast<float>(rows + 3) + 8.0f * dpi;
    const float x0 = (W - boxW) * 0.5f;
    const float y0 = m_titleBarH + 24.0f * dpi;
    const float bw = std::max(1.0f, 1.5f * dpi);

    float bg[4] = { 0.02f, 0.015f, 0.005f, 1.0f };
    m_prims.AddRectRgba(x0, y0, boxW, boxH, bg, 0.0f, PrimLayer::OverBlend);
    SetPanelRect(x0, y0, boxW, boxH);
    float lin[3];
    AmberRampCpu(0.55f, lin);
    float border[4] = { lin[0], lin[1], lin[2], 0.9f };
    m_prims.AddRectRgba(x0, y0, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0 + boxH - bw, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0, bw, boxH, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0 + boxW - bw, y0, bw, boxH, border, 0.0f, PrimLayer::Over);

    const float tx = x0 + pad, ty = y0 + pad;
    std::string head = "remote apps";
    if (HasSession())
    {
        const amber::Session& s = Cur();
        head += "  " + s.profile.host;
        if (s.profile.x11Backend == 1)
            head += s.profile.x11Trust != 0 ? "  X11 TRUSTED" : "  X11 RESTRICTED";
    }
    m_prims.AddText(tx, ty, head, 1.0f, m_sampler);
    if (n > 0)
    {
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "%d of %d", m_appsSel + 1, n);
        const float cw = m_prims.MeasureText(cnt, m_sampler);
        m_prims.AddText(x0 + boxW - pad - cw, ty, cnt, 0.45f, m_sampler);
    }
    const float sepY = ty + lineH + 2.0f * dpi;
    float sep[4] = { lin[0], lin[1], lin[2], 0.45f };
    m_prims.AddRectRgba(x0 + pad, sepY, boxW - 2.0f * pad, bw, sep, 0.0f, PrimLayer::Over);

    const float listY = sepY + 6.0f * dpi;
    if (!r.valid)
    {
        m_prims.AddText(tx, listY,
                        HasSession() && Cur().profile.x11Backend == 1
                            ? "Waiting for the AmberX host"
                            : "This session has no remote GUI (SSH \xc2\xbb X11 in its profile)",
                        0.45f, m_sampler);
    }
    else if (n == 0)
    {
        m_prims.AddText(tx, listY, "No forwarded windows open", 0.45f, m_sampler);
    }
    else
    {
        const int first = std::max(0, std::min(m_appsSel - rows + 1, n - rows));
        for (int i = 0; i < rows && first + i < n; ++i)
        {
            const int idx = first + i;
            const RemoteAppWindow& w = wins[static_cast<size_t>(idx)];
            const float ry = listY + static_cast<float>(i) * lineH;
            if (idx == m_appsSel)
            {
                float selc[4] = { lin[0], lin[1], lin[2], 0.18f };
                m_prims.AddRectRgba(x0 + pad * 0.5f, ry - 2.0f * dpi,
                                    boxW - pad, lineH, selc, 0.0f, PrimLayer::OverBlend);
            }
            const char* state = (w.flags & 1u) ? "minimized"
                              : (w.flags & 4u) ? "active"
                              : (w.flags & 2u) ? "maximized" : "open";
            // The title came from the remote machine. It was sanitised and
            // bounded by the X side and bounded again by the parser; it is cut
            // to the column here so it cannot push the state text off the row.
            std::string title = w.title.empty() ? std::string("(untitled)") : w.title;
            if (title.size() > 60)
                title = title.substr(0, 57) + "...";
            m_prims.AddText(tx, ry, title, idx == m_appsSel ? 1.0f : 0.7f, m_sampler);
            const float sw = m_prims.MeasureText(state, m_sampler);
            m_prims.AddText(x0 + boxW - pad - sw, ry, state, 0.45f, m_sampler);
        }
    }

    // The footer is the session's traffic, which is the number that says
    // whether a window that looks frozen is actually receiving anything.
    char foot[192];
    snprintf(foot, sizeof foot,
             "%u client%s  %u window%s  in %.1f MiB  out %.1f MiB  %u presents%s",
             r.clients, r.clients == 1 ? "" : "s", r.windows, r.windows == 1 ? "" : "s",
             static_cast<double>(r.x11In) / 1048576.0,
             static_cast<double>(r.x11Out) / 1048576.0, r.presents,
             r.rejected ? "  refused frames" : "");
    m_prims.AddText(tx, y0 + boxH - pad - lineH, foot, 0.4f, m_sampler);
    m_prims.AddText(tx, y0 + boxH - pad - lineH * 2.0f,
                    "Enter show    M minimize    C close    Esc dismiss", 0.35f, m_sampler);
}

void App::DrawJournal()
{
    if (!m_jrnOpen)
        return;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const float lineH = (m_gm.cellH > 0.0f) ? m_gm.cellH * 1.25f : 24.0f * dpi;
    const float pad = 12.0f * dpi;
    const float W = static_cast<float>(m_device.Width());
    const float boxW = std::min(W - 2.0f * pad, 900.0f * dpi);
    const int maxRows = 14;
    const int n = static_cast<int>(m_jrnHits.size());
    const int rows = std::max(1, std::min(maxRows, n));
    const float boxH = pad * 2.0f + lineH * static_cast<float>(rows + 2) + 8.0f * dpi;
    const float x0 = (W - boxW) * 0.5f;
    const float y0 = m_titleBarH + 24.0f * dpi;
    const float bw = std::max(1.0f, 1.5f * dpi);

    float bg[4] = { 0.02f, 0.015f, 0.005f, 1.0f };   // fully opaque
    m_prims.AddRectRgba(x0, y0, boxW, boxH, bg, 0.0f, PrimLayer::OverBlend);
    SetPanelRect(x0, y0, boxW, boxH);
    float lin[3];
    AmberRampCpu(0.55f, lin);
    float border[4] = { lin[0], lin[1], lin[2], 0.9f };
    m_prims.AddRectRgba(x0, y0, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0 + boxH - bw, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0, bw, boxH, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0 + boxW - bw, y0, bw, boxH, border, 0.0f, PrimLayer::Over);

    const float tx = x0 + pad, ty = y0 + pad;
    bool caret = std::fmod(m_time, 1.0) < 0.6;
    m_prims.AddText(tx, ty, "journal> " + m_jrnQuery + (caret ? "_" : " "), 1.0f,
                    m_sampler);
    if (n > 0)
    {
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "%d of %d", m_jrnSel + 1, n);
        float cw = m_prims.MeasureText(cnt, m_sampler);
        m_prims.AddText(x0 + boxW - pad - cw, ty, cnt, 0.45f, m_sampler);
    }
    const float sepY = ty + lineH + 2.0f * dpi;
    float sep[4] = { lin[0], lin[1], lin[2], 0.45f };
    m_prims.AddRectRgba(x0 + pad, sepY, boxW - 2.0f * pad, bw, sep, 0.0f,
                        PrimLayer::Over);

    if (n == 0)
    {
        m_prims.AddText(tx, sepY + 6.0f * dpi,
                        m_journal.Entries().empty()
                            ? "No commands recorded yet"
                            : "No matching commands",
                        0.45f, m_sampler);
    }

    // "3m", "4h", "2d" — the age is more useful at a glance than a timestamp.
    auto ageText = [](int64_t started) {
        int64_t secs = static_cast<int64_t>(std::time(nullptr)) - started;
        char b[24];
        if (started <= 0)          snprintf(b, sizeof(b), "-");
        else if (secs < 60)        snprintf(b, sizeof(b), "%llds", (long long)secs);
        else if (secs < 3600)      snprintf(b, sizeof(b), "%lldm", (long long)(secs / 60));
        else if (secs < 86400)     snprintf(b, sizeof(b), "%lldh", (long long)(secs / 3600));
        else                       snprintf(b, sizeof(b), "%lldd", (long long)(secs / 86400));
        return std::string(b);
    };
    auto durText = [](double d) {
        char b[24];
        if (d < 1.0)        snprintf(b, sizeof(b), "%dms", static_cast<int>(d * 1000.0));
        else if (d < 60.0)  snprintf(b, sizeof(b), "%.1fs", d);
        else                snprintf(b, sizeof(b), "%dm%02ds", static_cast<int>(d) / 60,
                                     static_cast<int>(d) % 60);
        return std::string(b);
    };

    const int first = std::max(0, std::min(m_jrnSel - rows + 1, n - rows));
    const auto& all = m_journal.Entries();
    for (int i = 0; i < rows && first + i < n; ++i)
    {
        const int idx = first + i;
        const amber::JournalEntry& e = all[m_jrnHits[idx]];
        const float ry = sepY + 6.0f * dpi + static_cast<float>(i) * lineH;
        const bool sel = (idx == m_jrnSel);
        if (sel)
        {
            float hl[4] = { lin[0], lin[1], lin[2], 0.22f };
            m_prims.AddRectRgba(x0 + pad * 0.5f, ry - 2.0f * dpi, boxW - pad, lineH,
                                hl, 0.0f, PrimLayer::OverBlend);
        }
        // Exit status as a coloured pip: green succeeded, red failed. This is
        // the column the eye lands on when hunting "the one that broke".
        float pip[4] = { 0.10f, 0.85f, 0.32f, sel ? 1.0f : 0.75f };
        if (e.interrupted)
        {
            // Amber, not red: the link died mid-command and the outcome was
            // never reported. "Unknown" is a third state, not a failure.
            pip[0] = 1.0f; pip[1] = 0.68f; pip[2] = 0.10f;
        }
        else if (e.exitCode != 0)
        {
            pip[0] = 1.0f; pip[1] = 0.20f; pip[2] = 0.12f;
        }
        m_prims.AddRectRgba(tx, ry + lineH * 0.32f, 5.0f * dpi, 5.0f * dpi, pip,
                            0.0f, PrimLayer::Over);

        // Right-hand meta column, laid out from the right edge inwards.
        const std::string age = ageText(e.startedAt);
        const std::string dur = durText(e.durationSec);
        std::string meta = dur + "  " + age;
        if (e.exitCode != 0)
        {
            char x[24];
            snprintf(x, sizeof(x), "exit %d  ", e.exitCode);
            meta = std::string(x) + meta;
        }
        const float metaW = m_prims.MeasureText(meta, m_sampler);
        const std::string host = e.host.empty() ? std::string() : e.host + "  ";
        const float hostW = m_prims.MeasureText(host, m_sampler);
        const float cmdX = tx + 12.0f * dpi;
        const float avail = boxW - pad * 2.0f - 12.0f * dpi - metaW - hostW - 16.0f * dpi;

        std::string cmd = e.command;
        if (m_prims.MeasureText(cmd, m_sampler) > avail)
        {
            while (!cmd.empty() &&
                   m_prims.MeasureText(cmd + "\xE2\x80\xA6", m_sampler) > avail)
            {
                cmd.pop_back();
                while (!cmd.empty() &&
                       (static_cast<unsigned char>(cmd.back()) & 0xC0) == 0x80)
                    cmd.pop_back();
            }
            cmd += "\xE2\x80\xA6";
        }
        m_prims.AddText(cmdX, ry, cmd, sel ? 1.0f : 0.72f, m_sampler);
        if (!host.empty())
            m_prims.AddText(x0 + boxW - pad - metaW - hostW, ry, host,
                            sel ? 0.62f : 0.40f, m_sampler);
        m_prims.AddText(x0 + boxW - pad - metaW, ry, meta, sel ? 0.62f : 0.40f,
                        m_sampler);
    }

    // Key legend along the foot, so the two Enter behaviours are discoverable.
    const float hy = y0 + boxH - pad - lineH * 0.9f;
    m_prims.AddText(tx, hy,
                    "enter insert   ctrl+enter run   ctrl+j jump   ctrl+c copy   del forget",
                    0.34f, m_sampler);
}

void App::DrawPalette()
{
    if (!m_palOpen)
        return;
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const float lineH = (m_gm.cellH > 0.0f) ? m_gm.cellH * 1.25f : 24.0f * dpi;
    const float pad = 12.0f * dpi;
    const float W = static_cast<float>(m_device.Width());
    const float boxW = std::min(W - 2.0f * pad, 680.0f * dpi);
    const int maxRows = 12;
    const int n = static_cast<int>(m_palHits.size());
    const int rows = std::max(1, std::min(maxRows, n));
    const float boxH = pad * 2.0f + lineH * static_cast<float>(rows + 1) + 8.0f * dpi;
    const float x0 = (W - boxW) * 0.5f;
    const float y0 = m_titleBarH + 24.0f * dpi;
    const float bw = std::max(1.0f, 1.5f * dpi);

    // Backdrop (darkens the field beneath) + theme-tinted border.
    float bg[4] = { 0.02f, 0.015f, 0.005f, 1.0f };   // fully opaque
    m_prims.AddRectRgba(x0, y0, boxW, boxH, bg, 0.0f, PrimLayer::OverBlend);
    SetPanelRect(x0, y0, boxW, boxH);
    float lin[3];
    AmberRampCpu(0.55f, lin);
    float border[4] = { lin[0], lin[1], lin[2], 0.9f };
    m_prims.AddRectRgba(x0, y0, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0 + boxH - bw, boxW, bw, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0, y0, bw, boxH, border, 0.0f, PrimLayer::Over);
    m_prims.AddRectRgba(x0 + boxW - bw, y0, bw, boxH, border, 0.0f, PrimLayer::Over);

    // Query line with a blinking caret.
    const float tx = x0 + pad, ty = y0 + pad;
    bool caret = std::fmod(m_time, 1.0) < 0.6;
    m_prims.AddText(tx, ty, "> " + m_palQuery + (caret ? "_" : " "), 1.0f, m_sampler);
    const float sepY = ty + lineH + 2.0f * dpi;
    float sep[4] = { lin[0], lin[1], lin[2], 0.45f };
    m_prims.AddRectRgba(x0 + pad, sepY, boxW - 2.0f * pad, bw, sep, 0.0f, PrimLayer::Over);

    // Results — keep the selection in view.
    int first = std::max(0, std::min(m_palSel - rows + 1, n - rows));
    if (n == 0)
    {
        m_prims.AddText(tx, sepY + 6.0f * dpi, "No matching commands", 0.45f, m_sampler);
    }
    for (int i = 0; i < rows && first + i < n; ++i)
    {
        int idx = first + i;
        float ry = sepY + 6.0f * dpi + static_cast<float>(i) * lineH;
        if (idx == m_palSel)
        {
            float hl[4] = { lin[0], lin[1], lin[2], 0.22f };
            m_prims.AddRectRgba(x0 + pad * 0.5f, ry - 2.0f * dpi, boxW - pad, lineH,
                                hl, 0.0f, PrimLayer::OverBlend);
        }
        m_prims.AddText(tx, ry, m_palAll[m_palHits[idx]].label,
                        idx == m_palSel ? 1.0f : 0.62f, m_sampler);
    }
    if (n > rows)
    {
        char more[48];
        snprintf(more, sizeof(more), "%d of %d", m_palSel + 1, n);
        float mw = m_prims.MeasureText(more, m_sampler);
        m_prims.AddText(x0 + boxW - pad - mw, ty, more, 0.45f, m_sampler);
    }
}

// ------------------------------------------------------------ live effects
// Reads back the text the user typed at the prompt: from the OSC 133 'B' mark
// (where the prompt ended) to the cursor at 'C' (where the command was
// submitted). Reading it off the grid rather than from keystrokes means it
// works for pasted, completed and history-recalled commands alike, and it
// never sees anything the terminal did not display — a password read by the
// shell is not echoed, so it cannot land here.
std::string App::LiftCommandText(const amber::Session& s) const
{
    if (s.promptCol < 0)
        return {};
    const Grid& g = s.grid;
    // The screen may have scrolled between the prompt and the submission, so
    // the recorded absolute row is rebased onto the buffer as it stands now.
    const int64_t delta = static_cast<int64_t>(s.promptRowId) -
                          static_cast<int64_t>(g.TotalPushed());
    const int total = g.ScrollbackSize() + g.Rows();
    int startAbs = g.ScrollbackSize() + static_cast<int>(delta);
    int curAbs = g.ScrollbackSize() + std::max(0, g.CurY());
    if (startAbs < 0 || startAbs >= total || curAbs < startAbs)
        return {};
    // At 'C' the newline has usually been processed, so the command occupies
    // the rows up to (but excluding) the cursor's. A command that has not
    // wrapped sits entirely on the prompt row.
    const int lastAbs = (curAbs > startAbs) ? curAbs - 1 : startAbs;
    const int kMaxRows = 10;    // a wrapped monster, not a whole screenful
    std::string out;
    for (int row = startAbs; row <= lastAbs && row - startAbs < kMaxRows; ++row)
    {
        const int from = (row == startAbs) ? s.promptCol : 0;
        const int to = (row == lastAbs && curAbs == startAbs) ? g.CurX() : g.Cols();
        std::string line;
        for (int col = std::max(0, from); col < std::min(to, g.Cols()); ++col)
        {
            const Cell& c = g.AbsCell(row, col);
            if (c.flags & CellWideTail)
                continue;
            amber::AppendClusterUtf8(line, (c.cp == 0) ? U' ' : c.cp);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        out += line;
    }
    // Leading whitespace is the user's "do not record" signal; keep it so the
    // journal can honour it, but drop a command that is only whitespace.
    if (out.find_first_not_of(" \t") == std::string::npos)
        return {};
    return out;
}

// ------------------------------------------------------------ block ranges
// An absolute row id addresses AbsCell at a different index: AbsCell counts
// from the oldest retained line, the id counts from the first line ever
// pushed. Returns -1 when the row has been trimmed away.
int App::AbsIndexForRow(const amber::Session& s, uint64_t rowId)
{
    const Grid& g = s.grid;
    const uint64_t base = g.TotalPushed() -
                          static_cast<uint64_t>(g.ScrollbackSize());
    if (rowId < base)
        return -1;
    const int idx = static_cast<int>(rowId - base);
    return idx < g.ScrollbackSize() + g.Rows() ? idx : -1;
}

// One line of the grid, trailing blanks removed. Reads the canonical grid;
// no block ever stores text.
std::string App::RowTextRaw(const amber::Session& s, uint64_t rowId) const
{
    const int idx = AbsIndexForRow(s, rowId);
    if (idx < 0)
        return {};
    const Grid& g = s.grid;
    std::string line;
    for (int c = 0; c < g.Cols(); ++c)
    {
        const Cell& cell = g.AbsCell(idx, c);
        if (cell.flags & CellWideTail)
            continue;
        amber::AppendClusterUtf8(line, cell.cp == 0 ? U' ' : cell.cp);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    return line;
}

// The output of a block, as text, straight out of the grid. Rows that have
// been trimmed are simply absent — never guessed at.
std::string App::BlockOutputText(const amber::Session& s, const amber::CommandBlock& b) const
{
    if (!b.hasOutput)
        return {};
    std::string out;
    for (uint64_t r = b.outputFirst; r <= b.outputLast; ++r)
    {
        if (AbsIndexForRow(s, r) < 0)
            continue;
        out += RowTextRaw(s, r);
        out += "\n";
    }
    return out;
}

std::string App::FirstOutputLine(const amber::Session& s, const amber::CommandBlock& b) const
{
    if (!b.hasOutput)
        return {};
    // Bounded: a block whose first hundred rows are blank is not worth
    // walking further for a summary line.
    const uint64_t stop = std::min(b.outputLast, b.outputFirst + 99);
    for (uint64_t r = b.outputFirst; r <= stop; ++r)
    {
        std::string line = RowTextRaw(s, r);
        const size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos)
            continue;
        return line.substr(a);
    }
    return {};
}

// The block currently being assembled: the last one, while it has not yet
// been finished by a 'D'. Returns nullptr when there is none.
amber::CommandBlock* App::OpenBlock(amber::Session& s)
{
    if (s.blocks.empty())
        return nullptr;
    amber::CommandBlock& b = s.blocks.back();
    return b.hasExit || b.interrupted ? nullptr : &b;
}

// Starts a block at `row`. Any block still open is closed first, truthfully:
// a new prompt means the previous command ended, but the shell never said
// how, so it gets no exit status rather than a made-up one.
amber::CommandBlock& App::BeginBlock(amber::Session& s, uint64_t row)
{
    if (amber::CommandBlock* prev = OpenBlock(s))
    {
        prev->running = false;
        prev->hasExit = false;
        if (prev->outputFirst > 0 && row > prev->outputFirst)
        {
            prev->outputLast = row - 1;
            prev->hasOutput = true;
            prev->lines = static_cast<int>(prev->outputLast - prev->outputFirst + 1);
        }
        FinishBlockSummary(s, *prev);
    }
    amber::CommandBlock b;
    b.id = s.nextBlockId++;
    b.sessionKey = s.profile.id.empty() ? s.label : s.profile.id;
    b.promptRow = row;
    b.inputRow = row;
    s.blocks.push_back(std::move(b));
    // Blocks are bounded by the scrollback that backs them, and by a ceiling
    // so a session that runs for days cannot grow the list without limit.
    const uint64_t oldest = s.grid.TotalPushed() -
                            static_cast<uint64_t>(s.grid.ScrollbackSize());
    amber::TrimBlocks(s.blocks, oldest);
    if (s.blocks.size() > kMaxBlocks)
        s.blocks.erase(s.blocks.begin(),
                       s.blocks.begin() + (s.blocks.size() - kMaxBlocks));
    return s.blocks.back();
}

void App::FinishBlockSummary(amber::Session& s, amber::CommandBlock& b)
{
    // The first non-blank line of the output is usually the useful part of a
    // long log, so a collapsed block can show it. Read from the grid, never
    // stored: the block holds no output text of its own.
    std::string first;
    if (b.hasOutput && m_foldFirstLine)
        first = FirstOutputLine(s, b);
    b.summary = amber::BuildSummary(b, m_foldShowCwd, first);
}

// A command finished. Two separate reasons to speak up: the user explicitly
// asked to be told about THIS one, or it ran long enough to cross the
// profile's threshold. Neither ever fires for a fast command by accident.
void App::NotifyBlockFinished(amber::Session& s, const amber::CommandBlock& b)
{
    const bool asked = s.notifyRunning && s.runningBlockId == b.id;
    s.notifyRunning = false;

    // -1 on either profile field means "follow the global setting".
    const amber::NotifyOn mode =
        s.profile.notifyCommands < 0
            ? static_cast<amber::NotifyOn>(std::clamp(m_notifyCommands, 0, 3))
            : static_cast<amber::NotifyOn>(std::clamp(s.profile.notifyCommands, 0, 3));
    const int threshold = s.profile.notifyAfterSeconds < 0 ? m_notifyAfterSecs
                                                           : s.profile.notifyAfterSeconds;

    const bool byPolicy = amber::ShouldNotifyCompletion(mode, threshold, b);
    if (!asked && !byPolicy)
        return;

    const std::string text = amber::CompletionText(b);
    // In the foreground the status line is enough and a toast would be rude;
    // a toast is for work you walked away from.
    if (!m_focused || m_minimized)
    {
        m_tray.Toast(L"AmberSSH — " + WideFromUtf8(s.Caption()), WideFromUtf8(text));
        s.unread = true;
    }
    else if (HasSession() && &s == &Cur())
    {
        SetStatus(text, 8.0);
    }
}

void App::OnShellMark(amber::Session& s, char kind, int code, bool hasCode)
{
    // Anything that is not a mark AmberSSH understands is ignored outright.
    // A shell (or a hostile server) can emit "OSC 133;X"; it must not create
    // a block, move a boundary, or disturb the ones that exist.
    if (kind != 'A' && kind != 'B' && kind != 'C' && kind != 'D')
        return;

    // Command-boundary bookkeeping for the tide marks / running pulse.
    uint64_t rowId = s.grid.TotalPushed() +
                     static_cast<uint64_t>(std::max(0, s.grid.CurY()));
    if (kind == 'A' || kind == 'C')
    {
        s.marks.push_back({ rowId, kind });
        if (s.marks.size() > 400)
            s.marks.erase(s.marks.begin(), s.marks.begin() + 100);
    }
    if (kind == 'A')
        BeginBlock(s, rowId);

    if (kind == 'B')
    {
        // End of the prompt: everything typed from here to the cursor at 'C'
        // is the command, so remember exactly where the user's text starts.
        s.promptRowId = rowId;
        s.promptCol = s.grid.CurX();
        // A shell that emits B without A still gets a block.
        amber::CommandBlock* b = OpenBlock(s);
        if (!b)
            b = &BeginBlock(s, rowId);
        b->inputRow = rowId;
        b->inputCol = s.grid.CurX();
    }
    if (kind == 'C')
    {
        s.cmdRunning = true;
        s.cmdStart = m_time;
        // The command text is lifted whether or not the journal is capturing:
        // the block needs it for Copy Command and Rerun, and it costs one
        // walk of at most ten rows.
        s.pendingCmd = LiftCommandText(s);
        s.pendingCwd = s.cwd;
        s.pendingStartedAt = static_cast<int64_t>(std::time(nullptr));
        s.promptCol = -1;
        s.outputStartRow = rowId;
        s.runningBytes = 0;

        amber::CommandBlock* b = OpenBlock(s);
        if (!b)
            b = &BeginBlock(s, rowId);     // C with no A and no B
        b->command = s.pendingCmd;
        b->cwd = s.pendingCwd;
        b->startedAt = s.pendingStartedAt;
        b->outputFirst = rowId;
        b->running = true;
        s.runningBlockId = b->id;
    }
    if (kind == 'D')
    {
        s.cmdRunning = false;
        // A failed command leaves the taskbar button red for a few seconds,
        // so a failure that happened while you were in another window is
        // still visible when you come back. An absent status is not a
        // failure, so it does not light it.
        if (hasCode && code != 0 && HasSession() && &Cur() == &s)
            m_tbErrorUntil = m_time + 6.0;

        // Finish the open block. A 'D' with no matching 'C' has nothing to
        // close, and inventing a block for it would put a command with no
        // rows into the list.
        amber::CommandBlock* b = OpenBlock(s);
        if (b && b->running)
        {
            b->running = false;
            b->hasExit = hasCode;
            b->exitCode = hasCode ? code : 0;
            b->endedAt = static_cast<int64_t>(std::time(nullptr));
            b->durationSec = std::max(0.0, m_time - s.cmdStart);
            b->bytes = s.runningBytes;
            // Output rows: everything between the C mark and here. A command
            // that printed nothing has none, and folding it would hide a row
            // that belongs to the next prompt.
            if (s.outputStartRow > 0 && rowId > s.outputStartRow &&
                !s.grid.AltActive())
            {
                b->outputFirst = s.outputStartRow;
                b->outputLast = rowId - 1;
                b->hasOutput = true;
                b->lines = static_cast<int>(b->outputLast - b->outputFirst + 1);
            }
            else
            {
                b->hasOutput = false;
                b->lines = 0;
            }
            FinishBlockSummary(s, *b);
            NotifyBlockFinished(s, *b);
        }
        s.runningBlockId = 0;
        s.runningBytes = 0;
        s.outputStartRow = 0;
        if (!s.pendingCmd.empty() && m_journalOn)
        {
            amber::JournalEntry e;
            e.host = s.Caption();
            e.cwd = s.pendingCwd;
            // Masked for storage: the journal is persisted, so a secret on a
            // command line must not survive on disk regardless of whether the
            // screen cloak happened to be on when it was typed.
            e.command = MaskForStorage(s.pendingCmd);
            e.cwd = MaskForStorage(e.cwd);
            // An absent status is recorded as unknown, not as success.
            e.exitCode = hasCode ? code : -1;
            e.interrupted = !hasCode;
            e.durationSec = std::max(0.0, m_time - s.cmdStart);
            e.startedAt = s.pendingStartedAt;
            e.blockId = b ? b->id : 0;
            e.sessionKey = s.profile.id.empty() ? s.label : s.profile.id;
            m_journal.Add(std::move(e));
        }
        s.pendingCmd.clear();
    }
    // Exit-code flash: quiet green on success, red wash + ring on failure —
    // active tab only. Without a reported status there is nothing to flash.
    if (kind != 'D' || !hasCode || !HasSession() || &Cur() != &s || m_minimized)
        return;
    m_exitFlashStart = m_time;
    if (code == 0)
    {
        m_exitFlashCol[0] = 0.10f;
        m_exitFlashCol[1] = 0.90f;
        m_exitFlashCol[2] = 0.30f;
        m_exitFlashAmt = 0.30f;
    }
    else
    {
        m_exitFlashCol[0] = 1.00f;
        m_exitFlashCol[1] = 0.12f;
        m_exitFlashCol[2] = 0.05f;
        // Reduced Motion keeps the colour — a failure must still be visible —
        // but drops it to a tint and skips the shockwave entirely. The
        // information stays; the movement goes.
        if (m_particles.tun.reducedMotion)
        {
            m_exitFlashAmt = 0.25f;
        }
        else
        {
            m_exitFlashAmt = 0.85f;
            TriggerShockwave();
        }
    }
}

void App::UpdateWeather()
{
    // Link weather from the kernel's TCP stats: RTT jitter → turbulence in
    // the particle wind, a retransmit burst → a storm gust. Decays on its own.
    if (!m_fxLive || !HasSession() || Cur().diagnostic)
    {
        m_weatherTurb = 0.0f;
        return;
    }
    if (m_time - m_lastWeatherSample >= 1.0)
    {
        m_lastWeatherSample = m_time;
        uint32_t rtt = Foc().ssh.RttUs();
        uint32_t retr = Foc().ssh.RetransBytes();
        if (m_prevRtt && rtt)
        {
            float jit = std::fabs(static_cast<float>(rtt) - static_cast<float>(m_prevRtt)) /
                        static_cast<float>(std::max(rtt, 1000u));
            m_weatherTurb = std::max(m_weatherTurb, std::min(0.6f, jit * 2.0f));
        }
        m_prevRtt = rtt;
        if (m_lastRetrans && retr > m_lastRetrans)
            m_weatherTurb = 0.9f;   // packets being lost right now: storm
        m_lastRetrans = retr;
    }
    m_weatherTurb *= std::exp(-m_dt * 0.8f);
    if (m_weatherTurb < 0.01f)
        m_weatherTurb = 0.0f;
}

void App::SelectBlockAt(int row, int col)
{
    (void)col;
    amber::Session& F = Foc();
    if (F.marks.empty())
    {
        SetStatus("No command marks yet — needs OSC 133 shell integration.", 6.0);
        return;
    }
    uint64_t pushed = F.grid.TotalPushed();
    int64_t top = static_cast<int64_t>(pushed) - F.grid.ViewOffset();
    int64_t clicked = top + row;
    int64_t start = -1, end = -1;
    for (size_t i = 0; i < F.marks.size(); ++i)
    {
        if (F.marks[i].kind != 'C' || static_cast<int64_t>(F.marks[i].rowId) > clicked)
            continue;
        start = static_cast<int64_t>(F.marks[i].rowId);
        end = -1;
        for (size_t j = i + 1; j < F.marks.size(); ++j)
            if (F.marks[j].kind == 'A')
            {
                end = static_cast<int64_t>(F.marks[j].rowId) - 1;
                break;
            }
    }
    if (start < 0)
        return;
    if (end < start)
        end = static_cast<int64_t>(pushed) + F.grid.CurY() - 1;   // still running
    int rows = F.grid.Rows();
    int vr0 = static_cast<int>(std::clamp<int64_t>(start - top, 0, rows - 1));
    int vr1 = static_cast<int>(std::clamp<int64_t>(end - top, 0, rows - 1));
    F.selecting = false;
    F.selActive = true;
    F.selStartR = vr0; F.selStartC = 0;
    F.selEndR = vr1;   F.selEndC = std::max(0, F.grid.Cols() - 1);
    F.selAnimStart = m_lastFrameTime;
    CopySelection();
    SetStatus("Command output selected and copied (" + std::to_string(vr1 - vr0 + 1) +
              " lines)", 5.0);
}

void App::AddEmber(amber::Session& s, bool severe, int rowsBelow)
{
    if (!m_fxLive)
        return;
    // The matched line ended with '\n', so it sits one row above the cursor,
    // minus any lines that completed after it in the same chunk.
    int64_t row = static_cast<int64_t>(s.grid.TotalPushed()) + s.grid.CurY() - 1 - rowsBelow;
    if (row < 0)
        row = 0;
    s.embers.push_back({ static_cast<uint64_t>(row), m_time, severe });
    if (s.embers.size() > 64)
        s.embers.erase(s.embers.begin());
    if (severe && HasSession() && &s == &Cur())
        m_shakeStart = m_time;
}

void App::DrawLiveEffects()
{
    if (!m_fxLive || !HasSession() || m_minimized)
        return;
    amber::Session& F = Foc();
    int co = 0, ro = 0;
    PaneOffset(F, co, ro);
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const int rows = F.grid.Rows(), cols = F.grid.Cols();
    const float paneX = m_gm.originX + static_cast<float>(co) * m_gm.cellW;
    const float paneW = static_cast<float>(cols) * m_gm.cellW;
    uint64_t pushed = F.grid.TotalPushed();
    int64_t top = static_cast<int64_t>(pushed) - F.grid.ViewOffset();
    float lin[3];
    AmberRampCpu(0.9f, lin);

    // 1. Tide marks: a hairline where each command's output began; the most
    //    recent block breathes so the "current" output is obvious.
    uint64_t lastC = 0;
    for (const auto& m : F.marks)
        if (m.kind == 'C')
            lastC = m.rowId;
    for (const auto& m : F.marks)
    {
        if (m.kind != 'C')
            continue;
        int64_t vr = static_cast<int64_t>(m.rowId) - top;
        if (vr < 0 || vr >= rows)
            continue;
        bool latest = (m.rowId == lastC);
        float a = latest ? 0.50f + 0.25f * std::sin(m_time * 2.2f) : 0.22f;
        float rgba[4] = { lin[0], lin[1], lin[2], a };
        float y = m_gm.originY + static_cast<float>(vr + ro) * m_gm.cellH - 1.0f * dpi;
        float hh = std::max(1.0f, 1.2f * dpi);
        m_prims.AddRectRgba(paneX, y, paneW, hh, rgba, 0.0f, PrimLayer::Over);
        m_prims.AddRectRgba(paneX - 5.0f * dpi, y - 2.0f * dpi, 4.0f * dpi, hh + 4.0f * dpi,
                            rgba, 0.0f, PrimLayer::Over);
    }

    int cr = 0, cc = 0;
    bool curVis = false;
    F.grid.CursorViewPos(cr, cc, curVis);
    const float cx = m_gm.originX + (static_cast<float>(cc + co) + 0.5f) * m_gm.cellW;
    const float cy = m_gm.originY + (static_cast<float>(cr + ro) + 0.5f) * m_gm.cellH;

    // 2. Running-command pulse: a radar sweep along the cursor row whose
    //    period stretches with how long the command has been running.
    if (F.cmdRunning && curVis)
    {
        float el = static_cast<float>(m_time - F.cmdStart);
        float period = 1.4f * (1.0f + std::log2(1.0f + el / 10.0f));
        float ph = std::fmod(static_cast<float>(m_time), period) / period;
        float bandW = 8.0f * m_gm.cellW;
        float x0 = paneX + ph * (paneW + bandW) - bandW;
        float x1 = std::min(x0 + bandW, paneX + paneW);
        x0 = std::max(x0, paneX);
        if (x1 > x0)
        {
            float t0 = (x0 - (paneX + ph * (paneW + bandW) - bandW)) / bandW;
            float c0[4] = { lin[0], lin[1], lin[2], 0.45f * t0 };
            float c1[4] = { lin[0], lin[1], lin[2], 0.45f };
            float y = m_gm.originY + static_cast<float>(cr + ro) * m_gm.cellH;
            m_prims.AddRectGradient(x0, y, x1 - x0, m_gm.cellH, c0, c1, false);
            // Bright leading edge so the sweep reads even at a glance.
            float edge[4] = { lin[0], lin[1], lin[2], 0.9f };
            m_prims.AddRectRgba(x1 - 2.0f * dpi, y, 2.0f * dpi, m_gm.cellH, edge, 0.0f,
                                PrimLayer::Over);
        }
    }

    // 3. Typing echo ring: opens on the keystroke, holds (pulsing) until the
    //    server's echo arrives, then snaps shut — the RTT made visible.
    bool closing = F.echoClosedAt >= 0.0 && (m_time - F.echoClosedAt) < 0.16;
    if (curVis && (F.echoPending || closing))
    {
        float R = m_gm.cellH * 0.9f, rad, a;
        if (F.echoPending)
        {
            float t = static_cast<float>(m_time - F.echoSentAt);
            rad = R * std::min(1.0f, t / 0.10f) * (1.0f + 0.08f * std::sin(static_cast<float>(m_time) * 9.0f));
            a = 0.85f;
        }
        else
        {
            float t = static_cast<float>((m_time - F.echoClosedAt) / 0.16);
            rad = R * (1.0f - t);
            a = 0.85f * (1.0f - t);
        }
        float rgba[4] = { lin[0], lin[1], lin[2], a };
        float d = std::max(1.5f, 1.6f * dpi);
        for (int k = 0; k < 28; ++k)
        {
            float ang = static_cast<float>(k) * 6.2831853f / 28.0f;
            m_prims.AddRectRgba(cx + std::cos(ang) * rad - d * 0.5f,
                                cy + std::sin(ang) * rad - d * 0.5f, d, d, rgba, 0.0f,
                                PrimLayer::Over);
        }
    }

    // 5. Error-line embers: matched lines glow red beneath the text and cool
    //    over several seconds, with a flickering ember line along the bottom.
    while (!F.embers.empty() && m_time - F.embers.front().t > 8.0)
        F.embers.erase(F.embers.begin());
    for (const auto& e : F.embers)
    {
        int64_t vr = static_cast<int64_t>(e.rowId) - top;
        if (vr < 0 || vr >= rows)
            continue;
        float age = static_cast<float>(m_time - e.t);
        float decay = std::exp(-age / (e.severe ? 5.0f : 3.5f));
        float y = m_gm.originY + static_cast<float>(vr + ro) * m_gm.cellH;
        float wash[4] = { 0.90f, 0.10f, 0.04f, (e.severe ? 0.42f : 0.28f) * decay };
        m_prims.AddRectRgba(paneX, y, paneW, m_gm.cellH, wash, 0.0f, PrimLayer::Under);
        float flick = 0.55f + 0.45f * std::sin(static_cast<float>(m_time) * 13.0f +
                                               static_cast<float>(e.rowId));
        float ember[4] = { 1.0f, 0.35f, 0.08f, 0.8f * decay * flick };
        m_prims.AddRectRgba(paneX, y + m_gm.cellH - 2.0f * dpi, paneW, 1.5f * dpi, ember,
                            0.0f, PrimLayer::Over);
    }

    // 6. Scroll-position phosphor bar: a column at the pane's right edge
    //    while scrolled back (and briefly after), thumb = the visible window.
    {
        int vo = F.grid.ViewOffset();
        int sb = F.grid.ScrollbackSize();
        if (vo != m_lastViewOffset)
        {
            m_lastViewOffset = vo;
            m_scrollChangedAt = m_time;
        }
        float target = (vo > 0 || (m_time - m_scrollChangedAt) < 1.5) ? 1.0f : 0.0f;
        m_scrollBarAlpha += (target - m_scrollBarAlpha) * std::min(1.0f, m_dt * 6.0f);
        if (m_scrollBarAlpha > 0.02f && sb > 0)
        {
            float A = m_scrollBarAlpha;
            float bw = std::max(2.0f, 3.0f * dpi);
            float bx = paneX + paneW - bw;
            float ty = m_gm.originY + static_cast<float>(ro) * m_gm.cellH;
            float th = static_cast<float>(rows) * m_gm.cellH;
            float total = static_cast<float>(sb + rows);
            float visFrac = static_cast<float>(rows) / total;
            float topFrac = static_cast<float>(sb - vo) / total;
            float track[4] = { lin[0], lin[1], lin[2], 0.18f * A };
            m_prims.AddRectRgba(bx, ty, bw, th, track, 0.0f, PrimLayer::Over);
            float thumbH = std::max(6.0f * dpi, th * visFrac);
            float thumbY = ty + std::min(th - thumbH, th * topFrac);
            float thumb[4] = { lin[0], lin[1], lin[2], 0.9f * A };
            m_prims.AddRectRgba(bx, thumbY, bw, thumbH, thumb, 0.0f, PrimLayer::Over);
            // History depth: a faint tick every 500 lines of scrollback.
            for (int l = 500; l < sb; l += 500)
            {
                float fy = ty + th * (static_cast<float>(l) / total);
                float tick[4] = { lin[0], lin[1], lin[2], 0.45f * A };
                m_prims.AddRectRgba(bx - 2.0f * dpi, fy, bw + 2.0f * dpi, 1.0f * dpi, tick,
                                    0.0f, PrimLayer::Over);
            }
        }
    }

    // 4. Momentum cursor: a comet tail from where the cursor came from,
    //    fading over 0.4 s, only for jumps of more than a cell.
    if (curVis)
    {
        if (cx != m_lastCurPx || cy != m_lastCurPy)
        {
            if (m_lastCurPx >= 0.0f &&
                (std::fabs(cx - m_lastCurPx) > m_gm.cellW * 1.5f ||
                 std::fabs(cy - m_lastCurPy) > m_gm.cellH * 0.5f))
                m_cursorTrail.push_back({ m_lastCurPx, m_lastCurPy, m_time });
            m_lastCurPx = cx;
            m_lastCurPy = cy;
        }
    }
    while (!m_cursorTrail.empty() && m_time - m_cursorTrail.front().t > 0.4)
        m_cursorTrail.erase(m_cursorTrail.begin());
    for (size_t i = 0; i < m_cursorTrail.size(); ++i)
    {
        const TrailPt& p = m_cursorTrail[i];
        float nx = (i + 1 < m_cursorTrail.size()) ? m_cursorTrail[i + 1].x : cx;
        float ny = (i + 1 < m_cursorTrail.size()) ? m_cursorTrail[i + 1].y : cy;
        float age = static_cast<float>((m_time - p.t) / 0.4);
        float fade = 1.0f - age;
        const int steps = 12;
        for (int k = 0; k <= steps; ++k)
        {
            float t = static_cast<float>(k) / steps;
            float sz = (1.0f + 2.5f * t * fade) * dpi;
            float rgba[4] = { lin[0], lin[1], lin[2], 0.75f * fade * t };
            m_prims.AddRectRgba(p.x + (nx - p.x) * t - sz * 0.5f,
                                p.y + (ny - p.y) * t - sz * 0.5f, sz, sz, rgba, 0.0f,
                                PrimLayer::Over);
        }
    }
}

// ------------------------------------------------------------ inline images
int App::OnInlineImage(amber::Session& s, amber::DecodedImage&& img, int cols, int rows)
{
    if (img.w <= 0 || img.h <= 0)
        return 0;
    float cellW = std::max(m_gm.cellW, 1.0f), cellH = std::max(m_gm.cellH, 1.0f);
    amber::Session::InlineImage im;
    im.id = s.nextImageId++;
    im.rowId = s.grid.TotalPushed() + static_cast<uint64_t>(std::max(0, s.grid.CurY()));
    im.col = s.grid.CurX();
    // Requested cell span, else the pixel size in cells (keeping aspect).
    if (cols > 0 && rows > 0) { im.cols = cols; im.rows = rows; }
    else if (cols > 0) { im.cols = cols; im.rows = std::max(1, static_cast<int>(std::ceil(cols * cellW * img.h / (img.w * cellH)))); }
    else if (rows > 0) { im.rows = rows; im.cols = std::max(1, static_cast<int>(std::ceil(rows * cellH * img.w / (img.h * cellW)))); }
    else
    {
        im.cols = std::max(1, static_cast<int>(std::ceil(img.w / cellW)));
        im.rows = std::max(1, static_cast<int>(std::ceil(img.h / cellH)));
    }
    // Never wider than the screen: scale to fit.
    int maxCols = std::max(1, s.grid.Cols() - im.col);
    if (im.cols > maxCols)
    {
        im.rows = std::max(1, im.rows * maxCols / im.cols);
        im.cols = maxCols;
    }
    im.img = std::make_shared<amber::DecodedImage>(std::move(img));
    // The image sits on the cursor row; the shell scrolls the rows below it
    // in normally. Rows the image needs beyond the screen bottom scroll now.
    s.images.push_back(std::move(im));
    EvictImages(s);
    return s.images.back().rows;
}

// Bytes of decoded pixels a session's inline images are holding.
size_t App::ImageBytes(const amber::Session& s)
{
    size_t total = 0;
    for (const amber::Session::InlineImage& im : s.images)
        if (im.img)
            total += im.img->rgba.size();
    return total;
}

// Keeps a session's inline images inside three bounds: a count, a byte
// budget, and the scrollback itself.
//
// The count alone was the only limit before, and it is not a limit: sixty-four
// images of 8 megapixels each is two gigabytes of RGBA, and a remote program
// can produce them as fast as the link allows. The budget is what actually
// stops that. Oldest first, because the newest image is the one on screen.
void App::EvictImages(amber::Session& s)
{
    // Rows that have scrolled out of the buffer take their images with them.
    const uint64_t oldest = s.grid.TotalPushed() -
                            static_cast<uint64_t>(s.grid.ScrollbackSize());
    s.images.erase(std::remove_if(s.images.begin(), s.images.end(),
                                  [oldest](const amber::Session::InlineImage& im)
                                  {
                                      return im.rowId + static_cast<uint64_t>(
                                                            std::max(0, im.rows)) < oldest;
                                  }),
                   s.images.end());
    while (s.images.size() > kMaxImagesPerSession)
        s.images.erase(s.images.begin());
    size_t bytes = ImageBytes(s);
    while (bytes > kImageByteBudget && s.images.size() > 1)
    {
        if (s.images.front().img)
            bytes -= std::min(bytes, s.images.front().img->rgba.size());
        s.images.erase(s.images.begin());
    }
}

App::GpuImage* App::EnsureGpuImage(ID3D12GraphicsCommandList* cl, amber::Session& s,
                                   amber::Session::InlineImage& im)
{
    uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&s) & 0xFFFFFFFFu) << 32) ^ im.id;
    for (GpuImage& g : m_gpuImages)
        if (g.key == key)
        {
            g.lastUsed = m_time;
            return &g;
        }
    // SRV pool: a fixed set of descriptor slots recycled LRU.
    if (m_imageSrvPool.empty())
        for (int i = 0; i < 24; ++i)
            m_imageSrvPool.push_back(m_device.AllocSrv());
    uint32_t slot;
    if (m_gpuImages.size() < m_imageSrvPool.size())
        slot = m_imageSrvPool[m_gpuImages.size()];
    else
    {
        // Evict the least recently used entry (its texture may still be
        // referenced by an in-flight frame: wait for the GPU first).
        size_t victim = 0;
        for (size_t i = 1; i < m_gpuImages.size(); ++i)
            if (m_gpuImages[i].lastUsed < m_gpuImages[victim].lastUsed)
                victim = i;
        m_device.WaitIdle();
        slot = m_gpuImages[victim].srv;
        m_gpuImages.erase(m_gpuImages.begin() + static_cast<ptrdiff_t>(victim));
    }
    ID3D12Device* d = m_device.Dev();
    GpuImage g;
    g.key = key;
    g.srv = slot;
    g.w = im.img->w;
    g.h = im.img->h;
    g.lastUsed = m_time;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = static_cast<UINT64>(g.w);
    rd.Height = static_cast<UINT>(g.h);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&g.tex))))
        return nullptr;
    g.tex->SetName(L"InlineImage");

    // Staging: premultiply into a 256-aligned row pitch.
    const uint32_t pitch = static_cast<uint32_t>(AlignUp(static_cast<uint32_t>(g.w) * 4,
                                                         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(pitch) * g.h;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&g.staging))))
        return nullptr;
    uint8_t* dst = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    g.staging->Map(0, &noRead, reinterpret_cast<void**>(&dst));
    const std::vector<uint8_t>& src = im.img->rgba;
    for (int y = 0; y < g.h; ++y)
    {
        uint8_t* row = dst + static_cast<size_t>(y) * pitch;
        const uint8_t* sr = &src[static_cast<size_t>(y) * g.w * 4];
        for (int x = 0; x < g.w; ++x)
        {
            uint32_t a = sr[x * 4 + 3];
            row[x * 4 + 0] = static_cast<uint8_t>(sr[x * 4 + 0] * a / 255);
            row[x * 4 + 1] = static_cast<uint8_t>(sr[x * 4 + 1] * a / 255);
            row[x * 4 + 2] = static_cast<uint8_t>(sr[x * 4 + 2] * a / 255);
            row[x * 4 + 3] = static_cast<uint8_t>(a);
        }
    }
    g.staging->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dl = {};
    dl.pResource = g.tex.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION sl = {};
    sl.pResource = g.staging.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width = static_cast<UINT>(g.w);
    sl.PlacedFootprint.Footprint.Height = static_cast<UINT>(g.h);
    sl.PlacedFootprint.Footprint.Depth = 1;
    sl.PlacedFootprint.Footprint.RowPitch = pitch;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.tex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(g.tex.Get(), &sv, m_device.SrvCpu(slot));
    g.uploaded = true;
    m_gpuImages.push_back(std::move(g));
    return &m_gpuImages.back();
}

void App::DrawInlineImages(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                           D3D12_GPU_VIRTUAL_ADDRESS cb)
{
    if (!HasSession())
        return;
    auto drawFor = [&](amber::Session& s, int colOff, int rowOff, int paneRows) {
        if (s.images.empty())
            return;
        // A grid reset (counter went backwards) orphans every placement.
        uint64_t pushed = s.grid.TotalPushed();
        if (pushed < s.imagesPushedSeen)
            s.images.clear();
        s.imagesPushedSeen = pushed;
        // View row 0 shows row id (TotalPushed - viewOffset).
        int64_t top = static_cast<int64_t>(pushed) - s.grid.ViewOffset();
        for (amber::Session::InlineImage& im : s.images)
        {
            int64_t viewRow = static_cast<int64_t>(im.rowId) - top;
            if (viewRow + im.rows <= 0 || viewRow >= paneRows)
                continue;
            GpuImage* g = EnsureGpuImage(cl, s, im);
            if (!g)
                continue;
            float x = m_gm.originX + static_cast<float>(im.col + colOff) * m_gm.cellW;
            float y = m_gm.originY + static_cast<float>(viewRow + rowOff) * m_gm.cellH;
            float w = static_cast<float>(im.cols) * m_gm.cellW;
            float h = static_cast<float>(im.rows) * m_gm.cellH;
            m_prims.DrawImage(cl, frame, cb, g->srv, x, y, w, h, 1.0f - m_rainAmt);
        }
    };
    amber::Session& P = Cur();
    if (P.layout.Empty() || P.layout.Count() <= 1)
    {
        drawFor(P, 0, 0, std::min(P.grid.Rows(), static_cast<int>(m_gm.rows)));
        return;
    }
    const int gc = static_cast<int>(m_gm.cols), gr = static_cast<int>(m_gm.rows);
    for (const auto& [id, r] : P.layout.Rects(gc, gr))
    {
        if (r.cols <= 0 || r.rows <= 0)
            continue;
        if (amber::Session* p = PaneById(P, id))
            drawFor(*p, r.col, r.row, std::min(p->grid.Rows(), r.rows));
    }
}

// ------------------------------------------------------------ vitals / forwards / quake
void App::SyncVitals()
{
    if (!m_vitalsOn || !HasSession() || Cur().diagnostic ||
        Cur().state != amber::SessionState::Connected)
    {
        if (m_vitals.Running())
            m_vitals.Stop();
        return;
    }
    if (m_vitals.Running() && m_vitals.ProfileId() == Cur().profile.id)
        return;
    m_vitals.Start(Cur().profile, Cur().savedPassword.Reveal(),
                   Cur().savedPassphrase.Reveal());
}

void App::DrawVitals(float rightEdgeX)
{
    if (!m_vitalsOn || !HasSession())
        return;
    std::vector<amber::VitalsMonitor::Sample> hist = m_vitals.History();
    const float dpi = static_cast<float>(m_dpi) / 96.0f;
    const float graphW = 64.0f * dpi, graphH = m_titleBarH * 0.55f, gap = 10.0f * dpi;
    const float baseY = (m_titleBarH - graphH) * 0.5f;
    float x = rightEdgeX - 3.0f * (graphW + gap);
    float lin[3];
    AmberRampCpu(0.8f, lin);
    float dimC[4] = { lin[0] * 0.25f, lin[1] * 0.25f, lin[2] * 0.25f, 0.6f };
    struct Series { const char* label; float scale; };
    static const Series kSeries[3] = { { "cpu", 1.0f }, { "mem", 1.0f }, { "net", 0.0f } };
    float netMax = 1.0f;
    for (const auto& s : hist)
        netMax = std::max(netMax, s.netKBs);
    for (int k = 0; k < 3; ++k)
    {
        // Frame
        m_prims.AddRectRgba(x, baseY, graphW, graphH, dimC, 0.0f, PrimLayer::Over);
        float bw = std::max(1.0f, graphW / 60.0f);
        for (size_t i = 0; i < hist.size(); ++i)
        {
            float v = (k == 0) ? hist[i].cpu : (k == 1) ? hist[i].mem : hist[i].netKBs / netMax;
            v = std::clamp(v, 0.02f, 1.0f);
            float bh = v * graphH;
            float hot = (k == 0 && v > 0.8f) ? 1.0f : 0.55f + 0.45f * v;
            float c[3];
            AmberRampCpu(hot, c);
            float rgba[4] = { c[0], c[1], c[2], 0.9f };
            m_prims.AddRectRgba(x + static_cast<float>(i) * bw, baseY + graphH - bh, bw, bh,
                                rgba, 0.0f, PrimLayer::Over);
        }
        m_prims.AddText(x + 2.0f * dpi, baseY - 1.0f * dpi, kSeries[k].label, 0.45f, m_sampler);
        x += graphW + gap;
    }
}

void App::EditForwards()
{
    if (!HasSession() || Cur().diagnostic)
    {
        SetStatus("Open a session first — forwarding rules belong to its profile.");
        return;
    }
    amber::ConnectionProfile p = Cur().profile;
    if (!amber::ForwardsDialog::Show(m_hwnd, p))
        return;
    Cur().profile = p;
    if (!p.id.empty())
    {
        m_profiles.Upsert(p);
        std::string err;
        m_profiles.Save(&err);
    }
    SetStatus("Forwarding rules saved — they apply on the next connect / reconnect.", 8.0);
}

void App::ApplyQuakeHotkey()
{
    UnregisterHotKey(m_hwnd, 1);
    if (m_quake)
    {
        if (!RegisterHotKey(m_hwnd, 1, MOD_CONTROL | MOD_NOREPEAT, VK_OEM_3))
            SetStatus("Could not register Ctrl+` (another app owns it).", 6.0);
    }
}

void App::QuakeToggle()
{
    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    int W = wa.right - wa.left, H = (wa.bottom - wa.top) * 45 / 100;
    bool visible = IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd) &&
                   GetForegroundWindow() == m_hwnd;
    if (visible)
    {
        // Slide up and hide.
        for (int i = 1; i <= 6; ++i)
        {
            SetWindowPos(m_hwnd, nullptr, wa.left, wa.top - H * i / 6, W, H,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Sleep(12);
        }
        ShowWindow(m_hwnd, SW_HIDE);
        m_quakeHidden = true;
        return;
    }
    if (!m_quakeHidden)
        GetWindowRect(m_hwnd, &m_quakeRestore);
    ShowWindow(m_hwnd, SW_SHOWNA);
    for (int i = 1; i <= 6; ++i)
    {
        SetWindowPos(m_hwnd, HWND_TOP, wa.left, wa.top - H + H * i / 6, W, H,
                     SWP_SHOWWINDOW);
        Sleep(12);
    }
    SetForegroundWindow(m_hwnd);
    m_quakeHidden = false;
}

// ------------------------------------------------------------ jump list
void App::UpdateJumpList()
{
    std::vector<std::pair<std::wstring, std::string>> items;
    for (const amber::ConnectionProfile& p : m_profiles.All())
    {
        if (!p.Valid())
            continue;
        std::string title = p.name.empty() ? (p.username + "@" + p.host) : p.name;
        items.push_back({ WideFromUtf8(title), p.id });
    }
    // Workspaces ride the same "--connect <id>" mechanism with a "ws:" prefix,
    // so the jump list gains them without a second launch verb.
    m_workspaces.Load();
    for (const amber::Workspace& w : m_workspaces.All())
        items.push_back({ L"Workspace \x2014 " + WideFromUtf8(w.name),
                          "ws:" + w.name });
    amber::UpdateJumpList(items);
}

// ------------------------------------------------------------ snippets & triggers
static long long FileMtime(const std::filesystem::path& p)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    return ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

static std::vector<std::string> ReadUserLines(const std::filesystem::path& p)
{
    std::vector<std::string> lines;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f)
        return lines;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.append(buf, n);
    fclose(f);
    size_t pos = 0;
    while (pos <= data.size())
    {
        size_t nl = data.find('\n', pos);
        std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos
                                                                     : nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos && line[s] != '#')
            lines.push_back(line.substr(s));
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return lines;
}

void App::LoadSnippets()
{
    m_snippets.clear();
    std::filesystem::path p = amber::DataRoot() / "snippets.txt";
    m_snippetsMtime = FileMtime(p);
    for (const std::string& line : ReadUserLines(p))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        Snippet sn;
        sn.name = line.substr(0, eq);
        while (!sn.name.empty() && sn.name.back() == ' ')
            sn.name.pop_back();
        size_t v = line.find_first_not_of(" ", eq + 1);
        sn.text = (v == std::string::npos) ? std::string() : line.substr(v);
        if (!sn.name.empty() && m_snippets.size() < 100)
            m_snippets.push_back(std::move(sn));
    }
}

void App::LoadTriggers()
{
    m_triggers.clear();
    std::filesystem::path p = amber::DataRoot() / "triggers.txt";
    m_triggersMtime = FileMtime(p);
    for (const std::string& line : ReadUserLines(p))
    {
        try
        {
            m_triggers.emplace_back(
                line, std::regex(line, std::regex::ECMAScript | std::regex::icase));
        }
        catch (const std::regex_error&)
        {
            SetStatus("triggers.txt: bad pattern: " + line, 8.0);
        }
    }
}

void App::CheckUserFiles()
{
    if (m_time - m_userFilesChecked < 2.0)
        return;
    m_userFilesChecked = m_time;
    if (FileMtime(amber::DataRoot() / "snippets.txt") != m_snippetsMtime)
        LoadSnippets();
    if (FileMtime(amber::DataRoot() / "triggers.txt") != m_triggersMtime)
    {
        LoadTriggers();
        SetStatus("Output triggers reloaded (" + std::to_string(m_triggers.size()) +
                  " patterns)", 4.0);
    }
}

void App::EditUserFile(const char* name, const char* tmpl)
{
    std::filesystem::path p = amber::DataRoot() / name;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!std::filesystem::exists(p, ec))
    {
        if (FILE* f = _wfopen(p.c_str(), L"wb"))
        {
            fputs(tmpl, f);
            fclose(f);
        }
    }
    ShellExecuteW(m_hwnd, L"open", L"notepad.exe", p.c_str(), nullptr, SW_SHOWNORMAL);
    SetStatus(std::string("Editing ") + name + " — saved changes reload automatically.",
              6.0);
}

void App::RunSnippet(int index)
{
    if (index < 0 || index >= static_cast<int>(m_snippets.size()) || !HasSession())
        return;
    std::string text = m_snippets[static_cast<size_t>(index)].text;
    const amber::ConnectionProfile& prof = Foc().profile;
    // {host} {user} {port} from the session; any other {name} prompts.
    for (size_t open; (open = text.find('{')) != std::string::npos;)
    {
        size_t close = text.find('}', open);
        if (close == std::string::npos)
            break;
        std::string var = text.substr(open + 1, close - open - 1);
        std::string val;
        if (var == "host")      val = prof.host;
        else if (var == "user") val = prof.username;
        else if (var == "port") val = std::to_string(prof.port);
        else
        {
            std::string in;
            if (!PromptText(("Snippet value for {" + var + "}").c_str(), in))
                return;
            val = in;
        }
        text.replace(open, close - open + 1, val);
    }
    // Literal "\n" → Enter.
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n')
        {
            out.push_back('\r');
            ++i;
        }
        else
            out.push_back(text[i]);
    }
    SendToShell(out);
}

void App::ScanTriggers(amber::Session& s, const uint8_t* d, size_t n)
{
    StripToPlain(s.trigLine, d, n, s.trigEsc);
    size_t nl;
    while ((nl = s.trigLine.find('\n')) != std::string::npos)
    {
        std::string line = s.trigLine.substr(0, nl);
        s.trigLine.erase(0, nl + 1);
        if (line.empty() || line.size() > 4096)
            continue;
        // Lines already completed after this one in the same chunk: the
        // parser's cursor is that many rows further down.
        int later = static_cast<int>(std::count(s.trigLine.begin(), s.trigLine.end(), '\n'));
        bool fired = false;
        for (const auto& t : m_triggers)
        {
            if (std::regex_search(line, t.second))
            {
                FireTrigger(s, t.first, line, later);
                fired = true;
                break;
            }
        }
        // Built-in: error-looking lines smoulder; fatal ones shake the screen.
        if (!fired && m_fxLive)
        {
            static const std::regex kKill(
                "(segmentation fault|core dumped|\\bkilled\\b|out of memory|oom-kill|kernel panic)",
                std::regex::ECMAScript | std::regex::icase);
            static const std::regex kErr(
                "(\\berror\\b|\\bfatal\\b|traceback|exception|permission denied|"
                "no such file|command not found|\\bfailed\\b)",
                std::regex::ECMAScript | std::regex::icase);
            if (std::regex_search(line, kKill))
                AddEmber(s, true, later);
            else if (std::regex_search(line, kErr))
                AddEmber(s, false, later);
        }
    }
    if (s.trigLine.size() > 8192)
        s.trigLine.erase(0, s.trigLine.size() - 4096);
}

void App::FireTrigger(amber::Session& s, const std::string& pattern,
                      const std::string& line, int rowsBelow)
{
    s.lastTriggerAt = m_time;
    AddEmber(s, false, rowsBelow);
    SetStatus("Trigger [" + pattern + "]: " + line, 8.0);
    if (m_fxBell && HasSession() && &s == &Cur())
        TriggerShockwave();
    if (!m_focused || m_minimized)
    {
        m_tray.Toast(L"AmberSSH — " + WideFromUtf8(s.Caption()),
                     WideFromUtf8(line.substr(0, 200)));
        s.unread = true;
    }
}

// ------------------------------------------------------------ asciinema
void App::ToggleRecording()
{
    if (!HasSession())
        return;
    amber::Session& F = Foc();
    if (F.castFile)
    {
        fclose(F.castFile);
        F.castFile = nullptr;
        SetStatus("Recording stopped.");
        UpdateMenuChecks();
        return;
    }
    wchar_t path[MAX_PATH] = L"amberssh-session.cast";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"asciinema recordings\0*.cast\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"cast";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return;
    F.castFile = _wfopen(path, L"wb");
    if (!F.castFile)
    {
        SetStatus("Could not open the recording file.");
        return;
    }
    F.castStart = m_time;
    fprintf(F.castFile,
            "{\"version\": 2, \"width\": %d, \"height\": %d, \"timestamp\": %lld, "
            "\"env\": {\"TERM\": \"%s\", \"SHELL\": \"/bin/sh\"}, "
            "\"title\": \"%s\"}\n",
            F.grid.Cols(), F.grid.Rows(), static_cast<long long>(time(nullptr)),
            kTermTypes[std::clamp(m_termTypeId, 0, 2)], F.label.c_str());
    SetStatus("Recording to asciinema .cast — choose Record again to stop.");
    UpdateMenuChecks();
}

void App::RecordCast(amber::Session& s, const uint8_t* d, size_t n)
{
    // One "o" event per drained chunk, JSON-escaped.
    std::string esc;
    esc.reserve(n + 16);
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t b = d[i];
        switch (b)
        {
        case '"':  esc += "\\\""; break;
        case '\\': esc += "\\\\"; break;
        case '\n': esc += "\\n"; break;
        case '\r': esc += "\\r"; break;
        case '\t': esc += "\\t"; break;
        default:
            if (b < 0x20)
            {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", b);
                esc += u;
            }
            else
                esc.push_back(static_cast<char>(b));
        }
    }
    fprintf(s.castFile, "[%.6f, \"o\", \"%s\"]\n", m_time - s.castStart, esc.c_str());
}

void App::PlayRecording()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"asciinema recordings\0*.cast\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return;
    PlayRecordingFile(path);
}

void App::PlayRecordingFile(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return;
    std::string data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        data.append(buf, n);
    fclose(f);

    // A local tab (no SSH) whose queued events are fed by their timestamps.
    auto session = std::make_unique<amber::Session>();
    session->diagnostic = true;
    session->label = "playback";
    session->state = amber::SessionState::Connected;
    session->bornAt = m_time;
    session->grid.Init(m_gm.cols ? static_cast<int>(m_gm.cols) : 120,
                       m_gm.rows ? static_cast<int>(m_gm.rows) : 40);

    // Minimal v2 parsing: skip the header line, then [t, "o", "text"] lines.
    size_t pos = data.find('\n');
    pos = (pos == std::string::npos) ? data.size() : pos + 1;
    while (pos < data.size())
    {
        size_t nl = data.find('\n', pos);
        std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos
                                                                     : nl - pos);
        pos = (nl == std::string::npos) ? data.size() : nl + 1;
        if (line.size() < 8 || line[0] != '[')
            continue;
        double t = atof(line.c_str() + 1);
        size_t q = line.find("\"o\"");
        if (q == std::string::npos)
            continue;
        size_t s0 = line.find('"', q + 3);
        if (s0 == std::string::npos)
            continue;
        std::string text;
        for (size_t i = s0 + 1; i < line.size(); ++i)
        {
            char ch = line[i];
            if (ch == '"')
                break;
            if (ch == '\\' && i + 1 < line.size())
            {
                char e = line[++i];
                switch (e)
                {
                case 'n': text.push_back('\n'); break;
                case 'r': text.push_back('\r'); break;
                case 't': text.push_back('\t'); break;
                case 'b': text.push_back('\b'); break;
                case 'u':
                    if (i + 4 < line.size())
                    {
                        unsigned v = static_cast<unsigned>(
                            strtoul(line.substr(i + 1, 4).c_str(), nullptr, 16));
                        AppendUtf8(text, static_cast<char32_t>(v));
                        i += 4;
                    }
                    break;
                default: text.push_back(e); break;
                }
            }
            else
                text.push_back(ch);
        }
        session->castEvents.push_back({ t, std::move(text) });
    }
    session->castPlayStart = m_time;
    // A recording is a terminal stream like any other: it can carry OSC 133
    // marks, OSC 7 and inline images, so it gets the full sink set.
    BindSessionSinks(*session);
    m_sessions.push_back(std::move(session));
    m_active = static_cast<int>(m_sessions.size()) - 1;
    SelectTab(m_active);
    SetStatus("Playing recording (" + std::to_string(Cur().castEvents.size()) +
              " events)");
}

// ------------------------------------------------------------ boot sequence
// BIOS/POST-style log that types out while the SSH handshake runs (paced by
// DrainSessionOutput). The parser reset on connect wipes it, so the remote
// MOTD lands on a clean screen.
std::string App::BootSequenceText(const std::string& host) const
{
    std::string h = host.empty() ? "remote host" : host;
    std::string d;
    d += "\x1b[1;33mAMBERSSH PHOSPHOR BIOS v5.1\x1b[0m\r\n";
    d += "\x1b[2mCopyright (C) 2026 Amber Systems. Tube array licensed.\x1b[0m\r\n";
    d += "\r\n";
    d += "Memory test ................ 65536 KB \x1b[1mOK\x1b[0m\r\n";
    d += "Nixie tubes ................ 8 detected, all warm\r\n";
    d += "Particle kernel ............ loaded\r\n";
    d += "Phosphor calibration ....... amber, 3200K\r\n";
    d += "Glyph atlas ................ resident\r\n";
    d += "\r\n";
    d += "Resolving \x1b[1m" + h + "\x1b[0m ...\r\n";
    d += "Negotiating SSH transport .. kex, host key, cipher\r\n";
    d += "Opening channel ............ pty xterm-256color\r\n";
    d += "\r\n";
    d += "\x1b[2mWaiting for remote shell...\x1b[0m\r\n";
    return d;
}

// ------------------------------------------------------------ roadmap features
bool App::PromptText(const char* title, std::string& inOut)
{
    wchar_t buf[512] = L"";
    std::wstring t = WideFromUtf8(title);
    if (!RunPromptDialog(m_hwnd, t.c_str(), t.c_str(),
                         WideFromUtf8(inOut).c_str(), false, buf, 512))
        return false;
    inOut = Utf8FromWide(buf);
    return true;
}

void App::ToggleLogging()
{
    if (!HasSession())
        return;
    amber::Session& F = Foc();
    if (F.logFile)
    {
        fclose(F.logFile);
        F.logFile = nullptr;
        SetStatus("Session logging stopped.");
        UpdateMenuChecks();
        return;
    }
    wchar_t path[MAX_PATH] = L"amberssh-session.log";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Log files\0*.log;*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"log";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return;
    F.logFile = _wfopen(path, L"ab");
    F.logEscState = 0;
    F.logMaskBuf.clear();
    if (!F.logFile)
        SetStatus("Could not open the log file.");
    else if (F.logRaw && m_cloak.enabled)
        // Raw logging is byte-exact by definition — masking it would corrupt
        // the escape sequences it exists to preserve. Saying so is better than
        // letting the cloak's presence imply a protection the file does not
        // have.
        SetStatus("Logging RAW session output — the Privacy Cloak does not "
                  "mask a raw log. Switch to plain text to have it masked.",
                  10.0);
    else if (m_cloak.enabled)
        SetStatus("Logging session output (plain text, masked by the "
                  "Privacy Cloak).", 6.0);
    else
        SetStatus("Logging session output (plain text).");
    UpdateMenuChecks();
}

void App::SearchScrollbackPrompt()
{
    if (!HasSession())
        return;
    std::string term = m_searchTerm;
    if (!PromptText("Search scrollback (upward)", term) || term.empty())
        return;
    m_searchTerm = term;
    m_searchAbsRow = -1;
    SearchNext();
}

void App::SearchNext()
{
    if (!HasSession())
        return;
    if (m_searchTerm.empty())
    {
        SetStatus("No search term — Ctrl+Shift+F first.");
        return;
    }
    amber::Session& F = Foc();
    Grid& g = F.grid;
    auto lower = [](std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string needle = lower(m_searchTerm);
    int total = g.ScrollbackSize() + g.Rows();
    int start = (m_searchAbsRow < 0) ? total - 1 : m_searchAbsRow - 1;
    for (int r = std::min(start, total - 1); r >= 0; --r)
    {
        std::string line;
        line.reserve(static_cast<size_t>(g.Cols()));
        for (int c = 0; c < g.Cols(); ++c)
        {
            char32_t cp = g.AbsCell(r, c).cp;
            line.push_back(cp >= 32 && cp < 127
                               ? static_cast<char>(
                                     std::tolower(static_cast<int>(cp)))
                               : ' ');
        }
        size_t p = line.find(needle);
        if (p == std::string::npos)
            continue;

        m_searchAbsRow = r;
        // Search reads the raw grid, so it finds matches inside collapsed
        // command blocks too. That is deliberate — silently skipping folded
        // output would make the result depend on what happened to be folded.
        // The block is expanded so the match is actually visible, and the
        // status line says so.
        const uint64_t rowId = g.TotalPushed() -
                               static_cast<uint64_t>(g.ScrollbackSize()) +
                               static_cast<uint64_t>(r);
        bool expanded = false;
        RevealRow(F, rowId, &expanded);
        const int sb = g.ScrollbackSize();
        const int vr = r - (sb - g.ViewOffset());
        F.selActive = true;
        F.selecting = false;
        F.selStartR = F.selEndR = vr;
        F.selStartC = static_cast<int>(p);
        F.selEndC = static_cast<int>(p + needle.size()) - 1;
        F.selAnimStart = m_lastFrameTime;
        SetStatus(expanded ? "Found \"" + m_searchTerm +
                                 "\" inside a folded command — expanded it"
                           : "Found \"" + m_searchTerm + "\" — Ctrl+G for previous",
                  4.0);
        return;
    }
    m_searchAbsRow = -1;
    SetStatus("\"" + m_searchTerm + "\" not found further up (Ctrl+G wraps).",
              4.0);
}

// Ctrl+click on a remote path: opens the SFTP browser at the directory that
// contains it. Returns false when the token under the cursor is not a path,
// so the caller can fall through to bare-URL scanning.
bool App::OpenRemotePathAt(int row, int col)
{
    if (!HasSession())
        return false;
    amber::Session& F = Foc();
    if (F.diagnostic || F.state != amber::SessionState::Connected)
        return false;
    const Grid& g = F.grid;
    auto at = [&](int c) -> char32_t { return g.ViewCell(row, c).cp; };
    auto isBreak = [](char32_t cp) {
        return cp == 0 || cp == U' ' || cp == U'\t' || cp == U'"' ||
               cp == U'\'' || cp == U'`' || cp == U'(' || cp == U')' ||
               cp == U'<' || cp == U'>' || cp == U'[' || cp == U']';
    };
    if (col < 0 || col >= g.Cols() || isBreak(at(col)))
        return false;
    int a = col, b = col;
    while (a > 0 && !isBreak(at(a - 1)))
        --a;
    while (b + 1 < g.Cols() && !isBreak(at(b + 1)))
        ++b;
    std::string tok;
    for (int c = a; c <= b; ++c)
        AppendUtf8(tok, at(c));
    // Compiler and linter output appends ":line" or ":line:col" — strip it.
    while (!tok.empty())
    {
        size_t colon = tok.find_last_of(':');
        if (colon == std::string::npos || colon + 1 >= tok.size())
            break;
        bool allDigits = true;
        for (size_t i = colon + 1; i < tok.size(); ++i)
            if (!isdigit(static_cast<unsigned char>(tok[i])))
                allDigits = false;
        if (!allDigits)
            break;
        tok.resize(colon);
    }
    while (!tok.empty() && strchr(".,;:", tok.back()) != nullptr)
        tok.pop_back();
    if (tok.empty() || tok.find("://") != std::string::npos)
        return false;
    // A path is an absolute one, an explicitly relative one, or something with
    // a separator in it. A bare word is left alone — it is far more likely to
    // be prose than a file, and there is no cheap way to check remotely.
    const bool absolute = tok[0] == '/';
    const bool explicitRel = tok.rfind("./", 0) == 0 || tok.rfind("../", 0) == 0 ||
                             tok.rfind("~/", 0) == 0;
    if (!absolute && !explicitRel && tok.find('/') == std::string::npos)
        return false;

    std::string full = tok;
    if (!absolute && tok[0] != '~')
    {
        if (F.cwd.empty())
        {
            SetStatus("Cannot resolve a relative path: the remote working "
                      "directory is unknown (needs OSC 7)", 5.0);
            return true;
        }
        full = F.cwd;
        if (full.back() != '/')
            full += '/';
        full += (tok.rfind("./", 0) == 0) ? tok.substr(2) : tok;
    }
    // Open the containing directory; a trailing slash means it IS one.
    std::string dir = full;
    if (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    else
    {
        size_t slash = dir.find_last_of('/');
        dir = (slash == std::string::npos) ? std::string("/")
              : (slash == 0)               ? std::string("/")
                                           : dir.substr(0, slash);
    }
    amber::SftpBrowser::OpenTab(m_hwnd, F.profile, F.savedPassword,
                                F.savedPassphrase, dir);
    SetStatus("SFTP: " + dir, 4.0);
    return true;
}

void App::OpenUrlAt(int row, int col)
{
    if (!HasSession())
        return;
    const Grid& g = Foc().grid;
    // OSC 8 hyperlink under the cell wins over bare-URL scanning. Only web
    // and mail schemes are launched — a remote program must not be able to
    // hand the shell a file:// or custom-scheme target.
    if (uint16_t id = g.ViewCell(row, col).link)
    {
        const std::string& uri = Foc().parser.LinkUri(id);
        if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0 ||
            uri.rfind("mailto:", 0) == 0)
        {
            ShellExecuteW(nullptr, L"open", WideFromUtf8(uri).c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
            SetStatus("Opened " + uri, 4.0);
        }
        else
            SetStatus("Link not opened (unsupported scheme): " + uri, 5.0);
        return;
    }
    // No hyperlink here: try the token under the cursor as a REMOTE PATH. A
    // path in an ls listing, a stack trace or a compiler error is the thing
    // you most often want to open next, and the remote working directory is
    // already tracked, so a relative one resolves correctly.
    if (OpenRemotePathAt(row, col))
        return;
    std::string line;
    line.reserve(static_cast<size_t>(g.Cols()));
    for (int c = 0; c < g.Cols(); ++c)
    {
        char32_t cp = g.ViewCell(row, c).cp;
        line.push_back(cp >= 32 && cp < 127 ? static_cast<char>(cp) : ' ');
    }
    auto isUrlCh = [](char ch)
    {
        return std::isalnum(static_cast<unsigned char>(ch)) ||
               strchr("-._~:/?#[]@!$&'()*+,;=%", ch) != nullptr;
    };
    for (size_t pos = 0; (pos = line.find("http", pos)) != std::string::npos;
         ++pos)
    {
        size_t end = pos;
        if (line.compare(pos, 8, "https://") == 0)
            end = pos + 8;
        else if (line.compare(pos, 7, "http://") == 0)
            end = pos + 7;
        else
            continue;
        while (end < line.size() && isUrlCh(line[end]))
            ++end;
        while (end > pos && strchr(".,;:!?)]'\"", line[end - 1]))
            --end;
        if (col >= static_cast<int>(pos) && col < static_cast<int>(end))
        {
            std::string url = line.substr(pos, end - pos);
            ShellExecuteW(m_hwnd, L"open", WideFromUtf8(url).c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
            SetStatus("Opening " + url, 3.0);
            return;
        }
        pos = end;
    }
    // No URL: treat the token under the cell as a remote file name and open
    // it in the SFTP browser (download → default app, re-upload on save).
    PreviewRemoteFileAt(row, col);
}

void App::PreviewRemoteFileAt(int row, int col)
{
    amber::Session& F = Foc();
    const Grid& g = F.grid;
    std::string line;
    for (int c = 0; c < g.Cols(); ++c)
    {
        const Cell& cell = g.ViewCell(row, c);
        if (cell.flags & CellWideTail)
            continue;
        amber::AppendClusterUtf8(line, (cell.cp == 0) ? U' ' : cell.cp);
    }
    // Token boundaries: whitespace and the usual listing decorations.
    auto isSep = [](char ch) { return ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' ||
                                      ch == '(' || ch == ')' || ch == '[' || ch == ']' ||
                                      ch == ',' || ch == ';' || ch == '*' || ch == '@'; };
    // Map the clicked column to a byte offset (cells → UTF-8 bytes).
    size_t byte = 0;
    for (int c = 0; c < col && c < g.Cols(); ++c)
    {
        const Cell& cell = g.ViewCell(row, c);
        if (cell.flags & CellWideTail)
            continue;
        std::string tmp;
        AppendUtf8(tmp, (cell.cp == 0) ? U' ' : cell.cp);
        byte += tmp.size();
    }
    if (byte >= line.size() || isSep(line[byte]))
    {
        SetStatus("No URL or file name under that cell (Ctrl+click a link or a file).");
        return;
    }
    size_t s = byte, e = byte;
    while (s > 0 && !isSep(line[s - 1])) --s;
    while (e < line.size() && !isSep(line[e])) ++e;
    std::string token = line.substr(s, e - s);
    while (!token.empty() && (token.back() == ':' || token.back() == '.' || token.back() == '/'))
        token.pop_back();
    if (token.empty())
        return;
    if (F.diagnostic || F.state != amber::SessionState::Connected)
    {
        SetStatus("File preview needs a connected SSH session.");
        return;
    }
    if (F.cwd.empty() && token[0] != '/')
    {
        SetStatus("Remote directory unknown — add OSC 7 to your shell "
                  "(e.g. zsh: precmd(){ printf '\\e]7;file://%s%s\\a' $HOST $PWD }).", 10.0);
        return;
    }
    std::string path = (token[0] == '/') ? token : amber::SftpJoin(F.cwd, token);
    SetStatus("Opening " + path + " via SFTP...", 5.0);
    amber::SftpBrowser::OpenRemoteFile(m_hwnd, F.profile, F.savedPassword,
                                       F.savedPassphrase, path);
}

void App::SftpNewTab()
{
    amber::ConnectionRequest req;
    if (!amber::ConnectionDialog::Show(m_hwnd, m_profiles, req))
        return;
    amber::SftpBrowser::OpenTab(m_hwnd, req.profile, req.password, req.passphrase);
    req.password.Clear();
    req.passphrase.Clear();
}

void App::StartReconnect(amber::Session& s)
{
    SshConfig cfg;
    BuildSshConfig(s.profile, cfg);
    cfg.password = s.savedPassword.Reveal();
    cfg.passphrase = s.savedPassphrase.Reveal();
    cfg.proxyPass = s.savedProxyPassword.Reveal();
    cfg.cols = std::max(2, s.grid.Cols());
    cfg.rows = std::max(2, s.grid.Rows());
    // Guardian > Reattach: "re-establish port forwards". Off means a
    // reconnected session comes back WITHOUT its tunnels, which is what
    // somebody wants when a listener is shared with another tool. The first
    // connect always gets them — the setting is about the reconnect.
    if (s.everConnected && !s.profile.restoreForwards)
        cfg.forwards.clear();

    amber::Session* raw = &s;
    s.parser.SetWriter([raw](const char* d, size_t n) { raw->ssh.Send(d, n); });
    // Nothing is fed to the parser here. The old build wrote a yellow
    // "[amberssh: reconnecting...]" line into the grid, which then turned up
    // in selections, scrollback searches and session logs as if the server
    // had sent it. The annotation is drawn over the view instead.
    // A duplicated split pane comes through here too, and that is a first
    // connect, not a reconnect — the tab should say so.
    s.state = s.everConnected ? amber::SessionState::Reconnecting
                              : amber::SessionState::Connecting;
    s.status = s.guardian.StatusText(m_time);
    if (s.status.empty())
        s.status = "connecting...";

    // What to put back once the link is up. Built now, from the profile and
    // the last known directory, so a failure to build is reported before the
    // attempt rather than in the middle of it.
    {
        amber::RestoreRequest rr;
        rr.reattach = s.profile.reattachMode;
        rr.reattachSession = s.profile.reattachSession;
        rr.reattachCustom = s.profile.reattachCommand;
        rr.restoreCwd = s.profile.restoreCwd;
        rr.cwd = s.cwd;
        std::string rerr;
        s.restorePlan = amber::BuildRestorePlan(rr, rerr);
        if (!rerr.empty())
            SetStatus("Reattach: " + rerr, 8.0);
    }

    if (!s.ssh.Start(cfg))
    {
        // The worker refused to start (a thread still winding down). Hand it
        // back to the guardian as a transient failure rather than leaving the
        // tab wedged in Reconnecting with nothing scheduled.
        amber::ScrubString(cfg.password);
        amber::ScrubString(cfg.passphrase);
        amber::ScrubString(cfg.proxyPass);
        s.guardian.OnDrop("could not start the connection", m_time);
        s.status = s.guardian.StatusText(m_time);
        return;
    }
    amber::ScrubString(cfg.password);
    amber::ScrubString(cfg.passphrase);
    amber::ScrubString(cfg.proxyPass);
    s.ssh.RequestResize(cfg.cols, cfg.rows);
}

// ---------------------------------------------------------------- guardian
// A view annotation: drawn over the grid at the row the cursor is on, never
// written into it. See Session::Notice.
void App::AddNotice(amber::Session& s, int kind, const std::string& text)
{
    if (!s.profile.reconnectBanner)
        return;
    amber::Session::Notice n;
    n.rowId = s.grid.TotalPushed() + static_cast<uint64_t>(std::max(0, s.grid.CurY()));
    n.t = m_time;
    n.kind = kind;
    n.text = text.size() > 160 ? text.substr(0, 160) : text;
    s.notices.push_back(std::move(n));
    if (s.notices.size() > 64)
        s.notices.erase(s.notices.begin());
}

// A command was running when the link died. Its outcome is unknowable from
// here, so it is recorded as interrupted and never given an exit code.
void App::NoteInterruptedCommand(amber::Session& s, const std::string& why)
{
    if (!s.cmdRunning)
        return;
    s.cmdRunning = false;
    s.hadInterrupted = true;
    s.interrupted.command = s.pendingCmd;
    s.interrupted.cwd = s.pendingCwd;
    s.interrupted.startedAt = s.pendingStartedAt;
    s.interrupted.ranForSec = std::max(0.0, m_time - s.cmdStart);
    if (!s.pendingCmd.empty() && m_journalOn)
    {
        amber::JournalEntry e;
        e.host = s.Caption();
        e.cwd = s.pendingCwd;
        e.command = MaskForStorage(s.pendingCmd);
        e.cwd = MaskForStorage(e.cwd);
        e.exitCode = -1;          // unknown, and stays unknown
        e.interrupted = true;
        e.durationSec = s.interrupted.ranForSec;
        e.startedAt = s.pendingStartedAt;
        m_journal.Add(std::move(e));
    }
    s.pendingCmd.clear();
    s.outputStartRow = 0;
    AddNotice(s, 1, "interrupted while a command was running — its outcome is "
                    "unknown (" + why + ")");
}

// A reconnect attempt succeeded.
void App::OnSessionRecovered(amber::Session& s)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "reconnected after %.1f s",
             s.guardian.LastOutageSec());
    AddNotice(s, 2, msg);
    if (HasSession() && &s == &Cur())
        SetStatus(std::string("Reconnected to ") + s.label + " after " +
                      std::to_string(static_cast<int>(s.guardian.LastOutageSec() + 0.5)) + "s",
                  6.0);
    if (s.hadInterrupted)
    {
        s.hadInterrupted = false;
        AddNotice(s, 1, s.interrupted.command.empty()
                            ? std::string("the command that was running did not survive the drop")
                            : ("\"" + s.interrupted.command +
                               "\" was interrupted — AmberSSH does not know how it ended"));
    }
    // The restore plan, sent once. It is only ever a reattach command and a
    // cd, both built by BuildRestorePlan; nothing the user typed is replayed.
    for (const amber::RestoreStep& step : s.restorePlan)
    {
        const std::string line = step.text + "\n";
        s.ssh.Send(line.data(), line.size());
        AddNotice(s, 0, (step.kind == amber::RestoreStep::Kind::Reattach
                             ? "reattach: "
                             : "restored directory: ") + step.text);
    }
    s.restorePlan.clear();
    // Only while the window is in the background, and only on the outcome —
    // never once per failed attempt.
    if (s.profile.reconnectNotify && (!m_focused || m_minimized))
    {
        m_tray.Toast(L"AmberSSH — " + WideFromUtf8(s.Caption()),
                     WideFromUtf8(msg));
        s.unread = true;
    }
}

// "Stop reconnecting" — the user takes the session off the guardian's hands.
void App::GuardianStop(amber::Session& s)
{
    if (!s.guardian.Armed() && !s.guardian.NeedsConsent())
    {
        SetStatus("This session is not waiting to reconnect.", 4.0);
        return;
    }
    s.guardian.StopByUser();
    s.state = amber::SessionState::Disconnected;
    s.status = s.guardian.StatusText(m_time);
    s.restorePlan.clear();
    AddNotice(s, 1, "reconnect stopped");
    SetStatus("Reconnect stopped for " + s.label + ".", 5.0);
}

// "Reconnect now" — collapses the remaining wait, and is also the way back
// out of "gave up". The attempt itself still runs the whole authentication
// and host-key check: what changes is only that a human asked for it.
void App::GuardianRetryNow(amber::Session& s)
{
    if (s.ssh.Running())
    {
        SetStatus("This session is still connected.", 4.0);
        return;
    }
    if (!s.everConnected)
    {
        SetStatus("This session has never connected — open it from the "
                  "connection manager.", 6.0);
        return;
    }
    s.userClosed = false;
    s.guardian.RetryNow(m_time);
    s.state = amber::SessionState::Reconnecting;
    s.status = s.guardian.StatusText(m_time);
    SetStatus("Reconnecting to " + s.label + "...", 4.0);
}

// Every retry was used up.
void App::OnSessionGaveUp(amber::Session& s)
{
    s.state = amber::SessionState::Error;
    s.status = s.guardian.StatusText(m_time);
    AddNotice(s, 1, s.status);
    s.restorePlan.clear();
    if (s.profile.reconnectNotify && (!m_focused || m_minimized))
    {
        m_tray.Toast(L"AmberSSH — " + WideFromUtf8(s.Caption()),
                     WideFromUtf8(s.status + " — " + s.guardian.LastReason()));
        s.unread = true;
    }
}

// ============================================================ profile plumbing
// Everything from the Connection dialog's pages that the transport needs.
void App::BuildSshConfig(const amber::ConnectionProfile& p, SshConfig& cfg) const
{
    cfg.protocol = static_cast<int>(p.protocol);
    cfg.host = p.host;
    cfg.port = p.port;
    cfg.user = p.username;
    cfg.useKey = p.auth == amber::AuthMethod::PublicKey;
    cfg.useAgent = p.auth == amber::AuthMethod::Agent;
    cfg.keyPath = p.privateKeyPath;
    cfg.termType = p.termType.empty() ? std::string(kTermTypes[std::clamp(m_termTypeId, 0, 2)])
                                      : p.termType;
    cfg.termSpeed = p.termSpeed;
    cfg.envVars = p.envVars;
    cfg.forwards = p.forwards;
    cfg.jumpHost = p.jumpHost;
    // Connection
    cfg.connectTimeoutSeconds = p.connectTimeoutSeconds;
    cfg.keepaliveSeconds = p.keepaliveSeconds;
    cfg.tcpNoDelay = p.tcpNoDelay;
    cfg.tcpKeepalive = p.tcpKeepalive;
    cfg.ipVersion = p.ipVersion;
    cfg.logicalHost = p.logicalHost;
    // Proxy
    cfg.proxyType = static_cast<int>(p.proxyType);
    cfg.proxyHost = p.proxyHost;
    cfg.proxyPort = p.proxyPort;
    cfg.proxyUser = p.proxyUser;
    cfg.proxyExclude = p.proxyExclude;
    cfg.proxyLocalhost = p.proxyLocalhost;
    cfg.proxyDns = p.proxyDns;
    // SSH
    cfg.remoteCommand = p.remoteCommand;
    cfg.noShell = p.noShell;
    cfg.compression = p.compression;
    cfg.cipherPref = p.cipherPref;
    cfg.kexPref = p.kexPref;
    cfg.hostKeyPref = p.hostKeyPref;
    cfg.manualHostKeys.clear();
    {
        size_t pos = 0;
        const std::string& k = p.manualHostKeys;
        while (pos < k.size())
        {
            size_t end = k.find_first_of("\n,;", pos);
            std::string one = k.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? k.size() : end + 1;
            size_t a = one.find_first_not_of(" \t\r");
            if (a == std::string::npos)
                continue;
            cfg.manualHostKeys.push_back(one.substr(a));
        }
    }
    cfg.agentForward = p.agentForward;
    cfg.x11Forward = p.x11Forward;
    cfg.x11Display = p.x11Display;
    if (cfg.x11Forward)
    {
        // Untrusted forwarding: a fresh fake cookie per session goes to the
        // remote host, and the real one — read from .Xauthority — is
        // substituted locally as each connection opens. A remote host that is
        // compromised therefore never holds a credential that opens this
        // display, during the session or after it.
        cfg.x11FakeCookieHex = amber::MakeCookieHex();
        cfg.x11Backend = p.x11Backend;
        if (cfg.x11Backend == 1)
        {
            // AmberX is the display, and it is told to accept exactly the
            // cookie the remote host was given — so the real cookie is the
            // fake one. The setup-packet check still runs; the substitution
            // it performs is an identity.
            cfg.x11RealCookieHex = cfg.x11FakeCookieHex;
            // the identity strip on every AmberX frame: this session, this skin
            cfg.amberxIdentity = cfg.host + " \xc2\xb7 " + cfg.user;
            cfg.amberxSkin = amber::ChromeId();
            cfg.x11Trusted = p.x11Trust != 0;
            cfg.x11Clipboard = p.x11Clipboard;

            // ---- Remote GUI (Phase 7) ------------------------------------
            // The X screen this session gets. Computed here because this is
            // the process that knows where its own window is; the host is
            // told a rectangle, not a policy.
            RECT desk{};
            if (p.displayMode == 1)
            {
                HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi{ sizeof mi };
                if (GetMonitorInfoW(mon, &mi))
                    desk = mi.rcMonitor;
            }
            else if (p.displayMode == 2)
            {
                HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi{ sizeof mi };
                if (GetMonitorInfoW(mon, &mi))
                    desk = { mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.left + p.displayW, mi.rcMonitor.top + p.displayH };
            }
            cfg.x11DesktopX = desk.left;
            cfg.x11DesktopY = desk.top;
            cfg.x11DesktopW = desk.right - desk.left;
            cfg.x11DesktopH = desk.bottom - desk.top;
            // Performance: a repaint ceiling for forwarded windows. Auto and
            // Quality leave it alone; the other two trade smoothness for the
            // work this machine does. It is not compression — see the page.
            cfg.x11PresentCapHz = p.perfMode == 2 ? 60 : p.perfMode == 3 ? 20 : 0;
            // Window mode: only native windows exist, so anything else is
            // carried through and reported by the session rather than
            // silently ignored here.
            cfg.x11WindowMode = p.windowMode;
        }
        else
        {
            const amber::XDisplay d = amber::ParseDisplay(cfg.x11Display);
            const std::vector<amber::XAuthEntry> entries =
                amber::ParseXAuthority(amber::LoadXAuthorityFile());
            cfg.x11RealCookieHex =
                amber::BytesToHex(amber::CookieForDisplay(entries, d));
        }
    }
    // Telnet / Rlogin / Serial
    cfg.telnetPassive = p.telnetPassive;
    cfg.telnetNewline = p.telnetNewline;
    cfg.rloginLocalUser = p.rloginLocalUser;
    cfg.localShellKey = p.localShellKey;
    cfg.localExe = p.localExe;
    cfg.localArgs = p.localArgs;
    cfg.localCwd = p.localCwd;
    cfg.localEnv = p.localEnv;
    cfg.localShellIntegration = p.localShellIntegration;
    cfg.serialPort = p.serialPort;
    cfg.serialBaud = p.serialBaud;
    cfg.serialDataBits = p.serialDataBits;
    cfg.serialStopBits = p.serialStopBits;
    cfg.serialParity = static_cast<int>(p.serialParity);
    cfg.serialFlow = static_cast<int>(p.serialFlow);
}

// Terminal / Features / Translation / Colours / Window pages -> parser and
// grid; Logging / Terminal pages -> per-session flags.
void App::ApplyProfileToSession(amber::Session& s)
{
    const amber::ConnectionProfile& p = s.profile;
    TermFeatures f;
    f.allowAppCursor = p.allowAppCursor;
    f.allowAppKeypad = p.allowAppKeypad;
    f.allowMouse = p.allowMouse;
    f.allowRemoteResize = p.allowRemoteResize;
    f.allowAltScreen = p.allowAltScreen;
    f.allowRemoteTitle = p.allowRemoteTitle;
    f.allowScrollbackClear = p.allowScrollbackClear;
    f.allowAnsiColours = p.allowAnsiColours;
    f.allow256Colours = p.allow256Colours;
    f.implicitCrInLf = p.implicitCr;
    f.implicitLfInCr = p.implicitLf;
    f.autowrapDefault = p.autoWrap;
    f.appCursorDefault = p.appCursorInitial;
    f.appKeypadDefault = p.appKeypadInitial;
    f.charset = static_cast<int>(p.charset);
    f.poorMansLineDrawing = p.poorMansLineDrawing;
    f.answerback = p.answerback;
    s.parser.SetFeatures(f);
    s.grid.SetScrollbackMax(static_cast<size_t>(std::max(0, p.scrollbackLines)));
    s.localEcho = p.localEcho == amber::TriState::On;        // Auto: resolved per frame
    s.lineEditing = p.localLineEdit == amber::TriState::On;
    s.logRaw = p.logMode == amber::LogMode::All;
    s.logFlush = p.logFlush;
    // Session Guardian: the reconnect policy travels with the profile, and
    // the jitter seed is derived from the profile id so two tabs on the same
    // host back off on different schedules instead of retrying in lockstep.
    amber::GuardianPolicy gp;
    gp.mode = p.reconnectMode;
    gp.maxAttempts = std::max(0, p.reconnectMaxAttempts);
    gp.jitterPercent = std::clamp(p.reconnectJitterPercent, 0, 50);
    gp.seed = amber::SeedFromId(p.id.empty() ? (p.host + p.username) : p.id);
    s.guardian.Configure(gp);
}

// Appearance / Effects pages: renderer-wide overrides the profile asks for.
void App::ApplyProfileGlobals(const amber::ConnectionProfile& p)
{
    if (!p.fontFamily.empty())
    {
        if (m_sampler.SetFontFamily(WideFromUtf8(p.fontFamily)))
            UpdateFontMetrics(m_fontPx);
        else
            SetStatus("Font \"" + p.fontFamily + "\" not found - keeping the current face", 5.0);
    }
    if (p.fontSize > 0.0f)
        UpdateFontMetrics(p.fontSize);
    if (p.densityPpc > 0)
    {
        m_densityAuto = false;
        m_densityPpc = std::clamp(static_cast<uint32_t>(p.densityPpc), 8u, kMaxParticlesPerCell);
        UpdateGridDims();
    }
    if (!p.effectPreset.empty())
    {
        struct Preset { const char* name; unsigned anim; int bloom, twinkle, trail; int density; };
        static const Preset kPresets[] = {
            { "Classic Amber",  0u, 1, 1, 1, 0 },
            { "Clean Amber",    0u, 0, 0, 0, 0 },
            { "Deep Phosphor",  0u, 2, 1, 2, 0 },
            { "Miami Night",    1u, 2, 2, 2, 0 },
            { "Performance 4K", 0u, 0, 0, 0, 48 },
        };
        for (const Preset& k : kPresets)
        {
            if (p.effectPreset != k.name)
                continue;
            m_motionStyle = static_cast<int>(k.anim);
            HandleMenuCommand(IdmBloomFirst + k.bloom);
            HandleMenuCommand(IdmTwinkleFirst + k.twinkle);
            HandleMenuCommand(IdmTrailFirst + k.trail);
            if (k.density > 0)
            {
                m_densityAuto = false;
                m_densityPpc = static_cast<uint32_t>(k.density);
                UpdateGridDims();
            }
            UpdateMenuChecks();
            SetStatus(std::string("Effect preset: ") + k.name, 3.0);
            break;
        }
    }
}

// Bell page: style, taskbar flash, overload protection.
void App::RingBell(amber::Session& s, bool activeTab)
{
    const amber::ConnectionProfile& p = s.profile;
    if (p.bellOverload)
    {
        if (m_time - s.bellBurstStart > 2.0)
        {
            s.bellBurstStart = m_time;
            s.bellBurst = 0;
        }
        if (++s.bellBurst > 5)
            s.bellMutedUntil = m_time + 5.0;
        if (m_time < s.bellMutedUntil)
            return;
    }
    if (p.bell == amber::BellStyle::None)
        return;
    const bool visual = p.bell == amber::BellStyle::Visual || p.bell == amber::BellStyle::Both;
    const bool beep = p.bell == amber::BellStyle::Beep || p.bell == amber::BellStyle::Both;
    if (visual && m_fxBell && activeTab && !m_minimized)
    {
        TriggerShockwave();
        SetStatus("bell", 0.8);
    }
    if (beep)
        MessageBeep(MB_OK);
    if (p.bellTaskbar && !m_focused)
    {
        FLASHWINFO fi = { sizeof(fi), m_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
        FlashWindowEx(&fi);
    }
}

// Logging page: &Y &M &D &T &H &P substitutions, && = literal &.
std::string App::ExpandLogName(const amber::ConnectionProfile& p) const
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string out;
    const std::string& in = p.logFile;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != '&' || i + 1 >= in.size())
        {
            out.push_back(in[i]);
            continue;
        }
        char c = static_cast<char>(toupper(static_cast<unsigned char>(in[++i])));
        char buf[32];
        switch (c)
        {
        case 'Y': snprintf(buf, sizeof(buf), "%04d", st.wYear); out += buf; break;
        case 'M': snprintf(buf, sizeof(buf), "%02d", st.wMonth); out += buf; break;
        case 'D': snprintf(buf, sizeof(buf), "%02d", st.wDay); out += buf; break;
        case 'T': snprintf(buf, sizeof(buf), "%02d%02d%02d", st.wHour, st.wMinute, st.wSecond); out += buf; break;
        case 'H': out += p.protocol == amber::Protocol::Serial ? p.serialPort : p.host; break;
        case 'P': out += std::to_string(p.port); break;
        case '&': out.push_back('&'); break;
        default:  out.push_back('&'); out.push_back(in[i]); break;
        }
    }
    // Characters that cannot appear in a file name (host names never do).
    for (char& ch : out)
        if (ch == ':' && &ch != out.data() + 1)
            ch = '-';
    return out;
}

void App::OpenProfileLog(amber::Session& s)
{
    if (s.profile.logMode == amber::LogMode::None || s.diagnostic)
        return;
    std::string name = ExpandLogName(s.profile);
    if (name.empty())
        return;
    if (s.logFile)
    {
        fclose(s.logFile);
        s.logFile = nullptr;
    }
    s.logFile = _wfopen(WideFromUtf8(name).c_str(), s.profile.logAppend ? L"ab" : L"wb");
    s.logEscState = 0;
    SetStatus(s.logFile ? "Logging to " + name : "Could not open log file " + name, 4.0);
    UpdateMenuChecks();
}

// Terminal page: local echo mirrors typing; local line editing holds the
// line until Enter. Returns true when the bytes were consumed here.
bool App::HandleLocalLine(amber::Session& s, const std::string& bytes)
{
    if (s.diagnostic || bytes.empty() || (!s.localEcho && !s.lineEditing))
        return false;
    auto echo = [&](const std::string& t) {
        s.parser.Feed(reinterpret_cast<const uint8_t*>(t.data()), t.size());
    };
    if (!s.lineEditing)
    {
        // Local echo only: mirror what we type, still send it at once.
        std::string e;
        for (char ch : bytes)
        {
            if (ch == '\r')
                e += "\r\n";
            else if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\t')
                e.push_back(ch);
        }
        echo(e);
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        unsigned char b = static_cast<unsigned char>(bytes[i]);
        if (b == '\r')
        {
            std::string line = s.lineBuf + "\r";
            s.lineBuf.clear();
            echo("\r\n");
            s.ssh.Send(line.data(), line.size());
        }
        else if (b == 0x7F || b == 0x08)
        {
            if (!s.lineBuf.empty())
            {
                do
                    s.lineBuf.pop_back();
                while (!s.lineBuf.empty() &&
                       (static_cast<unsigned char>(s.lineBuf.back()) & 0xC0) == 0x80);
                echo("\b \b");
            }
        }
        else if (b == 0x1B)
        {
            // Escape sequences (arrows, function keys) pass straight through.
            std::string rest = bytes.substr(i);
            s.ssh.Send(rest.data(), rest.size());
            return true;
        }
        else if (b < 0x20 && b != '\t')
        {
            char c = static_cast<char>(b);   // control keys act immediately
            s.ssh.Send(&c, 1);
        }
        else
        {
            s.lineBuf.push_back(static_cast<char>(b));
            if (s.localEcho || s.lineEditing)
                echo(std::string(1, static_cast<char>(b)));
        }
    }
    return true;
}

// Selection page, "Windows" mode: right-click opens this menu.
void App::ShowContextMenu(int px, int py)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"&Copy");
    AppendMenuW(m, MF_STRING, 2, L"&Paste");
    AppendMenuW(m, MF_STRING, 3, L"Select &All");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Clear Scroll&back");
    // If the click landed inside a recognised command block, its actions go
    // here — which is the place the spec asks for them, on the block itself.
    // The submenu is absent entirely when there is no block under the
    // pointer, rather than present and inert.
    // Held as an ID, not a pointer: TrackPopupMenu runs its own modal message
    // loop, and any block appended while it is up would reallocate the vector
    // out from under a pointer taken now.
    uint64_t hitId = 0;
    bool hitBookmarked = false;
    std::string hitCommand;
    if (HasSession())
    {
        int hr = 0, hc = 0;
        if (CellFromPx(px, py, hr, hc))
        {
            amber::Session& s = Foc();
            int co = 0, ro = 0;
            PaneOffset(s, co, ro);
            const uint64_t row = s.grid.TotalPushed() -
                                 static_cast<uint64_t>(s.grid.ViewOffset()) +
                                 static_cast<uint64_t>(std::max(0, hr - ro));
            if (const amber::CommandBlock* b = amber::BlockAtRow(s.blocks, row))
            {
                hitId = b->id;
                hitBookmarked = b->bookmarked;
                hitCommand = b->command;
            }
        }
    }
    if (hitId != 0)
    {
        HMENU blk = CreatePopupMenu();
        AppendMenuW(blk, MF_STRING, IdmBlockCopyCommand, L"Copy &Command");
        AppendMenuW(blk, MF_STRING, IdmBlockCopyOutput, L"Copy &Output");
        AppendMenuW(blk, MF_STRING, IdmBlockCopyBoth, L"Copy &Both");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(blk, MF_STRING, IdmBlockFold, L"&Fold / Expand");
        AppendMenuW(blk, MF_STRING, IdmBlockSearch, L"&Search in Output...");
        AppendMenuW(blk, MF_STRING, IdmBlockSnippet, L"Save as S&nippet...");
        AppendMenuW(blk, MF_STRING | (hitBookmarked ? MF_CHECKED : 0),
                    IdmBlockBookmark, L"Boo&kmark");
        AppendMenuW(blk, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(blk, MF_STRING, IdmBlockRerun, L"&Type the Command (does not run it)");
        AppendMenuW(blk, MF_STRING, IdmBlockRerunNow, L"&Run the Command Now");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        std::wstring label = L"Command Block";
        if (!hitCommand.empty())
        {
            std::string c = hitCommand.substr(0, 40);
            label += L": " + WideFromUtf8(c);
        }
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(blk), label.c_str());
    }
    POINT pt = { px, py };
    ClientToScreen(m_hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(m);
    if (!HasSession())
        return;
    amber::Session& F = Foc();
    switch (cmd)
    {
    case 1: CopySelection(); break;
    case 2: Paste(); break;
    case 3:
        F.selActive = true;
        F.selRect = false;
        F.selStartR = 0;
        F.selStartC = 0;
        F.selEndR = std::max(0, F.grid.Rows() - 1);
        F.selEndC = std::max(0, F.grid.Cols() - 1);
        break;
    case 4:
        F.grid.SetScrollbackMax(0);
        F.grid.SetScrollbackMax(static_cast<size_t>(std::max(0, F.profile.scrollbackLines)));
        SetStatus("Scrollback cleared.");
        break;
    default:
        if (cmd >= IdmBlockCopyCommand && cmd <= IdmBlockNextBookmark)
            BlockAction(cmd, hitId);
        break;
    }
}

// Features page allowed CSI 8 ; rows ; cols t: size the window to fit.
void App::OnRemoteResize(amber::Session& s, int cols, int rows)
{
    if (!HasSession() || &s != &Cur() || m_fullscreen || IsZoomed(m_hwnd) ||
        Cur().layout.Count() > 1)
        return;
    cols = std::clamp(cols, 20, 500);
    rows = std::clamp(rows, 5, 200);
    float pad = static_cast<float>(m_gapPx) * m_dpi / 96.0f;
    int cw = static_cast<int>(std::ceil(cols * m_gm.cellW + 2.0f * pad));
    int ch = static_cast<int>(std::ceil(rows * m_gm.cellH + m_titleBarH + 2.0f * pad));
    RECT wr, cr;
    GetWindowRect(m_hwnd, &wr);
    GetClientRect(m_hwnd, &cr);
    int dx = (wr.right - wr.left) - (cr.right - cr.left);
    int dy = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    SetWindowPos(m_hwnd, nullptr, 0, 0, cw + dx, ch + dy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Behaviour page: a fixed window title beats the remote title and the label.
std::string App::TitleFor(const amber::Session& s) const
{
    const std::string& t = !s.profile.windowTitle.empty() ? s.profile.windowTitle
                         : !s.remoteTitle.empty()          ? s.remoteTitle
                                                           : s.label;
    return t.empty() ? m_titleBase : (m_titleBase + " \xE2\x80\x94 " + t);
}

// Every parser sink a pane session needs, in one place. This used to be
// copied inline at each creation site, which is how a sink gets forgotten;
// it is also the mechanism that lets a session be re-homed, because binding
// is now a single call rather than five scattered lambdas.
void App::BindSessionSinks(amber::Session& s)
{
    amber::Session* raw = &s;
    raw->parser.SetWriter([raw](const char* d, size_t n) { raw->ssh.Send(d, n); });
    raw->parser.SetTitleSink([this, raw](const std::string& t)
    {
        raw->remoteTitle = t;
        if (HasSession() && &Cur() == raw)
            SetWindowTextW(m_hwnd, WideFromUtf8(TitleFor(*raw)).c_str());
    });
    raw->parser.SetClipboardSink([this](const std::string& b64)
    {
        std::string text = DecodeBase64(b64);
        if (text.empty())
            return;
        SetClipboardText(text);
        char msg[64];
        snprintf(msg, sizeof(msg), "Remote copied %zu characters", text.size());
        SetStatus(msg);
    });
    raw->parser.SetCwdSink([raw](const std::string& dir) { raw->cwd = dir; });
    raw->parser.SetImageSink([this, raw](amber::DecodedImage&& img, int cols, int rows)
                             { return OnInlineImage(*raw, std::move(img), cols, rows); });
    raw->parser.SetMarkSink([this, raw](char kind, int code, bool hasCode)
                            { OnShellMark(*raw, kind, code, hasCode); });
    raw->parser.SetResizeSink([this, raw](int c, int r) { OnRemoteResize(*raw, c, r); });
}

std::unique_ptr<amber::Session> App::MakePaneSession(const amber::Session& from)
{
    auto p = std::make_unique<amber::Session>();
    p->profile = from.profile;
    p->label = from.label;
    p->state = amber::SessionState::Connecting;
    p->status = "connecting...";
    p->bornAt = m_time;
    p->savedPassword.Assign(from.savedPassword.View());
    p->savedPassphrase.Assign(from.savedPassphrase.View());
    p->savedProxyPassword.Assign(from.savedProxyPassword.View());
    p->grid.Init(std::max(2, from.grid.Cols() / 2), std::max(2, from.grid.Rows()));
    BindSessionSinks(*p);
    ApplyProfileToSession(*p);      // the same Connection-dialog behaviour
    return p;
}

// A pane's rect in the active tab's cell grid, from the layout tree.
amber::PaneRect App::PaneRectOf(const amber::Session& s) const
{
    if (!HasSession())
        return {};
    const amber::Session& tab = Cur();
    const amber::PaneId id = PaneIdOf(tab, s);
    if (id == amber::kNoPane)
        return {};
    if (tab.layout.Empty())
        return amber::PaneRect{ 0, 0, static_cast<int>(m_gm.cols),
                                static_cast<int>(m_gm.rows) };
    return tab.layout.RectOf(id, static_cast<int>(m_gm.cols),
                             static_cast<int>(m_gm.rows));
}

// Splits the focused pane, connecting `profile` in the new one — or a clone
// of the pane being split when it is null. Returns the new pane's id, or
// kNoPane when the split was refused. The single place a pane is created, so
// the tree and the session list cannot drift apart.
amber::PaneId App::AddPane(const amber::ConnectionProfile* profile, bool vertical)
{
    if (!HasSession())
        return amber::kNoPane;
    amber::Session& tab = Cur();
    if (tab.IsVnc())
    {
        // a desktop has no panes: there is no shell to split, and a pane
        // session cloned from a VNC profile would be neither a terminal
        // nor a desktop
        SetStatus("A remote desktop tab cannot be split into panes", 4.0);
        return amber::kNoPane;
    }
    if (tab.diagnostic)
    {
        SetStatus("The diagnostic session cannot be split.");
        return amber::kNoPane;
    }
    if (tab.layout.Empty())
        tab.layout.Reset(0);
    const amber::PaneId id = tab.nextPaneId;
    const amber::SplitDir dir = vertical ? amber::SplitDir::Vertical
                                         : amber::SplitDir::Horizontal;
    // Ask the tree first: it refuses rather than creating a pane too small to
    // be a terminal, and a refusal must not leave a stray session behind.
    if (!tab.layout.Split(tab.focus, id, dir, static_cast<int>(m_gm.cols),
                          static_cast<int>(m_gm.rows)))
    {
        SetStatus(vertical ? "Not enough columns to split this pane."
                           : "Not enough rows to split this pane.", 5.0);
        return amber::kNoPane;
    }
    ++tab.nextPaneId;
    amber::Session* parent = PaneById(tab, tab.focus);
    auto p = MakePaneSession(parent ? *parent : tab);
    if (profile)
    {
        p->profile = *profile;
        p->label = profile->username.empty()
                       ? profile->host
                       : profile->username + "@" + profile->host;
        ApplyProfileToSession(*p);
    }
    amber::Session* raw = p.get();
    if (tab.extraPanes.size() < static_cast<size_t>(id))
        tab.extraPanes.resize(static_cast<size_t>(id));
    tab.extraPanes[static_cast<size_t>(id) - 1] = std::move(p);
    tab.focus = id;
    UpdateGridDims();               // sizes every pane and its PTY
    StartReconnect(*raw);           // a fresh connection for the new pane
    m_particles.Reset();
    return id;
}

void App::SplitPane(bool vertical)
{
    if (AddPane(nullptr, vertical) == amber::kNoPane)
        return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Split %s — %zu panes. Ctrl+Shift+arrows moves focus.",
             vertical ? "vertical" : "horizontal", Cur().layout.Count());
    SetStatus(msg, 5.0);
}

void App::CloseSplit()
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    if (tab.layout.Count() <= 1)
    {
        SetStatus("This tab has one pane — Ctrl+Shift+W closes the tab.", 5.0);
        return;
    }
    // Pane 0 is the tab's own session object, and the tab's identity, profile
    // and layout live on it. Closing it would mean promoting another pane's
    // Session into its place, and a Session owns a running worker thread and
    // so cannot be moved. Refused rather than closing the whole tab behind
    // the user's back — see the Stage 5 report on what would lift this.
    if (tab.focus == 0)
    {
        SetStatus("The tab's original pane cannot be closed on its own — "
                  "close another pane, or close the tab with Ctrl+Shift+W.", 7.0);
        return;
    }
    const amber::PaneId gone = tab.focus;
    if (!tab.layout.Close(gone))
    {
        SetStatus("Could not close that pane.", 4.0);
        return;
    }
    // Focus follows to a pane that still exists before the session dies, so
    // nothing can dereference the one being destroyed.
    tab.focus = tab.layout.Panes().empty() ? 0 : tab.layout.Panes().front();
    // Any broadcast target that just vanished goes with it.
    tab.broadcast.erase(std::remove(tab.broadcast.begin(), tab.broadcast.end(), gone),
                        tab.broadcast.end());
    // The slot is left null rather than erased, so ids are never reused and a
    // broadcast set cannot come to mean a different pane.
    if (static_cast<size_t>(gone) - 1 < tab.extraPanes.size())
        tab.extraPanes[static_cast<size_t>(gone) - 1].reset();   // dtor disconnects
    UpdateGridDims();
    m_particles.Reset();
    char msg[96];
    snprintf(msg, sizeof(msg), "Pane closed — %zu remain.", tab.layout.Count());
    SetStatus(msg);
}

// -------------------------------------------------------------- broadcast
// The target picker. A modal checklist of the tab's panes, because the only
// safe way to broadcast is for the user to have named each destination.
//
// Built as a menu rather than a dialog so it can be popped from the status
// bar, the menu and the palette without three layouts to keep skinned.
void App::PickBroadcastTargets()
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    const std::vector<amber::PaneId> ids = tab.layout.Panes();
    if (ids.size() <= 1)
    {
        SetStatus("Broadcast needs more than one pane — split this tab first.", 5.0);
        return;
    }
    HMENU m = CreatePopupMenu();
    // Command ids 1..N are the panes; the actions use high values so they
    // cannot collide with a pane index.
    std::vector<amber::PaneId> order;
    for (amber::PaneId id : ids)
    {
        const amber::Session* p = PaneById(tab, id);
        if (!p)
            continue;
        order.push_back(id);
        const bool on = std::find(tab.broadcast.begin(), tab.broadcast.end(), id) !=
                        tab.broadcast.end();
        std::wstring label = std::to_wstring(order.size()) + L". " +
                             WideFromUtf8(p->Caption());
        if (p->readOnly)
            label += L"  (read-only — cannot be a target)";
        else if (p->state != amber::SessionState::Connected)
            label += L"  (not connected)";
        UINT flags = MF_STRING | (on ? MF_CHECKED : 0u);
        if (p->readOnly)
            flags |= MF_GRAYED;
        AppendMenuW(m, flags, static_cast<UINT_PTR>(order.size()), label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1001, L"Select &All (excluding read-only)");
    AppendMenuW(m, MF_STRING, 1002, L"Select &None — stop broadcasting");

    POINT pt = {};
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_NONOTIFY,
                                   pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 0)
        return;
    if (cmd == 1002)
    {
        StopBroadcast();
        return;
    }
    if (cmd == 1001)
    {
        BroadcastAll();
        return;
    }
    const size_t idx = static_cast<size_t>(cmd) - 1;
    if (idx >= order.size())
        return;
    const amber::PaneId id = order[idx];
    const amber::Session* p = PaneById(tab, id);
    if (!p || p->readOnly)
        return;
    auto it = std::find(tab.broadcast.begin(), tab.broadcast.end(), id);
    if (it != tab.broadcast.end())
        tab.broadcast.erase(it);
    else
        tab.broadcast.push_back(id);
    ReportBroadcast();
    // Left open, in effect: picking one target then wanting another is the
    // normal case, so the picker comes straight back up.
    PickBroadcastTargets();
}

void App::BroadcastAll()
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    tab.broadcast.clear();
    for (amber::PaneId id : tab.layout.Panes())
    {
        const amber::Session* p = PaneById(tab, id);
        // Read-only panes are excluded: a pane that refuses typing cannot be
        // a broadcast target, and listing it would make the count a lie.
        if (p && !p->readOnly)
            tab.broadcast.push_back(id);
    }
    ReportBroadcast();
}

void App::StopBroadcast()
{
    if (!HasSession())
        return;
    Cur().broadcast.clear();
    SetStatus("Broadcast STOPPED — keystrokes go to the focused pane only.", 6.0);
    UpdateMenuChecks();
}

void App::ReportBroadcast()
{
    if (!HasSession())
        return;
    const amber::Session& tab = Cur();
    if (tab.broadcast.empty())
    {
        SetStatus("Broadcast off.");
    }
    else
    {
        std::string names;
        for (amber::PaneId id : tab.broadcast)
        {
            const amber::Session* p = PaneById(tab, id);
            if (!p)
                continue;
            if (!names.empty())
                names += ", ";
            names += p->Caption();
        }
        SetStatus("Broadcasting to " + std::to_string(tab.broadcast.size()) +
                      " panes: " + names + "  (Ctrl+Shift+B stops)",
                  8.0);
    }
    UpdateMenuChecks();
}

// ------------------------------------------------------------ pane commands
void App::FocusPane(amber::PaneLayout::Dir d)
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    const amber::PaneId to = tab.layout.Neighbour(tab.focus, d,
                                                  static_cast<int>(m_gm.cols),
                                                  static_cast<int>(m_gm.rows));
    if (to == amber::kNoPane)
        return;
    tab.focus = to;
    Foc().ClearSelection();
}

void App::FocusPaneCycle(int delta)
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    const amber::PaneId to = tab.layout.Cycle(tab.focus, delta);
    if (to == amber::kNoPane || to == tab.focus)
        return;
    tab.focus = to;
    Foc().ClearSelection();
}

void App::MovePane(amber::PaneLayout::Dir d)
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    const int cols = static_cast<int>(m_gm.cols), rows = static_cast<int>(m_gm.rows);
    const amber::PaneId to = tab.layout.Neighbour(tab.focus, d, cols, rows);
    if (to == amber::kNoPane)
    {
        SetStatus("No pane that way to move next to.", 4.0);
        return;
    }
    const amber::SplitDir sd = (d == amber::PaneLayout::Dir::Left ||
                                d == amber::PaneLayout::Dir::Right)
                                   ? amber::SplitDir::Vertical
                                   : amber::SplitDir::Horizontal;
    const size_t before = tab.layout.Count();
    if (!tab.layout.Move(tab.focus, to, sd, cols, rows))
    {
        // Move restores the layout when the destination has no room; the one
        // case it cannot is when nothing had room at all, which would have
        // dropped the pane. Detect that and say so rather than leaving a
        // session with no way to reach it.
        if (tab.layout.Count() != before)
            SetStatus("That pane could not be placed and was closed.", 6.0);
        else
            SetStatus("No room to move the pane there.", 4.0);
        UpdateGridDims();
        return;
    }
    UpdateGridDims();
    SetStatus("Pane moved.");
}

void App::SwapPaneWith(amber::PaneLayout::Dir d)
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    const amber::PaneId to = tab.layout.Neighbour(tab.focus, d,
                                                  static_cast<int>(m_gm.cols),
                                                  static_cast<int>(m_gm.rows));
    if (to == amber::kNoPane)
    {
        SetStatus("No pane that way to swap with.", 4.0);
        return;
    }
    if (!tab.layout.Swap(tab.focus, to))
        return;
    UpdateGridDims();
    SetStatus("Panes swapped.");
}

void App::ResizePane(amber::PaneLayout::Dir d)
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    if (tab.layout.Count() <= 1)
        return;
    // Left/Up shrink, Right/Down grow — the direction the edge moves.
    const float step = (d == amber::PaneLayout::Dir::Right ||
                        d == amber::PaneLayout::Dir::Down)
                           ? 0.04f
                           : -0.04f;
    if (!tab.layout.Resize(tab.focus, step))
        return;
    UpdateGridDims();               // propagates the new size to every PTY
}

void App::RotatePane()
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    if (!tab.layout.Rotate(tab.focus))
    {
        SetStatus("Nothing to rotate — this tab has one pane.", 4.0);
        return;
    }
    UpdateGridDims();
    SetStatus("Layout rotated.");
}

void App::ToggleZoomPane()
{
    if (!HasSession())
        return;
    amber::Session& tab = Cur();
    if (tab.layout.Count() <= 1)
    {
        SetStatus("Nothing to zoom — this tab has one pane.", 4.0);
        return;
    }
    if (tab.layout.Zoomed())
    {
        // The tree was never modified, so this restores the exact layout by
        // construction rather than by remembering it.
        tab.layout.ClearZoom();
        SetStatus("Zoom off — layout restored.");
    }
    else
    {
        tab.layout.SetZoom(tab.focus);
        SetStatus("Pane zoomed — Ctrl+Shift+Z restores the layout.", 5.0);
    }
    UpdateGridDims();
    m_particles.Reset();
}

void App::ToggleReadOnlyPane()
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    s.readOnly = !s.readOnly;
    if (s.readOnly)
    {
        // A read-only pane is also removed from the broadcast set: leaving it
        // there would mean the set reports a target it cannot reach.
        amber::Session& tab = Cur();
        const amber::PaneId id = PaneIdOf(tab, s);
        tab.broadcast.erase(std::remove(tab.broadcast.begin(), tab.broadcast.end(), id),
                            tab.broadcast.end());
    }
    SetStatus(s.readOnly
                  ? "Pane is READ-ONLY — output only. Ctrl+Shift+R unlocks it."
                  : "Pane unlocked — it accepts input again.",
              6.0);
}

// The text of one absolute row, trimmed. Used to name the command the view
// just landed on.
std::string App::RowText(const amber::Session& s, uint64_t rowId) const
{
    const Grid& g = s.grid;
    const int64_t delta = static_cast<int64_t>(rowId) -
                          static_cast<int64_t>(g.TotalPushed());
    const int abs = g.ScrollbackSize() + static_cast<int>(delta);
    if (abs < 0 || abs >= g.ScrollbackSize() + g.Rows())
        return {};
    std::string line;
    for (int c = 0; c < g.Cols(); ++c)
    {
        const Cell& cell = g.AbsCell(abs, c);
        if (cell.flags & CellWideTail)
            continue;
        amber::AppendClusterUtf8(line, (cell.cp == 0) ? U' ' : cell.cp);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    size_t first = line.find_first_not_of(' ');
    return (first == std::string::npos) ? std::string() : line.substr(first);
}

// Rebuilds the display-row map for a pane. Rows hidden by a collapsed fold
// are skipped and the first of them becomes the summary row.
void App::BuildFoldMap(amber::Session& s)
{
    const Grid& g = s.grid;
    // A reset (a reconnect, or ESC c) restarts TotalPushed, so every row id
    // the blocks hold is stale. Drop them rather than leave the gutter
    // pointing at rows that are not the rows those commands ran on.
    if (g.TotalPushed() < s.blocksPushedSeen)
    {
        s.blocks.clear();
        s.runningBlockId = 0;
        s.outputStartRow = 0;
        s.notifyRunning = false;
    }
    s.blocksPushedSeen = g.TotalPushed();
    // Rows that have scrolled out of the buffer take their blocks with them.
    // Done here, once a frame, rather than only when a command finishes, so a
    // session producing output without marks still bounds the list.
    amber::TrimBlocks(s.blocks,
                      g.TotalPushed() - static_cast<uint64_t>(g.ScrollbackSize()));
    const int rows = g.Rows();
    s.rowMap.assign(static_cast<size_t>(std::max(0, rows)), amber::Session::RowSlot{});
    // The alternate screen has no scrollback and no command structure, and a
    // full-screen app owns every row: folding must never touch it.
    if (s.blocks.empty() || g.AltActive() || !s.AnyCollapsed())
    {
        for (int i = 0; i < rows; ++i)
            s.rowMap[i] = { i, -1 };
        return;
    }
    const uint64_t base = g.TotalPushed() - static_cast<uint64_t>(g.ViewOffset());
    int src = 0;
    for (int d = 0; d < rows; ++d)
    {
        const uint64_t abs = base + static_cast<uint64_t>(src);
        int hit = -1;
        for (size_t i = 0; i < s.blocks.size(); ++i)
        {
            const amber::CommandBlock& f = s.blocks[i];
            if (f.collapsed && f.hasOutput && abs >= f.outputFirst && abs <= f.outputLast)
            {
                hit = static_cast<int>(i);
                break;
            }
        }
        if (hit >= 0)
        {
            s.rowMap[d] = { src, hit };
            // Skip the rest of the folded block in one step.
            const uint64_t last = s.blocks[static_cast<size_t>(hit)].outputLast;
            src += static_cast<int>(last - abs) + 1;
        }
        else
        {
            s.rowMap[d] = { src, -1 };
            ++src;
        }
    }
}

Cell App::FoldedCell(const amber::Session& s, int viewRow, int col) const
{
    if (viewRow < 0 || viewRow >= static_cast<int>(s.rowMap.size()))
        return s.grid.ViewCell(viewRow, col);
    const amber::Session::RowSlot& slot = s.rowMap[static_cast<size_t>(viewRow)];
    if (slot.blockIndex < 0)
        return s.grid.ViewCell(slot.src, col);
    // Synthetic summary row: the block's own text, in the accent colour so it
    // reads as chrome rather than as output.
    const amber::CommandBlock& f = s.blocks[static_cast<size_t>(slot.blockIndex)];
    Cell out;
    out.cp = (col >= 0 && col < static_cast<int>(f.summary.size()))
                 ? f.summary[static_cast<size_t>(col)]
                 : U' ';
    // Three outcomes, three colours: succeeded, failed, and not known —
    // which is neither of the other two and must not be painted as either.
    const uint32_t rgb = f.Failed() ? 0xFF6B4A : f.Unknown() ? 0xC0C0C0 : 0xFFB000;
    out.fg = amber::ColRgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    out.attr = AttrDim;
    return out;
}

// ------------------------------------------------------------ privacy cloak
// Rebuilds the per-cell mask for one pane. Runs the detector over the text as
// it will be DRAWN (folding applied), so what the user sees and what was
// scanned are the same thing.
//
// The mask is a display overlay and nothing else: the grid still holds the
// real characters, so search, selection and copy are unaffected, and turning
// the cloak off restores the screen with nothing lost.
void App::RebuildCloak(amber::Session& s, int rows, int cols)
{
    const size_t n = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    if (!m_cloak.enabled)
    {
        if (!s.cloakMask.empty())
        {
            s.cloakMask.clear();
            s.cloakStamp = 0;
            s.cloakCount = 0;
        }
        return;
    }

    // A cheap content stamp. Running the detector over every row every frame
    // would cost more than the whole compose pass; running it when the text
    // changes costs nothing anyone can measure.
    uint64_t stamp = 1469598103934665603ull;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
        {
            stamp ^= static_cast<uint64_t>(FoldedCell(s, r, c).cp);
            stamp *= 1099511628211ull;
        }
    if (stamp == s.cloakStamp && s.cloakMask.size() == n)
        return;
    s.cloakStamp = stamp;
    s.cloakMask.assign(n, 0);
    s.cloakCount = 0;

    // A PEM key spans many lines, so the block state is carried down the pane
    // in order — the same reason the module exposes PemState at all.
    amber::PemState pem;
    std::string line;
    std::vector<int> colOf;       // byte offset -> column
    for (int r = 0; r < rows; ++r)
    {
        line.clear();
        colOf.clear();
        for (int c = 0; c < cols; ++c)
        {
            const Cell cell = FoldedCell(s, r, c);
            if (cell.flags & CellWideTail)
                continue;
            const size_t before = line.size();
            amber::AppendClusterUtf8(line, (cell.cp == 0) ? U' ' : cell.cp);
            for (size_t k = before; k < line.size(); ++k)
                colOf.push_back(c);
        }
        auto cover = [&](int c0, int c1)
        {
            for (int c = c0; c <= c1 && c < cols; ++c)
                if (c >= 0)
                    s.cloakMask[static_cast<size_t>(r) * cols + c] = 1;
        };
        // A key body line carries no pattern of its own; the block state is
        // what says it must be covered.
        if (amber::UpdatePem(line, pem))
        {
            cover(0, cols - 1);
            ++s.cloakCount;
            continue;
        }
        const std::vector<amber::SecretSpan> spans = amber::FindSecrets(line, m_cloak);
        for (const amber::SecretSpan& sp : spans)
        {
            if (sp.begin >= colOf.size())
                continue;
            const size_t last = (std::min)(sp.end, colOf.size()) - 1;
            cover(colOf[sp.begin], colOf[last]);
            ++s.cloakCount;
        }
    }
}

// ----------------------------------------------------------- remote display
void App::ReportXServers()
{
    const std::vector<amber::XServerInfo> found =
        amber::RankXServers(amber::ScanForXServers(),
                            amber::DetectRunningDisplay());
    if (found.empty())
    {
        SetStatus("No X server found. Install one (VcXsrv, X410, Cygwin/X) — "
                  "AmberSSH does not bundle one. File > Remote Display > "
                  "Setup Guide.",
                  10.0);
        return;
    }
    const amber::XServerInfo& best = found.front();
    std::string msg = best.running
                          ? best.name + " is running on display :" +
                                std::to_string(best.display)
                          : best.name + " is installed but not running";
    // The terms are named because the user is choosing what to install on
    // their own machine, and because AmberSSH bundling none of these is the
    // reason those terms never apply to it.
    if (!best.licence.empty())
        msg += "  (" + best.licence + ")";
    if (found.size() > 1)
        msg += "  — " + std::to_string(found.size() - 1) + " more found";
    SetStatus(msg, 10.0);
}

void App::StartXServer()
{
    const int running = amber::DetectRunningDisplay();
    if (running >= 0)
    {
        SetStatus("An X server is already listening on display :" +
                      std::to_string(running) + ".",
                  5.0);
        return;
    }
    const std::vector<amber::XServerInfo> found =
        amber::RankXServers(amber::ScanForXServers(), -1);
    for (const amber::XServerInfo& i : found)
    {
        const std::string args = amber::XServerLaunchArgs(i.kind, 0);
        if (args.empty() || i.exePath.empty())
            continue;
        // Access control stays ON — see XServerLaunchArgs. Started detached:
        // the X server outlives this session on purpose, because closing a tab
        // should not take the user's windows with it.
        const std::wstring exe = WideFromUtf8(i.exePath);
        const std::wstring wargs = WideFromUtf8(args);
        const HINSTANCE rc = ShellExecuteW(m_hwnd, L"open", exe.c_str(),
                                           wargs.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(rc) > 32)
        {
            SetStatus("Starting " + i.name + " on display :0 …", 6.0);
            return;
        }
    }
    SetStatus("Nothing to start: no X server AmberSSH knows how to launch is "
              "installed.",
              8.0);
}

void App::LaunchRemoteApp(bool startWeston)
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    if (s.state != amber::SessionState::Connected || s.diagnostic ||
        s.profile.protocol != amber::Protocol::Ssh)
    {
        SetStatus("RemoteApp needs a connected SSH session.", 5.0);
        return;
    }

    amber::RemoteAppOptions o;
    o.startWeston = startWeston;
    const amber::RemoteAppPlan p = amber::PlanRemoteApp(o);
    if (!p.valid)
    {
        SetStatus("RemoteApp: " + p.why, 6.0);
        return;
    }

    // 1. The tunnel. Bound to loopback, always — the spec carries the bind
    //    address explicitly so it cannot default to every interface.
    s.ssh.AddForward(p.forwardSpec);

    // 2. The compositor, if asked for. Sent as a command line the user can see
    //    in their scrollback rather than executed invisibly: it runs on their
    //    machine and they are entitled to read it first.
    if (startWeston && !p.remoteCommand.empty())
    {
        const std::string line = p.remoteCommand + " &\r";
        s.ssh.Send(line.data(), line.size());
    }

    // 3. The client. mstsc ships with Windows, so this route bundles nothing
    //    and raises no licensing question at all.
    const std::wstring rdp = amber::WriteTempRdpFile(amber::RdpFile(p, false));
    HINSTANCE rc = nullptr;
    if (!rdp.empty())
        rc = ShellExecuteW(m_hwnd, L"open", L"mstsc.exe",
                           (L"\"" + rdp + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else
        rc = ShellExecuteW(m_hwnd, L"open", L"mstsc.exe",
                           WideFromUtf8(amber::MstscArgs(p)).c_str(), nullptr,
                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32)
    {
        SetStatus("RemoteApp: could not start mstsc.exe. The tunnel is up on "
                  "127.0.0.1:" + std::to_string(p.localPort) +
                      " for any RDP client.",
                  10.0);
        return;
    }
    SetStatus("RemoteApp: tunnel on 127.0.0.1:" + std::to_string(p.localPort) +
                  " → the host's own 127.0.0.1:" + std::to_string(p.remotePort),
              8.0);
}

void App::InvalidateCloak()
{
    ForEachSession([](amber::Session& s)
    {
        s.cloakMask.clear();
        s.cloakStamp = 0;
        s.cloakCount = 0;
    });
}

// The masking used for text that LEAVES the terminal — the journal, the status
// bar, a notification. Unlike the on-screen cover this replaces a span with a
// fixed marker, so the length of the secret does not travel with it either.
std::string App::CloakText(const std::string& line) const
{
    if (!m_cloak.enabled)
        return line;
    return amber::MaskLine(line, m_cloak);
}

// ------------------------------------------------------------- blast radius
// The text of the command that is about to be submitted.
//
// With shell integration (OSC 133) this is exact: the 'B' mark says where the
// prompt ended, so what comes back is precisely what the user typed. Without
// it the line is recovered from the screen and the prompt is guessed at, which
// is why the caller is told which of the two it got — a warning built on a
// guess has to say so.
std::vector<std::string> App::CommandAboutToRun(const amber::Session& s,
                                                bool& exact) const
{
    exact = s.promptCol >= 0;
    if (exact)
    {
        const std::string cmd = LiftCommandText(s);
        if (!cmd.empty())
            return { cmd };
        exact = false;
    }
    // Fallback: the cursor's row, up to the cursor. Where the prompt ends is
    // now a guess, and a wrong guess in either direction is a safety failure:
    // cut too little and the analyser sees "C:\Users\me>rm" as the program;
    // cut too much and a redirect swallows the command.
    //
    // So do not guess once. Produce every plausible reading and let the caller
    // take the WORST — over-reporting on this path is the safe direction, and
    // whichever reading raised the alarm is the text the dialog shows.
    const Grid& g = s.grid;
    const int row = std::max(0, g.CurY());
    std::string line;
    for (int c = 0; c < std::min(g.CurX(), g.Cols()); ++c)
    {
        const Cell& cell = g.ViewCell(row, c);
        if (cell.flags & CellWideTail)
            continue;
        amber::AppendClusterUtf8(line, (cell.cp == 0) ? U' ' : cell.cp);
    }
    std::vector<std::string> out;
    if (line.find_first_not_of(" \t") == std::string::npos)
        return out;
    out.push_back(line);
    // "user@host:~$ " and the like — an unambiguous prompt ending.
    static const char* kEnds[] = { "$ ", "# ", "% ", "> " };
    size_t cut = 0;
    for (const char* e : kEnds)
    {
        const size_t at = line.rfind(e);
        if (at != std::string::npos && at + 2 > cut)
            cut = at + 2;
    }
    if (cut > 0 && cut < line.size())
        out.push_back(line.substr(cut));
    // "C:\Users\me>" — no trailing space, which is what cmd.exe and a good
    // many custom prompts do.
    const size_t bare = line.find_last_of("$#%>");
    if (bare != std::string::npos && bare + 1 < line.size())
    {
        size_t at = bare + 1;
        while (at < line.size() && line[at] == ' ')
            ++at;
        if (at < line.size())
            out.push_back(line.substr(at));
    }
    return out;
}

// Analyses what is about to be sent and asks for confirmation when the policy
// says so. Returns false when the user declined; the caller then sends
// nothing, leaving the typed line exactly where it was.
bool App::RiskCheck(const std::string& bytes)
{
    if (m_riskPolicy == amber::RiskPolicy::Off || !HasSession())
        return true;
    const amber::Session& F = Foc();
    if (F.diagnostic)
        return true;
    // Only a submission is analysed. A keystroke that does not end a line
    // cannot run anything.
    if (bytes.find('\r') == std::string::npos &&
        bytes.find('\n') == std::string::npos)
        return true;

    // Two sources. A paste, a snippet or a block rerun carries its own text,
    // so each submitted line in it is analysed. A bare Enter submits what is
    // already on the screen.
    std::vector<std::string> cmds;
    bool exact = true;
    bool typed = false;
    for (char c : bytes)
        if (static_cast<unsigned char>(c) >= 0x20)
            typed = true;
    if (typed)
    {
        std::string cur;
        for (char c : bytes)
        {
            if (c == '\r' || c == '\n')
            {
                if (!cur.empty())
                    cmds.push_back(cur);
                cur.clear();
            }
            else
                cur.push_back(c);
        }
        // A trailing fragment with no newline is not submitted yet.
    }
    else
    {
        bool ex = false;
        cmds = CommandAboutToRun(F, ex);
        exact = ex;
    }
    if (cmds.empty())
        return true;

    // The worst line in the batch is what the decision is about.
    amber::RiskReport worst;
    for (const std::string& c : cmds)
    {
        amber::RiskReport r = amber::AnalyseCommand(c);
        if (!r.Flagged(m_riskPolicy))
            continue;
        if (static_cast<int>(r.level) > static_cast<int>(worst.level))
            worst = std::move(r);
    }
    if (worst.level == amber::RiskLevel::None)
        return true;
    if (!exact)
    {
        // Say where the text came from rather than presenting a guess as the
        // command. The parser is exact; the recovery of the line is not.
        worst.incomplete = true;
        worst.incompleteWhy =
            "the command was read back from the screen because this shell does "
            "not report prompt marks (OSC 133), so the prompt may not have been "
            "trimmed correctly";
    }

    const amber::ConfirmStyle style = amber::ConfirmFor(worst.level, m_riskPolicy);
    if (style == amber::ConfirmStyle::None)
        return true;
    if (style == amber::ConfirmStyle::Notice)
    {
        SetStatus(worst.Headline(), 6.0);
        return true;
    }
    // What has to be typed back is the machine, because the question the
    // friction is really asking is "do you know where you are". A local
    // session has no remote host, so it is this computer.
    std::string host = F.profile.host;
    if (host.empty())
    {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = L"";
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(name, &n) && n)
            host = Utf8FromWide(name);
    }
    if (host.empty())
        host = "confirm";
    return amber::ShowRiskDialog(m_hwnd, worst, style, host);
}

int App::FoldSummaryAtPx(int px, int py) const
{
    if (!HasSession())
        return -1;
    const amber::Session& s = Foc();
    if (s.rowMap.empty())
        return -1;
    int col = static_cast<int>((px - m_gm.originX) / m_gm.cellW);
    int row = static_cast<int>((py - m_gm.originY) / m_gm.cellH);
    if (col < 0 || col >= static_cast<int>(m_gm.cols) || row < 0 ||
        row >= static_cast<int>(m_gm.rows))
        return -1;
    int co = 0, ro = 0;
    PaneOffset(const_cast<amber::Session&>(s), co, ro);
    row -= ro;
    if (row < 0 || row >= static_cast<int>(s.rowMap.size()))
        return -1;
    return s.rowMap[static_cast<size_t>(row)].blockIndex;
}

// Scrolls the view so `rowId` is on screen, expanding the block that hides it
// if one does. Returns false when the row has been trimmed away.
//
// The expansion is the whole point. Search reads the raw grid, so it has
// always FOUND text inside a collapsed block — but it then computed a view
// row from the grid position and ignored the fold map, landing the selection
// on whatever happened to be drawn there. A hidden match was reported as
// found and then shown somewhere else entirely.
bool App::RevealRow(amber::Session& s, uint64_t rowId, bool* expandedOut)
{
    if (expandedOut)
        *expandedOut = false;
    const int idx = AbsIndexForRow(s, rowId);
    if (idx < 0)
        return false;
    for (amber::CommandBlock& b : s.blocks)
    {
        if (!b.collapsed || !b.hasOutput)
            continue;
        if (rowId < b.outputFirst || rowId > b.outputLast)
            continue;
        b.collapsed = false;
        if (expandedOut)
            *expandedOut = true;
    }
    BuildFoldMap(s);
    Grid& g = s.grid;
    const int sb = g.ScrollbackSize();
    g.SnapView();
    const int back = std::clamp(sb - idx + g.Rows() / 2, 0, sb);
    g.SetView(back);
    return true;
}

// Steps between bookmarked blocks, oldest to newest.
void App::JumpToBookmark(int dir)
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    std::vector<uint64_t> marks;
    for (const amber::CommandBlock& b : s.blocks)
        if (b.bookmarked)
            marks.push_back(b.promptRow);
    if (marks.empty())
    {
        SetStatus("No bookmarked commands in this session.", 5.0);
        return;
    }
    const Grid& g = s.grid;
    const uint64_t here = g.TotalPushed() - static_cast<uint64_t>(g.ViewOffset());
    uint64_t target = marks.front();
    if (dir > 0)
    {
        target = marks.front();
        for (uint64_t m : marks)
            if (m > here) { target = m; break; }
    }
    else
    {
        target = marks.back();
        for (size_t i = marks.size(); i-- > 0;)
            if (marks[i] < here) { target = marks[i]; break; }
    }
    if (!RevealRow(s, target))
    {
        SetStatus("That bookmark's rows have scrolled out of the buffer.", 5.0);
        return;
    }
    SetStatus("Bookmark " + std::to_string(marks.size()) + " in this session", 3.0);
}

// ---------------------------------------------------------- block actions
// Everything a command block can do. Each reads the canonical grid for its
// text; no action writes into the terminal, and the two that could send
// something to the shell are deliberately split so the safe one is the
// default.
void App::BlockAction(int cmd, uint64_t blockId)
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    // A named block wins: the right-click menu knows exactly which one the
    // user pointed at, and falling back to the cursor would act on a
    // different command than the one they clicked.
    amber::CommandBlock* b = nullptr;
    if (blockId != 0)
        b = const_cast<amber::CommandBlock*>(amber::BlockById(s.blocks, blockId));
    if (!b)
        b = BlockAtCursor(s);
    if (!b)
    {
        SetStatus("No command blocks yet — this needs shell integration "
                  "(OSC 133) on the remote host.", 6.0);
        return;
    }
    switch (cmd)
    {
    case IdmBlockCopyCommand:
        if (b->command.empty())
        {
            SetStatus("That block has no command text.", 4.0);
            return;
        }
        SetClipboardText(b->command);
        SetStatus("Copied the command.");
        return;

    case IdmBlockCopyOutput:
    {
        const std::string out = BlockOutputText(s, *b);
        if (out.empty())
        {
            SetStatus("That command produced no output still in the scrollback.", 5.0);
            return;
        }
        SetClipboardText(out);
        SetStatus("Copied " + std::to_string(b->lines) + " lines of output.");
        return;
    }

    case IdmBlockCopyBoth:
    {
        std::string all = b->command;
        if (!all.empty())
            all += "\n";
        all += BlockOutputText(s, *b);
        if (all.empty())
        {
            SetStatus("Nothing to copy from that block.", 4.0);
            return;
        }
        SetClipboardText(all);
        SetStatus("Copied the command and its output.");
        return;
    }

    case IdmBlockFold:
        if (!b->hasOutput)
        {
            SetStatus("That command produced no output to fold.", 4.0);
            return;
        }
        b->collapsed = !b->collapsed;
        SetStatus(b->collapsed ? "Folded output" : "Expanded output");
        return;

    case IdmBlockSnippet:
        SaveBlockSnippet(*b);
        return;

    case IdmBlockBookmark:
        b->bookmarked = !b->bookmarked;
        FinishBlockSummary(s, *b);
        SetStatus(b->bookmarked
                      ? "Bookmarked. Bookmarks last as long as the rows they "
                        "point at — they are not saved."
                      : "Bookmark removed.",
                  6.0);
        return;

    case IdmBlockSearch:
        SearchWithinBlock(s, *b);
        return;

    case IdmBlockRerun:
    case IdmBlockRerunNow:
    {
        if (b->command.empty())
        {
            SetStatus("That block has no command text to run.", 4.0);
            return;
        }
        if (!s.Live())
        {
            SetStatus("This session is not connected.", 4.0);
            return;
        }
        // Type it, do not run it. A command recalled by accident has to be
        // readable and editable before it can do anything — the same rule the
        // journal already follows. Running is a separate, deliberate item.
        std::string text = b->command;
        if (cmd == IdmBlockRerunNow)
            text += "\r";
        SendToShell(text);
        SetStatus(cmd == IdmBlockRerunNow ? "Ran the command."
                                          : "Typed the command — press Enter to run it.");
        return;
    }

    case IdmBlockNotify:
        if (!s.cmdRunning)
        {
            SetStatus("Nothing is running in this session.", 4.0);
            return;
        }
        s.notifyRunning = !s.notifyRunning;
        SetStatus(s.notifyRunning
                      ? "Will report when this command finishes."
                      : "Will not report when this command finishes.");
        return;

    case IdmBlockPrevBookmark:
    case IdmBlockNextBookmark:
        JumpToBookmark(cmd == IdmBlockNextBookmark ? 1 : -1);
        return;

    default:
        return;
    }
}

// Appends the block's command to snippets.txt under a name the user picks.
void App::SaveBlockSnippet(const amber::CommandBlock& b)
{
    if (b.command.empty())
    {
        SetStatus("That block has no command text to save.", 4.0);
        return;
    }
    // A snippet line is "name = command", so neither half may carry a newline
    // and the name may not contain the separator.
    std::string name = b.command.substr(0, 40);
    if (!PromptText("Save snippet as", name) || name.empty())
        return;
    for (char& c : name)
        if (c == '=' || c == '\r' || c == '\n')
            c = ' ';
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    if (name.empty())
    {
        SetStatus("A snippet needs a name.", 4.0);
        return;
    }
    std::string text = b.command;
    for (char& c : text)
        if (c == '\r' || c == '\n')
            c = ' ';

    const std::filesystem::path p = amber::DataRoot() / "snippets.txt";
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    FILE* f = _wfopen(p.wstring().c_str(), L"ab");
    if (!f)
    {
        SetStatus("Could not write snippets.txt.", 5.0);
        return;
    }
    const std::string line = name + " = " + text + "\n";
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
    LoadSnippets();
    SetStatus("Saved snippet \"" + name + "\".");
}

// Search restricted to one block's output rows. Reports where it landed
// rather than moving the global search cursor, so F3 still means what it did.
void App::SearchWithinBlock(amber::Session& s, const amber::CommandBlock& b)
{
    if (!b.hasOutput)
    {
        SetStatus("That command produced no output to search.", 4.0);
        return;
    }
    std::string term = m_searchTerm;
    if (!PromptText("Search inside this command's output", term) || term.empty())
        return;
    m_searchTerm = term;
    auto lower = [](std::string t) {
        for (char& c : t)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    const std::string needle = lower(term);
    int hits = 0;
    uint64_t firstHit = 0;
    for (uint64_t r = b.outputFirst; r <= b.outputLast; ++r)
    {
        if (lower(RowTextRaw(s, r)).find(needle) == std::string::npos)
            continue;
        if (hits++ == 0)
            firstHit = r;
    }
    if (hits == 0)
    {
        SetStatus("\"" + term + "\" is not in that command's output.", 5.0);
        return;
    }
    RevealRow(s, firstHit);
    SetStatus("\"" + term + "\": " + std::to_string(hits) + " line" +
                  (hits == 1 ? "" : "s") + " in this block — F3 for the next "
                  "match anywhere",
              6.0);
}

// The block the cursor sits in — or, when the view is scrolled back, the one
// under the top of the view — else the most recent one with output. Returns
// nullptr only when there are no blocks at all.
amber::CommandBlock* App::BlockAtCursor(amber::Session& s)
{
    if (s.blocks.empty())
        return nullptr;
    const Grid& g = s.grid;
    // Scrolled back: the user is looking at history, so the block they mean
    // is the one on screen, not the one at the live cursor.
    const uint64_t probe =
        g.ViewOffset() > 0
            ? g.TotalPushed() - static_cast<uint64_t>(g.ViewOffset()) +
                  static_cast<uint64_t>(std::max(0, g.Rows() / 2))
            : g.TotalPushed() + static_cast<uint64_t>(std::max(0, g.CurY()));
    if (amber::CommandBlock* b = amber::BlockAtRow(s.blocks, probe))
        return b;
    for (size_t i = s.blocks.size(); i-- > 0;)
        if (s.blocks[i].hasOutput)
            return &s.blocks[i];
    return &s.blocks.back();
}

// Re-renders every summary, after a setting that changes what they say.
void App::RebuildBlockSummaries(amber::Session& s)
{
    for (amber::CommandBlock& b : s.blocks)
        if (!b.running)
            FinishBlockSummary(s, b);
}

void App::FoldAll(bool collapsed)
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    if (s.blocks.empty())
    {
        SetStatus("Nothing to fold yet — this needs shell integration "
                  "(OSC 133) on the remote host", 5.0);
        return;
    }
    int n = 0;
    for (amber::CommandBlock& f : s.blocks)
    {
        // A block that printed nothing has no rows to hide; collapsing it
        // would put a summary row where the next prompt belongs.
        if (!f.hasOutput)
            continue;
        if (f.collapsed != collapsed)
            ++n;
        f.collapsed = collapsed;
    }
    if (collapsed)
        s.grid.SnapView();
    SetStatus(collapsed ? "Folded " + std::to_string(n) + " command outputs"
                        : "Expanded " + std::to_string(n) + " command outputs");
}

void App::ToggleFoldAtCursor()
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    if (s.blocks.empty())
    {
        SetStatus("Nothing to fold yet — this needs shell integration "
                  "(OSC 133) on the remote host", 5.0);
        return;
    }
    amber::CommandBlock* pick = BlockAtCursor(s);
    if (!pick || !pick->hasOutput)
    {
        SetStatus("That command produced no output to fold.", 4.0);
        return;
    }
    pick->collapsed = !pick->collapsed;
    SetStatus(pick->collapsed ? "Folded output" : "Expanded output");
}

void App::JumpToMark(int dir)
{
    if (!HasSession())
        return;
    amber::Session& s = Foc();
    const Grid& g = s.grid;
    // Prompt marks only: 'C' marks sit at the start of output, and landing
    // there would hide the command you were looking for.
    std::vector<uint64_t> prompts;
    for (const amber::Session::Mark& m : s.marks)
        if (m.kind == 'A')
            prompts.push_back(m.rowId);
    if (prompts.empty())
    {
        SetStatus("No command marks yet — this needs shell integration "
                  "(OSC 133) on the remote host", 5.0);
        return;
    }
    std::sort(prompts.begin(), prompts.end());
    // The row currently at the top of the view, in absolute terms.
    const uint64_t top = g.TotalPushed() -
                         static_cast<uint64_t>(g.ViewOffset());
    uint64_t target = 0;
    bool found = false;
    if (dir < 0)
    {
        for (auto it = prompts.rbegin(); it != prompts.rend(); ++it)
            if (*it + 1 < top) { target = *it; found = true; break; }
    }
    else
    {
        for (uint64_t r : prompts)
            if (r > top) { target = r; found = true; break; }
    }
    if (!found)
    {
        if (dir > 0)
        {
            s.grid.SnapView();
            SetStatus("Latest command");
        }
        else
            SetStatus("Oldest command mark");
        return;
    }
    // Land the prompt one row down from the top so it has a little air.
    const int64_t back = static_cast<int64_t>(g.TotalPushed()) -
                         static_cast<int64_t>(target) + 1;
    s.grid.SetView(static_cast<int>(std::max<int64_t>(0, back)));
    std::string what = RowText(s, target);
    if (what.size() > 90)
        what = what.substr(0, 90) + "\xE2\x80\xA6";
    SetStatus(what.empty() ? "Command mark" : what, 4.0);
}

// Per-host motion rule, the same shape as the per-host theme rule:
// "prod*=14,*.dev=7". First match wins; -1 when nothing matches.
int App::MotionForHost(const std::string& host) const
{
    if (m_hostMotion.empty() || host.empty())
        return -1;
    auto match = [](const std::string& pat, const std::string& s) {
        size_t p = 0, i = 0, star = std::string::npos, mark = 0;
        while (i < s.size())
        {
            if (p < pat.size() && (pat[p] == '?' ||
                                   tolower((unsigned char)pat[p]) ==
                                       tolower((unsigned char)s[i])))
            { ++p; ++i; }
            else if (p < pat.size() && pat[p] == '*')
            { star = p++; mark = i; }
            else if (star != std::string::npos)
            { p = star + 1; i = ++mark; }
            else
                return false;
        }
        while (p < pat.size() && pat[p] == '*')
            ++p;
        return p == pat.size();
    };
    size_t start = 0;
    while (start < m_hostMotion.size())
    {
        size_t comma = m_hostMotion.find(',', start);
        std::string rule = m_hostMotion.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        start = (comma == std::string::npos) ? m_hostMotion.size() : comma + 1;
        size_t eq = rule.find('=');
        if (eq == std::string::npos)
            continue;
        std::string pat = rule.substr(0, eq);
        while (!pat.empty() && pat.front() == ' ')
            pat.erase(pat.begin());
        while (!pat.empty() && pat.back() == ' ')
            pat.pop_back();
        int style = atoi(rule.c_str() + eq + 1);
        if (style >= 0 && style < kMotionStyleCount && match(pat, host))
            return style;
    }
    return -1;
}

// Night shift: how far the phosphor has drifted towards ember, 0..1. Local
// clock rather than a location lookup — the point is the feel of the room
// after dark, and asking for a location to compute a sunset would be a poor
// trade for that.
float App::NightShiftAmount() const
{
    if (m_nightShift == 0)
        return 0.0f;
    if (m_nightShift == 2)
        return 1.0f;
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    const float hour = static_cast<float>(lt.tm_hour) +
                       static_cast<float>(lt.tm_min) / 60.0f;
    // Ramps up between 18:00 and 21:00, holds overnight, clears by 07:00.
    if (hour >= 21.0f || hour < 6.0f)
        return 1.0f;
    if (hour >= 18.0f)
        return (hour - 18.0f) / 3.0f;
    if (hour < 7.0f)
        return 1.0f - (hour - 6.0f);
    return 0.0f;
}

void App::UpdateFieldEffects()
{
    ParticleTunables& tun = m_particles.tun;

    // ---- depth parallax: the pointer tilts the field ---------------------
    float wantX = 0.0f, wantY = 0.0f;
    if (m_fxParallax && !m_minimized)
    {
        const float W = static_cast<float>(m_device.Width());
        const float H = static_cast<float>(m_device.Height());
        if (W > 1.0f && H > 1.0f)
        {
            const float dpi = static_cast<float>(m_dpi) / 96.0f;
            const float nx = std::clamp(m_lastMousePx / W, 0.0f, 1.0f) - 0.5f;
            const float ny = std::clamp(m_lastMousePy / H, 0.0f, 1.0f) - 0.5f;
            wantX = -nx * 16.0f * dpi;
            wantY = -ny * 9.0f * dpi;
        }
    }
    // Eased, so the field leans rather than snapping to the pointer.
    const float ease = 1.0f - std::exp(-m_dt * 6.0f);
    tun.tiltX += (wantX - tun.tiltX) * ease;
    tun.tiltY += (wantY - tun.tiltY) * ease;

    // ---- window slosh: an impulse when the frame moves -------------------
    tun.sloshX = 0.0f;
    tun.sloshY = 0.0f;
    if (m_hwnd)
    {
        RECT r{};
        GetWindowRect(m_hwnd, &r);
        if (m_lastWindowRect.right != 0 || m_lastWindowRect.bottom != 0)
        {
            const float dx = static_cast<float>(r.left - m_lastWindowRect.left);
            const float dy = static_cast<float>(r.top - m_lastWindowRect.top);
            if (m_fxSlosh && !m_minimized && (dx != 0.0f || dy != 0.0f))
            {
                // Clamped: a throw across two monitors should lag, not turn
                // the screen inside out.
                tun.sloshX = std::clamp(dx, -90.0f, 90.0f);
                tun.sloshY = std::clamp(dy, -90.0f, 90.0f);
            }
        }
        m_lastWindowRect = r;
    }

    // ---- phosphor warm-up ------------------------------------------------
    if (!m_fxWarmup || !HasSession())
        tun.warmup = 1.0f;
    else
    {
        const double age = m_time - Cur().bornAt;
        tun.warmup = static_cast<float>(std::clamp(age / 2.6, 0.0, 1.0));
    }

    // ---- the active motion style ----------------------------------------
    // A per-host rule wins over the global choice, so production can be given
    // a style that does not look like staging.
    uint32_t style = static_cast<uint32_t>(m_motionStyle);
    if (HasSession())
    {
        const int perHost = MotionForHost(Cur().profile.host);
        if (perHost >= 0)
            style = static_cast<uint32_t>(perHost);
    }
    tun.animStyle = style;
    tun.departStyle = static_cast<uint32_t>(m_departStyle);
    tun.effectSpeed = CurrentSpeed() * m_timeDial;
}

void App::UpdateTaskbarProgress()
{
    if (!m_hwnd)
        return;
    amber::TaskbarState want = amber::TaskbarState::None;
    int pct = -1;
    if (m_time < m_tbErrorUntil)
    {
        want = amber::TaskbarState::Error;
        pct = 100;
    }
    else if (HasSession() && Cur().cmdRunning)
    {
        want = amber::TaskbarState::Indeterminate;
        // A progress percentage redrawn in place (apt, pip, rsync, curl,
        // docker all do this with a carriage return) turns the shimmer into a
        // real bar. The cursor row is where that line lives.
        const Grid& g = Cur().grid;
        const int row = std::clamp(g.CurY(), 0, std::max(0, g.Rows() - 1));
        std::string line;
        for (int c = 0; c < g.Cols(); ++c)
        {
            char32_t cp = g.AbsCell(g.ScrollbackSize() + row, c).cp;
            line.push_back((cp >= 32 && cp < 127) ? static_cast<char>(cp) : ' ');
        }
        // Take the LAST percentage on the line: progress tools put the figure
        // after the bar, and an earlier number is usually a size or a count.
        for (size_t i = line.size(); i-- > 0;)
        {
            if (line[i] != '%')
                continue;
            size_t end = i;
            size_t start = i;
            while (start > 0 && isdigit(static_cast<unsigned char>(line[start - 1])))
                --start;
            if (start == end)
                continue;
            int v = atoi(line.substr(start, end - start).c_str());
            if (v >= 0 && v <= 100)
            {
                pct = v;
                want = amber::TaskbarState::Normal;
            }
            break;
        }
    }
    const int stateI = static_cast<int>(want);
    if (stateI == m_tbState && pct == m_tbPct)
        return;                      // nothing changed: no COM call
    m_tbState = stateI;
    m_tbPct = pct;
    amber::TaskbarProgress(m_hwnd, want,
                           static_cast<uint64_t>(std::max(0, pct)), 100);
}

void App::SaveWorkspaceAs()
{
    if (!HasSession())
    {
        SetStatus("Open some sessions first, then save them as a workspace.");
        return;
    }
    amber::Workspace w;
    for (const auto& sp : m_sessions)
    {
        const amber::Session& s = *sp;
        if (s.diagnostic || s.profile.id.empty())
            continue;               // a diagnostic or ad-hoc tab has nothing to restore
        amber::WorkspaceTab t;
        t.profileId = s.profile.id;
        if (s.layout.Count() > 1)
        {
            // Panes in id order, so the layout string's indices line up with
            // the list on the way back in.
            const std::vector<amber::PaneId> ids = s.layout.Panes();
            std::vector<amber::PaneId> ordered = ids;
            std::sort(ordered.begin(), ordered.end());
            std::vector<amber::PaneId> saved{ 0 };
            for (amber::PaneId id : ordered)
            {
                if (id == 0)
                    continue;
                const amber::Session* p = PaneById(s, id);
                if (!p || p->profile.id.empty())
                    continue;      // an ad-hoc pane has nothing to restore
                t.paneProfileIds.push_back(p->profile.id);
                saved.push_back(id);
            }
            if (!t.paneProfileIds.empty())
            {
                t.layout = s.layout.Serialize(saved);
                for (size_t k = 0; k < saved.size(); ++k)
                {
                    if (saved[k] == s.focus)
                        t.focusPane = static_cast<int>(k);
                    const amber::Session* p = PaneById(s, saved[k]);
                    if (p && p->readOnly)
                        t.readOnlyPanes.push_back(static_cast<int>(k));
                }
                // Schema 1 still gets the first split, so an older build
                // restores something sensible rather than nothing.
                t.splitProfileId = t.paneProfileIds.front();
            }
        }
        w.tabs.push_back(std::move(t));
    }
    if (w.tabs.empty())
    {
        SetStatus("Nothing to save: a workspace stores SAVED profiles, and "
                  "none of these tabs came from one.", 6.0);
        return;
    }
    std::string name = HasSession() ? Cur().Caption() : std::string("Workspace");
    if (!PromptText("Save workspace as", name) || name.empty())
        return;
    m_workspaces.Load();
    amber::Workspace out = w;
    out.name = name;
    m_workspaces.Put(std::move(out));
    RebuildWorkspaceMenu();
    UpdateJumpList();
    SetStatus("Workspace saved: " + name + " (" +
              std::to_string(w.tabs.size()) + " tabs)", 4.0);
}

void App::OpenWorkspace(const std::string& name)
{
    m_workspaces.Load();
    const amber::Workspace* w = m_workspaces.Find(name);
    if (!w)
    {
        SetStatus("No workspace named " + name, 5.0);
        return;
    }
    // Sessions are ADDED, never swapped in: closing someone's live sessions
    // because they opened a workspace would be its own kind of disaster.
    int opened = 0;
    int panes = 0;
    for (const amber::WorkspaceTab& t : w->tabs)
    {
        if (!ConnectProfileById(t.profileId))
            continue;
        ++opened;
        amber::Session& tab = Cur();
        // Panes in the order they were saved, so the layout string's indices
        // mean what they meant. `saved` mirrors the writer's list: index 0 is
        // pane 0, then each split in turn.
        std::vector<amber::PaneId> saved{ 0 };
        for (const std::string& id : t.paneProfileIds)
        {
            const amber::ConnectionProfile* p = m_profiles.Find(id);
            if (!p)
                continue;           // the profile was deleted since the save
            // Direction is a placeholder: the layout string below decides the
            // real shape. Splitting vertically first just guarantees room.
            const amber::PaneId made = AddPane(p, true);
            if (made == amber::kNoPane)
                break;              // no room for more panes at this size
            saved.push_back(made);
            ++panes;
        }
        if (!t.layout.empty() && saved.size() > 1)
        {
            // Restore the tree only when every pane it names came back;
            // otherwise the layout would hide a live session or name one that
            // does not exist, and the panes keep the shape AddPane gave them.
            if (!tab.layout.Deserialize(t.layout, saved))
                SetStatus("Workspace: the saved pane layout no longer fits — "
                          "panes reopened side by side.", 6.0);
        }
        // Read-only comes back; broadcast deliberately does not.
        for (int idx : t.readOnlyPanes)
            if (idx >= 0 && static_cast<size_t>(idx) < saved.size())
                if (amber::Session* p = PaneById(tab, saved[static_cast<size_t>(idx)]))
                    p->readOnly = true;
        tab.broadcast.clear();
        if (t.focusPane >= 0 && static_cast<size_t>(t.focusPane) < saved.size())
            tab.focus = saved[static_cast<size_t>(t.focusPane)];
        // A zoom is never restored, per the spec: coming back to a workspace
        // with one pane filling the window and the rest invisible is a
        // surprise, not a restoration.
        tab.layout.ClearZoom();
        UpdateGridDims();
    }
    if (panes > 0)
        SetStatus("Workspace " + name + ": " + std::to_string(opened) +
                      " tabs, " + std::to_string(panes) + " extra panes. "
                      "Broadcast is off.",
                  6.0);
    SetStatus("Workspace " + name + ": opened " + std::to_string(opened) +
              " of " + std::to_string(w->tabs.size()) + " sessions", 5.0);
}

void App::DeleteWorkspace(const std::string& name)
{
    m_workspaces.Load();
    if (m_workspaces.Remove(name))
    {
        RebuildWorkspaceMenu();
        UpdateJumpList();
        SetStatus("Workspace deleted: " + name);
    }
}

void App::RebuildWorkspaceMenu()
{
    if (!m_workspaceMenu)
        return;
    while (GetMenuItemCount(m_workspaceMenu) > 0)
        DeleteMenu(m_workspaceMenu, 0, MF_BYPOSITION);
    AppendMenuW(m_workspaceMenu, MF_STRING, IdmWorkspaceSave,
                L"&Save Current Sessions as Workspace...");
    m_workspaces.Load();
    m_workspaceNames.clear();
    const auto& all = m_workspaces.All();
    if (!all.empty())
        AppendMenuW(m_workspaceMenu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < all.size() && i < 24; ++i)
    {
        m_workspaceNames.push_back(all[i].name);
        std::wstring label = WideFromUtf8(all[i].name) + L"  (" +
                             std::to_wstring(all[i].tabs.size()) + L")";
        AppendMenuW(m_workspaceMenu, MF_STRING,
                    IdmWorkspaceFirst + static_cast<int>(i), label.c_str());
    }
    if (!all.empty())
    {
        AppendMenuW(m_workspaceMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_workspaceMenu, MF_STRING, IdmWorkspaceDelete,
                    L"&Delete a Workspace...");
    }
    ThemeMenuBar(m_workspaceMenu, 1);
}

void App::OpenSftpPanel()
{
    if (!HasSession() || Foc().diagnostic ||
        Foc().state != amber::SessionState::Connected)
    {
        SetStatus("Connect a session first, then open the SFTP browser.");
        return;
    }
    amber::Session& F = Foc();
    amber::SftpBrowser::OpenTab(m_hwnd, F.profile, F.savedPassword,
                                F.savedPassphrase, F.cwd);
}

void App::ImportProfiles()
{
    int added = 0;
    auto haveName = [&](const std::string& name)
    {
        for (const auto& p : m_profiles.All())
            if (p.name == name)
                return true;
        return false;
    };

    // ---- OpenSSH ~/.ssh/config ------------------------------------------
    wchar_t prof[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    std::wstring cfgPath = std::wstring(prof) + L"\\.ssh\\config";
    if (FILE* f = _wfopen(cfgPath.c_str(), L"rb"))
    {
        std::string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            data.append(buf, n);
        fclose(f);

        amber::ConnectionProfile cur;
        bool open = false;
        auto flush = [&]()
        {
            if (open && !cur.name.empty() &&
                cur.name.find('*') == std::string::npos &&
                cur.name.find('?') == std::string::npos && !haveName(cur.name))
            {
                if (cur.host.empty())
                    cur.host = cur.name;
                cur.id = amber::MakeUuid();
                if (!cur.privateKeyPath.empty())
                    cur.auth = amber::AuthMethod::PublicKey;
                m_profiles.Upsert(cur);
                ++added;
            }
            cur = amber::ConnectionProfile{};
            open = false;
        };
        size_t lineStart = 0;
        while (lineStart <= data.size())
        {
            size_t nl = data.find('\n', lineStart);
            std::string line = data.substr(
                lineStart, nl == std::string::npos ? std::string::npos
                                                   : nl - lineStart);
            lineStart = (nl == std::string::npos) ? data.size() + 1 : nl + 1;
            // trim + split "Key Value"
            size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string::npos || line[b] == '#')
                continue;
            size_t sp = line.find_first_of(" \t=", b);
            if (sp == std::string::npos)
                continue;
            std::string key = line.substr(b, sp - b);
            size_t vb = line.find_first_not_of(" \t=", sp);
            if (vb == std::string::npos)
                continue;
            std::string val = line.substr(vb);
            while (!val.empty() && (val.back() == '\r' || val.back() == ' '))
                val.pop_back();
            for (char& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == "host")
            {
                flush();
                open = true;
                cur.name = val.substr(0, val.find(' '));
            }
            else if (key == "hostname")   cur.host = val;
            else if (key == "user")       cur.username = val;
            else if (key == "port")       cur.port = std::max(1, atoi(val.c_str()));
            else if (key == "identityfile")
            {
                if (val.rfind("~/", 0) == 0)
                    val = Utf8FromWide(prof) + "\\" + val.substr(2);
                cur.privateKeyPath = val;
            }
            else if (key == "proxyjump")  cur.jumpHost = val;
        }
        flush();
    }

    // ---- PuTTY registry sessions -----------------------------------------
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\SimonTatham\\PuTTY\\Sessions", 0, KEY_READ,
                      &hk) == ERROR_SUCCESS)
    {
        for (DWORD i = 0;; ++i)
        {
            wchar_t keyName[256];
            DWORD kl = 256;
            if (RegEnumKeyExW(hk, i, keyName, &kl, nullptr, nullptr, nullptr,
                              nullptr) != ERROR_SUCCESS)
                break;
            HKEY sk = nullptr;
            if (RegOpenKeyExW(hk, keyName, 0, KEY_READ, &sk) != ERROR_SUCCESS)
                continue;
            auto readSz = [&](const wchar_t* v) -> std::string
            {
                wchar_t b2[512] = L"";
                DWORD cb = sizeof(b2), ty = 0;
                if (RegQueryValueExW(sk, v, nullptr, &ty,
                                     reinterpret_cast<LPBYTE>(b2), &cb) ==
                        ERROR_SUCCESS && ty == REG_SZ)
                    return Utf8FromWide(b2);
                return {};
            };
            auto readDw = [&](const wchar_t* v, DWORD def) -> DWORD
            {
                DWORD d = def, cb = sizeof(d), ty = 0;
                if (RegQueryValueExW(sk, v, nullptr, &ty,
                                     reinterpret_cast<LPBYTE>(&d), &cb) ==
                        ERROR_SUCCESS && ty == REG_DWORD)
                    return d;
                return def;
            };
            // PuTTY URL-encodes session names (%20 and friends).
            std::string name = Utf8FromWide(keyName);
            std::string decoded;
            for (size_t k = 0; k < name.size(); ++k)
            {
                if (name[k] == '%' && k + 2 < name.size())
                {
                    decoded.push_back(static_cast<char>(
                        strtol(name.substr(k + 1, 2).c_str(), nullptr, 16)));
                    k += 2;
                }
                else
                    decoded.push_back(name[k]);
            }
            std::string host = readSz(L"HostName");
            if (!host.empty() && decoded != "Default Settings" &&
                !haveName(decoded))
            {
                amber::ConnectionProfile p;
                p.id = amber::MakeUuid();
                p.name = decoded;
                p.host = host;
                p.port = static_cast<int>(readDw(L"PortNumber", 22));
                p.username = readSz(L"UserName");
                std::string ppk = readSz(L"PublicKeyFile");
                if (!ppk.empty())
                {
                    p.privateKeyPath = ppk;   // .ppk needs converting; noted
                    p.auth = amber::AuthMethod::PublicKey;
                }
                m_profiles.Upsert(p);
                ++added;
            }
            RegCloseKey(sk);
        }
        RegCloseKey(hk);
    }

    std::string err;
    m_profiles.Save(&err);
    SetStatus("Imported " + std::to_string(added) +
                  " profiles (PuTTY .ppk keys must be converted to OpenSSH).",
              6.0);
}

void App::DownloadFont(int faceIndex)
{
    // Curated open-source download sources (jsDelivr mirrors of google/fonts
    // and the Hack repo). Fonts absent here are system-only (e.g. Consolas,
    // OCR A Extended) or too large to fetch (Iosevka).
    static const std::unordered_map<std::wstring, const wchar_t*> kUrls = {
        { L"JetBrains Mono",  L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf" },
        { L"Fira Code",       L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/firacode/FiraCode%5Bwght%5D.ttf" },
        { L"Hack",            L"https://cdn.jsdelivr.net/gh/source-foundry/Hack@master/build/ttf/Hack-Regular.ttf" },
        { L"Source Code Pro", L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/sourcecodepro/SourceCodePro%5Bwght%5D.ttf" },
        { L"IBM Plex Mono",   L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/ibmplexmono/IBMPlexMono-Regular.ttf" },
        { L"Space Mono",      L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/spacemono/SpaceMono-Regular.ttf" },
        { L"Ubuntu Mono",     L"https://cdn.jsdelivr.net/gh/google/fonts@main/ufl/ubuntumono/UbuntuMono-Regular.ttf" },
        { L"Share Tech Mono", L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/sharetechmono/ShareTechMono-Regular.ttf" },
        { L"Syne Mono",       L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/synemono/SyneMono-Regular.ttf" },
        { L"Orbitron",        L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/orbitron/Orbitron%5Bwght%5D.ttf" },
        { L"Michroma",        L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/michroma/Michroma-Regular.ttf" },
    };
    std::wstring family = kFontFaces[faceIndex];
    auto it = kUrls.find(family);
    if (it == kUrls.end())
    {
        SetStatus(Utf8FromWide(family) +
                  " is not installed and has no download source.", 6.0);
        return;
    }

    std::wstring dest;
    try
    {
        std::filesystem::path dir = amber::DataRoot() / "fonts";
        std::filesystem::create_directories(dir);
        std::wstring fname = family;
        fname.erase(std::remove(fname.begin(), fname.end(), L' '), fname.end());
        dest = (dir / (fname + L".ttf")).wstring();
    }
    catch (...)
    {
        SetStatus("Could not create the font cache directory.", 5.0);
        return;
    }

    SetStatus("Downloading " + Utf8FromWide(family) + "…", 30.0);
    HWND hwnd = m_hwnd;
    std::wstring url = it->second;
    // Fetch off the UI thread; the window is notified on completion.
    std::thread([hwnd, url, dest, faceIndex]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0,
                                        nullptr);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_FONT_READY, static_cast<WPARAM>(faceIndex),
                     hr == S_OK ? 1 : 0);
    }).detach();
}


// ---------------------------------------------------------- chrome typeface
// A skin may WANT a boutique face (Jost, EB Garamond…) that is not installed
// on most machines. The rule: letter in the wanted face when it is bundled or
// installed, fall back to the skin's installed face otherwise, and fetch the
// wanted one in the background so it is there next time — the same deal the
// terminal faces get.
void App::RegisterGdiFonts()
{
    // DirectWrite sees the bundled fonts through the private collection; GDI
    // (the dialogs, the menu) needs them registered as private resources.
    for (const std::wstring& dir : m_fontDirs)
    {
        try
        {
            for (const auto& e : std::filesystem::directory_iterator(dir))
            {
                if (!e.is_regular_file())
                    continue;
                std::wstring ext = e.path().extension().wstring();
                for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
                if (ext == L".ttf" || ext == L".otf")
                    AddFontResourceExW(e.path().c_str(), FR_PRIVATE, nullptr);
            }
        }
        catch (...) {}
    }
}

void App::ApplyChromeFace()
{
    // The dialogs, the menu and the About panel letter in the wanted face
    // when it is bundled or installed, else in the skin's installed face;
    // the wanted one is fetched in the background so it is there next time.
    const amber::ChromeSpec& ch = amber::Chrome();
    const wchar_t* use = ch.uiFont;
    if (amber::ChromeSkinned() && ch.uiFontWanted)
    {
        if (m_sampler.HasFamily(ch.uiFontWanted))
            use = ch.uiFontWanted;
        else
            DownloadFontFamily(ch.uiFontWanted);
    }
    amber::SetChromeFace(amber::ChromeSkinned() ? use : nullptr);
    if (!m_chromeReady)
        return;
    // The strip letters in the same face through its own atlas. A GDI named
    // instance ("Bahnschrift Light") is a DirectWrite family plus a weight,
    // so the bare family is tried when the full name does not resolve.
    std::wstring want = use ? use : m_sampler.FontName();
    bool ok = m_chromeSampler.SetFontFamily(want);
    if (!ok)
    {
        std::wstring fam = want;
        for (const wchar_t* w : { L" SemiCondensed", L" Condensed", L" SemiBold", L" Light" })
        {
            const size_t p = fam.find(w);
            if (p != std::wstring::npos)
                fam.erase(p);
        }
        ok = fam != want && m_chromeSampler.SetFontFamily(fam);
    }
    if (!ok)
        m_chromeSampler.SetFontFamily(m_sampler.FontName());
    if (m_fontPx > 0.0f)
        m_chromeSampler.EnsureAtlas(m_fontPx);
}

void App::DownloadFontFamily(const std::wstring& family)
{
    static const std::unordered_map<std::wstring, const wchar_t*> kChromeUrls = {
        { L"Jost",               L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/jost/Jost%5Bwght%5D.ttf" },
        { L"EB Garamond",        L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/ebgaramond/EBGaramond%5Bwght%5D.ttf" },
        { L"Cormorant Garamond", L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/cormorantgaramond/CormorantGaramond%5Bwght%5D.ttf" },
        { L"Josefin Sans",       L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/josefinsans/JosefinSans%5Bwght%5D.ttf" },
        { L"Alegreya",           L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/alegreya/Alegreya%5Bwght%5D.ttf" },
        { L"Alegreya Sans",      L"https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/alegreyasans/AlegreyaSans-Regular.ttf" },
    };
    if (m_fontFetches.count(family))
        return;                                   // once per session
    auto it = kChromeUrls.find(family);
    if (it == kChromeUrls.end())
        return;
    std::wstring dest;
    try
    {
        std::filesystem::path dir = amber::DataRoot() / "fonts";
        std::filesystem::create_directories(dir);
        std::wstring fname = family;
        fname.erase(std::remove(fname.begin(), fname.end(), L' '), fname.end());
        dest = (dir / (fname + L".ttf")).wstring();
    }
    catch (...)
    {
        return;
    }
    m_fontFetches.insert(family);
    SetStatus("Fetching " + Utf8FromWide(family) + " for the interface…", 20.0);
    HWND hwnd = m_hwnd;
    std::wstring url = it->second;
    std::thread([hwnd, url, dest]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0, nullptr);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_FONT_READY, static_cast<WPARAM>(-2), hr == S_OK ? 1 : 0);
    }).detach();
}
void App::DownloadSymbolsFont()
{
    // Fired the first time a Private Use Area glyph (nerd-font prompt icon)
    // has no coverage anywhere: fetch the standalone Nerd Fonts symbols face
    // (~2 MB, MIT) once into the same cache the font menu downloads use.
    if (m_symbolsFetchStarted)
        return;
    m_symbolsFetchStarted = true;

    std::wstring dest;
    try
    {
        std::filesystem::path dir = amber::DataRoot() / "fonts";
        std::filesystem::create_directories(dir);
        dest = (dir / L"SymbolsNerdFontMono-Regular.ttf").wstring();
        if (std::filesystem::exists(dest))
            return;   // present but unloadable — don't refetch every run
    }
    catch (...)
    {
        return;
    }

    SetStatus("Fetching symbol font for prompt icons…", 20.0);
    HWND hwnd = m_hwnd;
    std::wstring url =
        L"https://cdn.jsdelivr.net/gh/ryanoasis/nerd-fonts@v3.2.1/"
        L"patched-fonts/NerdFontsSymbolsOnly/SymbolsNerdFontMono-Regular.ttf";
    std::thread([hwnd, url, dest]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0,
                                        nullptr);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_FONT_READY, static_cast<WPARAM>(-1),
                     hr == S_OK ? 1 : 0);
    }).detach();
}

// ----------------------------------------------------------- diagnostics
void App::StartDiagSession()
{
    auto session = std::make_unique<amber::Session>();
    session->diagnostic = true;
    session->label = "diagnostic";
    session->state = amber::SessionState::Connected;
    session->bornAt = m_time;
    session->grid.Init(m_gm.cols ? static_cast<int>(m_gm.cols) : 120,
                       m_gm.rows ? static_cast<int>(m_gm.rows) : 40);
    // The diagnostic session has no remote end, but it has a parser, so it
    // gets the same sinks as any other rather than a hand-picked subset.
    BindSessionSinks(*session);
    m_sessions.push_back(std::move(session));
    m_active = static_cast<int>(m_sessions.size()) - 1;
}

void App::FeedDiagnostic(amber::Session& s)
{
    std::string d;
    d += "\x1b[0m\r\n  AmberSSH color diagnostic\r\n\r\n";

    d += "  16 colors fg: ";
    for (int i = 0; i < 8; ++i)
        d += "\x1b[3" + std::to_string(i) + "mX" ;
    for (int i = 0; i < 8; ++i)
        d += "\x1b[9" + std::to_string(i) + "mX";
    d += "\x1b[0m   bg: ";
    for (int i = 0; i < 8; ++i)
        d += "\x1b[4" + std::to_string(i) + "m ";
    for (int i = 0; i < 8; ++i)
        d += "\x1b[10" + std::to_string(i) + "m ";
    d += "\x1b[0m\r\n\r\n  256-color cube:\r\n";
    for (int row = 0; row < 6; ++row)
    {
        d += "  ";
        for (int i = 16 + row * 36; i < 16 + (row + 1) * 36; ++i)
            d += "\x1b[48;5;" + std::to_string(i) + "m ";
        d += "\x1b[0m\r\n";
    }
    d += "  grayscale: ";
    for (int i = 232; i < 256; ++i)
        d += "\x1b[48;5;" + std::to_string(i) + "m ";
    d += "\x1b[0m\r\n\r\n  truecolor gradient:\r\n  ";
    for (int i = 0; i < 64; ++i)
    {
        int r = 255 - i * 3, g = 45 + i * 2, b = 146;
        d += "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" +
             std::to_string(b) + "m ";
    }
    d += "\x1b[0m\r\n  ";
    for (int i = 0; i < 64; ++i)
    {
        int r = 52 + i * 3, g = 20 + i, b = 95 + i * 2;
        d += "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" +
             std::to_string(b) + "m ";
    }
    d += "\x1b[0m\r\n\r\n";
    d += "  \x1b[1mbold\x1b[0m  \x1b[2mfaint\x1b[0m  \x1b[3mitalic\x1b[0m  "
         "\x1b[4munderline\x1b[0m  \x1b[21mdouble\x1b[0m  \x1b[7minverse\x1b[0m  "
         "\x1b[9mstrike\x1b[0m  \x1b[5mblink\x1b[0m  "
         "\x1b[58;2;45;226;230m\x1b[4mcolored-underline\x1b[0m\r\n\r\n";
    d += "  \x1b[38;2;255;45;146mTRUECOLOR PINK\x1b[0m  "
         "\x1b[38:2::99:230:166mCOLON GREEN\x1b[0m  "
         "\x1b[31mRED \x1b[32mGREEN \x1b[33mYELLOW \x1b[34mBLUE "
         "\x1b[35mMAGENTA \x1b[36mCYAN\x1b[0m\r\n\r\n";
    d += "  box drawing: \x1b[33m┌─┬─┐ ╔═╦═╗\x1b[0m  punctuation: .,;:!?'\"`~\r\n";
    d += "               \x1b[33m├─┼─┤ ╠═╬═╣\x1b[0m\r\n";
    d += "               \x1b[33m└─┴─┘ ╚═╩═╝\x1b[0m\r\n\r\n";
    d += "  simulated ls --color=always:\r\n";
    // Shell-integration marks: prompt (A) on the heading, output start (C)
    // on the first listing row — the tide mark hairline lands here.
    d += "\x1b]133;A\x07\x1b]133;C\x07";
    d += "  \x1b[0m-rw-r--r-- 1 josh josh  4096 notes.md\r\n";
    d += "  \x1b[01;34mdrwxr-xr-x\x1b[0m 2 josh josh  4096 \x1b[01;34msrc\x1b[0m\r\n";
    d += "  \x1b[01;32m-rwxr-xr-x\x1b[0m 1 josh josh 12288 \x1b[01;32mbuild.sh\x1b[0m\r\n";
    d += "  \x1b[01;36mlrwxrwxrwx\x1b[0m 1 josh josh    11 \x1b[01;36mlatest\x1b[0m -> notes.md\r\n";
    d += "  \x1b[01;31m-rw-r--r--\x1b[0m 1 josh josh  8192 \x1b[01;31marchive.tar.gz\x1b[0m\r\n";
    d += "\r\n  select across these lines to test the Miami Sunset gradient.\r\n";
    d += "  emoji: \xF0\x9F\x9A\x80 \xF0\x9F\x94\xA5 \xE2\x9D\xA4 \xF0\x9F\x98\x80 "
         "\xF0\x9F\x8C\x88 \xF0\x9F\x90\xA7 \xE2\x9C\x85 \xF0\x9F\x92\xBB "
         "\xF0\x9F\x8E\xAE \xF0\x9F\x8D\x95 \xE2\xAD\x90 \xF0\x9F\x9A\xA9\r\n";
    // VS16: text-default symbols forced to colour (❤️ ✔️ ☂️ ✈️ ⚠️).
    d += "  vs16 : \xE2\x9D\xA4\xEF\xB8\x8F \xE2\x9C\x94\xEF\xB8\x8F "
         "\xE2\x98\x82\xEF\xB8\x8F \xE2\x9C\x88\xEF\xB8\x8F "
         "\xE2\x9A\xA0\xEF\xB8\x8F  mono: \xE2\x9D\xA4 \xE2\x9C\x94 "
         "\xE2\x9E\x9C \xE2\x8C\x9A\r\n";
    // Double width: CJK text and the erase test — three rockets, then the
    // rightmost erased exactly as a shell would (2×BS, 2×SP, 2×BS). Two
    // rockets remaining proves width-2 cursor math matches the server's.
    d += "  wide : \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E "
         "\xEC\x95\x88\xEB\x85\x95 erase-> \xF0\x9F\x9A\x80\xF0\x9F\x9A\x80"
         "\xF0\x9F\x9A\x80\x08\x08  \x08\x08 <-two left\r\n";
    // Shell integration: a synthetic "command finished, exit 0" mark fires
    // the success edge-glow, so the effect is visible without a remote shell.
    d += "  shell integration: OSC 133;D;0 -> green edge glow\r\n"
         "\x1b]133;D;0\x07";
    // Inline image: a 24x12 two-band Sixel (orange over blue) at the cursor.
    d += "  sixel : \x1bPq#0;2;100;40;0#1;2;0;70;100#0!24~-#1!24~-\x1b\\"
         " <- inline image\r\n";
    // Ember demo: built-in error / fatal patterns smoulder (and the fatal
    // one shakes the screen for half a second).
    d += "  embers : error: build failed at step 3   <- error line smoulders\r\n";
    d += "  fatal  : Segmentation fault (core dumped)  <- screen shake + hot ember\r\n";
    // Phosphor persistence demo: a value overwritten in place — the old
    // digits ghost under the new ones for a second.
    d += "  persist: 8888888 8888888";
    for (int i = 0; i < 90; ++i)
        d += "\x1b[0m";   // ~360 paced bytes: the 8s stay on screen a few frames
    d += "\r  persist: 1234567 ABCDEFG\r\n";
    // A command that never finishes: the running-command pulse sweeps the
    // cursor row until an OSC 133;D arrives.
    d += "  running: \x1b]133;A\x07\x1b]133;C\x07";

    // Queued rather than fed directly: DrainSessionOutput paces it out at the
    // cascade rate so the diagnostic shows the materialize effects.
    s.localPending += d;
}

// -------------------------------------------------------------------- wndproc
LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                     bool& handled)
{

    handled = true;
    switch (msg)
    {
    case WM_UAHDRAWMENU:
        // Paint the themed menu-bar strip background ourselves.
        DrawMenuBarBg(reinterpret_cast<void*>(lParam));
        return TRUE;

    case WM_UAHDRAWMENUITEM:
        // Paint one top-level bar label (amber, hover-flip).
        DrawMenuBarItem(reinterpret_cast<void*>(lParam));
        return TRUE;

    case WM_NCCALCSIZE:
        if (wParam)
        {
            // Custom frame: reclaim the OS caption so our title bar fills the
            // top, but keep the resize borders (and thus DWM rounded corners +
            // shadow). Maximized insets by the frame so nothing is clipped or
            // covers the taskbar.
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            RECT req = p->rgrc[0];
            DefWindowProcW(hwnd, WM_NCCALCSIZE, wParam, lParam);
            if (IsZoomed(hwnd))
            {
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, m_dpi) +
                         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, m_dpi);
                p->rgrc[0].top = req.top + fy;
            }
            else
            {
                p->rgrc[0].top = req.top;   // no caption; borders on L/R/B stay
            }
            return 0;
        }
        handled = false;
        return 0;

    case WM_HOTKEY:
        if (wParam == 1)
            QuakeToggle();
        return 0;
    case WM_APP_TRAY:
    {
        // Clicking the toast (or the tray icon) brings the window back.
        UINT ev = LOWORD(lParam);
        if (ev == NIN_BALLOONUSERCLICK || ev == WM_LBUTTONUP ||
            ev == WM_LBUTTONDBLCLK)
        {
            if (IsIconic(m_hwnd))
                ShowWindow(m_hwnd, SW_RESTORE);
            SetForegroundWindow(m_hwnd);
        }
        return 0;
    }
    case WM_APP_FONT_READY:
    {
        int face = static_cast<int>(wParam);
        if (lParam == 0)
        {
            SetStatus("Font download failed (offline?).", 5.0);
            return 0;
        }
        if (face == -2)
        {
            // A chrome face arrived: both collections and GDI pick it up, and
            // the dialogs re-resolve on their next open.
            m_sampler.LoadAppFonts(m_fontDirs);
            if (m_chromeReady)
                m_chromeSampler.LoadAppFonts(m_fontDirs);
            RegisterGdiFonts();
            ApplyChromeFace();
            SetStatus("Interface font installed.", 4.0);
            return 0;
        }
        if (face < 0)
        {
            // Symbols fallback font (nerd-font prompt icons) arrived: reload
            // the private collection and drop cached '?' substitutions.
            m_sampler.RefreshAfterFontChange();
            UpdateFontMetrics(m_fontPx);
            SetStatus("Symbol font installed — prompt icons enabled.", 5.0);
            return 0;
        }
        m_sampler.LoadAppFonts(m_fontDirs);   // pick up the downloaded file
        if (face >= 0 && face < kFontFaceCount &&
            m_sampler.SetFontFamily(kFontFaces[face]))
        {
            m_fontFace = face;
            UpdateFontMetrics(m_fontPx);
            SetStatus("Font face: " + Utf8FromWide(kFontFaces[face]) +
                      " (downloaded)");
            SaveSettings();
            UpdateMenuChecks();
        }
        else
        {
            SetStatus("Downloaded, but the font could not be loaded.", 5.0);
        }
        return 0;
    }

    case WM_NCLBUTTONDBLCLK:
        // Double-clicking the drag strip toggles maximize (rounded work-area
        // fill), matching a normal titlebar.
        if (wParam == HTCAPTION)
        {
            ToggleMaximize();
            return 0;
        }
        handled = false;
        return 0;

    case WM_SYSCOMMAND:
    {
        // Route Aero Snap / Win+Up maximize and restore through our rounded
        // work-area fill instead of a true (square-cornered) maximize.
        UINT sc = static_cast<UINT>(wParam & 0xFFF0);
        if (sc == SC_MAXIMIZE)
        {
            if (!m_fakeMax) ToggleMaximize();
            return 0;
        }
        if (sc == SC_RESTORE && m_fakeMax)
        {
            ToggleMaximize();
            return 0;
        }
        handled = false;
        return 0;
    }

    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        int rb = MulDiv(6, static_cast<int>(m_dpi), 96);
        RECT rc;
        GetClientRect(hwnd, &rc);
        bool zoom = IsZoomed(hwnd) != 0 || m_fakeMax;
        bool t = !zoom && pt.y < rb, b = !zoom && pt.y >= rc.bottom - rb;
        bool l = !zoom && pt.x < rb, r = !zoom && pt.x >= rc.right - rb;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (t) return HTTOP;
        if (b) return HTBOTTOM;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (pt.y < static_cast<int>(m_titleBarH))
        {
            // Hamburger / +, window buttons, and the session tabs (incl. their
            // × ) take clicks (HTCLIENT); only the empty strip drags.
            bool closeHit = false;
            if (CaptionZoneAt(pt.x, pt.y) != CapZone::None ||
                TitleTabAt(pt.x, pt.y, closeHit) >= 0)
                return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_MEASUREITEM:
    {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlType == ODT_MENU)
        {
            MeasureMenuItem(*mis);
            return TRUE;
        }
        handled = false;
        return 0;
    }
    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_MENU)
        {
            DrawMenuItem(*dis);
            return TRUE;
        }
        handled = false;
        return 0;
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            m_minimized = true;
            return 0;
        }
        m_minimized = false;
        {
            uint32_t w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0 &&
                (w != m_device.Width() || h != m_device.Height()))
            {
                m_device.Resize(w, h);
                CreateSceneTarget();
                m_bloom.Resize(w, h);
                // Window page: "change the size of the font" keeps the
                // rows/cols and fits the font to the new client area.
                if (HasSession() && Cur().profile.resizeAction == amber::ResizeAction::Font &&
                    m_gm.cols > 0 && m_gm.rows > 0)
                {
                    float pad = static_cast<float>(m_gapPx) * m_dpi / 96.0f;
                    float availW = static_cast<float>(w) - 2.0f * pad;
                    float availH = static_cast<float>(h) - m_titleBarH - 2.0f * pad;
                    float fx = availW / (m_sampler.AdvanceEm() * static_cast<float>(m_gm.cols));
                    float fy = availH / (m_sampler.LineEm() * static_cast<float>(m_gm.rows));
                    UpdateFontMetrics(std::floor(std::min(fx, fy)));
                }
                else
                    UpdateGridDims();
            }
        }
        return 0;

    case WM_DPICHANGED:
    {
        uint32_t newDpi = HIWORD(wParam);
        if (newDpi && newDpi != m_dpi)
        {
            m_fontPx = m_fontPx * newDpi / m_dpi;
            m_dpi = newDpi;
        }
        const RECT* rc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateFontMetrics(m_fontPx);
        return 0;
    }

    case WM_DISPLAYCHANGE:
    case WM_EXITSIZEMOVE:
        HandleColorSpaceChange();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == 0 && HandleMenuCommand(LOWORD(wParam)))
            return 0;
        handled = false;
        return 0;

    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam), false);
        return 0;

    case WM_SYSCHAR:
        // Alt+letter → ESC-prefixed; swallow so no menu chime.
        OnChar(static_cast<wchar_t>(wParam), true);
        return 0;

    case WM_KEYDOWN:
        if (OnKeyDown(wParam))
            return 0;
        handled = false;
        return 0;

    case WM_SYSKEYDOWN:
        if (wParam == VK_F4)   // Alt+F4 closes (Behaviour page can disable it)
        {
            if (HasSession() && !Cur().profile.altF4Closes)
                return 0;
            handled = false;
            return 0;
        }
        if (OnKeyDown(wParam))
            return 0;
        handled = false;
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        // Only a desktop tab cares about releases: a terminal sends bytes on
        // the press. A key that went down on the far side must come up.
        if (VncKey(wParam, false))
            return 0;
        handled = false;
        return 0;
    case WM_LBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        if (my < static_cast<int>(m_titleBarH))
        {
            CapZone z = CaptionZoneAt(mx, my);
            if (z != CapZone::None) { CaptionClick(z); return 0; }
            bool closeHit = false;
            int ti = TitleTabAt(mx, my, closeHit);
            if (ti >= 0)
            {
                if (closeHit) CloseSession(ti);
                else          SelectTab(ti);
                return 0;
            }
            return 0;             // draggable strip (handled by HTCAPTION)
        }
        OnMouseButton(true, mx, my);
        return 0;
    }
    case WM_LBUTTONUP:
        OnMouseButton(false, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_RBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        if (my < static_cast<int>(m_titleBarH))
            return 0;
        OnMouseButton(true, mx, my, true);
        return 0;
    }
    case WM_RBUTTONUP:
        OnMouseButton(false, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true);
        return 0;
    case WM_MBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        if (my < static_cast<int>(m_titleBarH))
            return 0;
        OnMouseButton(true, mx, my, false, true);
        return 0;
    }
    case WM_MBUTTONUP:
        OnMouseButton(false, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, true);
        return 0;
    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        m_capHover = CaptionZoneAt(mx, my);
        bool closeHit = false;
        m_tabHover = TitleTabAt(mx, my, closeHit);
        if (my < static_cast<int>(m_titleBarH))
            return 0;             // over the title bar: no terminal selection
        OnMouseMove(mx, my);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (VncWheel(GET_WHEEL_DELTA_WPARAM(wParam)))
            return 0;
        OnWheel(GET_WHEEL_DELTA_WPARAM(wParam),
                (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
        return 0;

    case WM_SETFOCUS:
        m_focused = true;
        return 0;
    case WM_KILLFOCUS:
        m_focused = false;
        // whatever a desktop tab was holding down comes up: a Shift stuck on
        // the far side is the classic VNC failure
        VncReleaseAll();
        return 0;

    case WM_SIZING:
        // Window page: forbid resizing - the drag is snapped back.
        if (HasSession() && Cur().profile.resizeAction == amber::ResizeAction::Forbid)
        {
            RECT cur;
            GetWindowRect(hwnd, &cur);
            *reinterpret_cast<RECT*>(lParam) = cur;
            return TRUE;
        }
        handled = false;
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = { 480, 320 };
        // Allow programmatic sizing beyond the monitor (4K perf testing on
        // smaller displays); interactive drag is still bounded by the screen.
        mmi->ptMaxTrackSize = { 4200, 2400 };
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLIPBOARDUPDATE:
        OnWindowsClipboardChanged();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (m_clipboardListener)
            RemoveClipboardFormatListener(hwnd);
        PostQuitMessage(0);
        return 0;

    default:
        handled = false;
        return 0;
    }
}
