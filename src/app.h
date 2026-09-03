// app.h — application orchestration: frame loop, tab/session management, input
// routing, resize/HDR handling, selection & clipboard, status overlay.
//
// The connection manager, host-key confirmation and settings are native Win32
// dialogs (see src/ui/), not in-engine surfaces: the terminal viewport keeps
// exclusive ownership of the swap chain.
#pragma once

#include <set>

#include <memory>
#include <vector>

#include "common.h"
#include "dx/device.h"
#include "dx/shaders.h"
#include "glyphs/sampler.h"
#include "profiles/ProfileStore.h"
#include "render/bloom.h"
#include "render/composite.h"
#include "render/particles.h"
#include "render/prims.h"
#include "platform/AudioLevel.h"
#include "platform/Notify.h"
#include "sessions/CommandJournal.h"
#include "sessions/Session.h"
#include "sessions/Workspaces.h"
#include "ssh/Vitals.h"

#include <regex>
#include "term/SyntaxTint.h"
#include "term/grid.h"
#include "ui/ConnectionDialog.h"
#include "term/input.h"
#include "term/vtparser.h"

class App
{
public:
    // diagMode: start with a local diagnostic session (no SSH) that shows the
    // color/attribute test screen — used for visual and performance checks.
    // playPath: open this asciinema .cast in a local playback tab instead of
    // the connection manager (--play <file>).
    bool Init(HWND hwnd, bool diagMode = false, const std::string& connectId = {},
              const std::wstring& playPath = {}, const std::string& localShell = {});
    // Open an asciinema .cast in a local playback tab (File menu, --play).
    void PlayRecordingFile(const std::wstring& path);
    void Shutdown();
    void Tick();                       // called when the message queue is idle

    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM, bool& handled);

private:
    // frame
    void RenderFrame();
    void BuildVisualsFromGrid();
    void RecordScene(ID3D12GraphicsCommandList* cl);
    void DrawTabBar();
    void DrawStatusLine();
    // Ambient depth: slow-drifting background particles behind the terminal.
    void DrawBackground();

    // custom (client-drawn) title bar — Termius-style hamburger, session tabs,
    // a new-session (+) button and window controls, replacing the OS caption.
    enum class CapZone { None, Menu, Plus, Min, Max, Close };
    void DrawTitleBar();
    void CaptionLayout(RECT& menu, RECT& mn, RECT& mx, RECT& cl) const;
    CapZone CaptionZoneAt(int px, int py) const;
    void CaptionClick(CapZone z);
    float TitleBarH() const;
    void OpenAppMenu();                // pop the hamburger menu
    // Session tab under a title-bar point; -1 = none. closeHit = the tab's ×.
    int TitleTabAt(int px, int py, bool& closeHit) const;

    // lifecycle / layout
    void CreateSceneTarget();
    void UpdateFontMetrics(float fontPx);
    void UpdateGridDims();
    void HandleColorSpaceChange();

    // menu bar
    void BuildMenus();
    void UpdateMenuChecks();
    bool HandleMenuCommand(int id);    // true if the id was one of ours

    // theming ---------------------------------------------------------------
    // Applies theme m_themeId: fills the global CPU ramp, the particle ramp,
    // and the menu chrome colors, then repaints.
    void ApplyTheme();
    bool PromptCustomTheme();          // 3-color picker; false = cancelled
    void ThemeMenuBar(HMENU menu, int depth = 0);  // owner-draw dropdowns
    static const wchar_t* MenuFaceName();
    void MeasureMenuItem(MEASUREITEMSTRUCT& mis);
    void DrawMenuItem(const DRAWITEMSTRUCT& dis);
    // UAH themed menu-bar painting (LPARAM is a UAHMENU / UAHDRAWMENUITEM*).
    void DrawMenuBarBg(void* uahMenu);
    void DrawMenuBarItem(void* uahDrawItem);

    // settings / palette / diagnostics
    void LoadSettings();
    void SaveSettings();
    void ApplyEffectSettings();        // pushes toggles into the tunables
    const amber::Palette16& Pal() const;
    void StartDiagSession();           // local, no SSH
    void FeedDiagnostic(amber::Session& s);
    void UpdateAutoDensity();
    uint32_t RequestedDensity() const; // resolves Auto to the adaptive value
    const char* DensityLabel() const;
    void TriggerShockwave();           // ring impulse at the cursor cell
    // Modal numeric prompt for the Custom density option; 0 = cancelled.
    uint32_t PromptDensity(uint32_t current);
    // Modal decimal prompt for a custom motion speed; 0 = cancelled.
    float PromptSpeedValue(float current);
    float CurrentSpeed() const;        // resolves preset/custom speed level
    // Modal single-line text prompt; false = cancelled.
    bool PromptText(const char* title, std::string& inOut);

    // --- roadmap features ---------------------------------------------------
    void ToggleLogging();              // transcript logging for the active tab
    void SearchScrollbackPrompt();     // Ctrl+Shift+F
    void SearchNext();                 // F3 — continue upward from last match
    void OpenSftpPanel();              // SFTP browser for the active session
    void ImportProfiles();             // PuTTY registry + OpenSSH config
    void DownloadFont(int faceIndex);  // fetch a font not yet bundled/installed
    void DownloadSymbolsFont();        // one-time nerd-symbols fetch for PUA icons
    void DownloadFontFamily(const std::wstring& family); // a chrome face, by name
    void RegisterGdiFonts();           // bundled fonts as GDI private resources
    void ApplyChromeFace();            // resolve the skin's face for dialogs + strip
    void SplitPane(bool vertical);     // duplicate session into a second pane
    void CloseSplit();                 // keep the focused pane only
    void StartReconnect(amber::Session& s);   // build cfg from stored secrets
    void OpenUrlAt(int row, int col);  // Ctrl+click URL launcher
    bool OpenRemotePathAt(int row, int col);   // Ctrl+click remote path -> SFTP
    void PreviewRemoteFileAt(int row, int col);   // Ctrl+click file → SFTP open
    void SftpNewTab();                 // "+" in the SFTP browser

    // sessions --------------------------------------------------------------
    bool HasSession() const
    {
        return m_active >= 0 && m_active < static_cast<int>(m_sessions.size());
    }
    amber::Session& Cur() { return *m_sessions[static_cast<size_t>(m_active)]; }
    const amber::Session& Cur() const
    {
        return *m_sessions[static_cast<size_t>(m_active)];
    }
    // The session that receives input: the active tab's focused pane.
    amber::Session& Foc()
    {
        amber::Session& c = Cur();
        return (c.pane && c.paneFocus == 1) ? *c.pane : c;
    }
    const amber::Session& Foc() const
    {
        const amber::Session& c = Cur();
        return (c.pane && c.paneFocus == 1) ? *c.pane : c;
    }
    // View-grid offset of a session inside the active tab's layout.
    void PaneOffset(const amber::Session& s, int& colOff, int& rowOff) const
    {
        colOff = rowOff = 0;
        if (!HasSession() || !Cur().pane || &s != Cur().pane.get())
            return;
        if (Cur().paneVertical)
            colOff = Cur().grid.Cols() + 1;
        else
            rowOff = Cur().grid.Rows() + 1;
    }
    template <typename F> void ForEachSession(F&& f)
    {
        for (auto& sp : m_sessions)
        {
            f(*sp);
            if (sp->pane)
                f(*sp->pane);
        }
    }

    bool ShowConnectionDialog();       // modal; creates and starts a session
    bool StartSession(amber::ConnectionRequest& req);   // from any source
    // Opens a local console session for a discovered shell key ("pwsh",
    // "wsl:Ubuntu"...). The profile is ephemeral: nothing is saved unless the
    // user saves it from the connection manager.
    bool NewLocalSession(const std::string& shellKey);
    bool ConnectProfileById(const std::string& id);     // jump list / --connect
    bool UnlockSecrets();              // Windows Hello gate for remembered secrets
    void UpdateJumpList();             // taskbar "Saved Sessions" jump list
    // CloseSession plays the CRT power-off collapse (active tab), then
    // CloseSessionNow does the actual teardown.
    void CloseSession(int index);
    void CloseSessionNow(int index);
    // dir: +1 = the new session comes in from the right, -1 from the left,
    // 0 = inferred from tab order (drives the Compiz cube direction).
    void SelectTab(int index, int dir = 0);
    void CycleTab(int delta);
    void PumpSshEvents();              // drains every session, not just active
    void DrainSessionOutput(amber::Session& s, int budget);
    int  TabHitTest(int px, int py) const;

    // input
    void OnChar(wchar_t wc, bool alt);
    bool OnKeyDown(WPARAM vk);
    void OnMouseButton(bool down, int px, int py, bool rightButton = false,
                       bool middleButton = false);

    // ---- PuTTY-style per-profile behaviour (Connection dialog pages) ------
    void BuildSshConfig(const amber::ConnectionProfile& p, SshConfig& cfg) const;
    void ApplyProfileToSession(amber::Session& s);       // parser features, scrollback, echo
    void ApplyProfileGlobals(const amber::ConnectionProfile& p);   // font, preset, density
    void RingBell(amber::Session& s, bool activeTab);    // Bell page
    void OpenProfileLog(amber::Session& s);              // Logging page
    std::string ExpandLogName(const amber::ConnectionProfile& p) const;
    bool HandleLocalLine(amber::Session& s, const std::string& bytes);   // echo / line editing
    void ShowContextMenu(int px, int py);                // Selection: Windows mode right-click
    void OnRemoteResize(amber::Session& s, int cols, int rows);   // CSI 8 t
    std::string TitleFor(const amber::Session& s) const; // Behaviour: window title
    int m_gapPx = 8;                                     // Appearance: text/edge gap in use

    // ---- interface skin (ui/Chrome.h): title strip + tabs per style -------
    int m_chromeId = 0;                                  // settings "chrome"
    void DrawTitleBarClassic();
    void DrawTitleBarCyber();
    void DrawCaptionGlyphs(const RECT& menu, const RECT& mn, const RECT& mx,
                           const RECT& cl, const float ic[4], const float bar[4],
                           float th, float dpi, float bt, bool dark = false);
    // Skin text: additive coloured (neon) or opaque dark-on-bar (LCARS).
    float SkinText(float x, float y, const std::string& s, const float rgb[3],
                   uint32_t onFill = 0);
    std::string SkinCase(const std::string& s);
    void OnMouseMove(int px, int py);
    void OnWheel(int delta, bool ctrl);
    // Terminal mouse reporting (?1000/?1002/?1003, SGR ?1006): translates a
    // mouse event into an escape report for the remote app. Returns true
    // when consumed (reporting active and Shift not held). evt: 0 press,
    // 1 release, 2 motion, 3 wheel-up, 4 wheel-down; btn: 0 L, 1 M, 2 R.
    bool MouseReport(int px, int py, int evt, int btn);
    void SendToShell(const std::string& bytes);
    void CopySelection();
    void SetClipboardText(const std::string& utf8);
    void Paste();
    void ToggleFullscreen();
    bool CellFromPx(int px, int py, int& row, int& col) const;
    void SetStatus(const std::string& text, double seconds = 2.5);

    HWND m_hwnd = nullptr;
    Device m_device;
    ShaderCompiler m_shaders;
    GlyphSampler m_sampler;
    GlyphSampler m_chromeSampler;        // the skin's own face for the strip
    bool m_chromeReady = false;
    std::set<std::wstring> m_fontFetches; // chrome faces fetched this session
    ParticleRenderer m_particles;
    Bloom m_bloom;
    Composite m_composite;
    PrimRenderer m_prims;

    amber::ProfileStore m_profiles;
    std::vector<amber::SessionPtr> m_sessions;
    int m_active = -1;

    // scene target
    ComPtr<ID3D12Resource> m_scene;
    uint32_t m_sceneRtvSlot = UINT32_MAX;
    uint32_t m_sceneSrvSlot = UINT32_MAX;
    D3D12_RESOURCE_STATES m_sceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    std::vector<CellVisual> m_visuals;
    // Crisp-core glyphs staged per frame so their reveal follows the same
    // birth wave as the particles (bulk repaints draw, not pop).
    struct CoreGlyphPlan
    {
        uint32_t cell;
        char32_t cp;
        float x, y;
        float rgb[3];
        float shear;
        bool emoji;      // draw via the full-colour emoji atlas
        bool wide;       // double-width lead cell (two-cell budget)
        bool vs16;       // U+FE0F forced colour presentation
        // Row dimming (spotlight, Mothership shadow). The sharp letterform
        // carries most of a cell's legibility, so dimming only the particle
        // halo does not actually dim the text — the core has to follow.
        float dim = 1.0f;
    };
    std::vector<CoreGlyphPlan> m_corePlan;
    GridMetrics m_gm;
    float m_fontPx = 16.0f;
    uint32_t m_dpi = 96;

    // custom title bar geometry / hover state
    float m_titleBarH = 0.0f;
    CapZone m_capHover = CapZone::None;
    int m_tabHover = -1;               // session tab under the pointer
    RECT m_plusRect = {};              // new-session button, title-bar coords
    // "Fake maximize": fills the monitor work area in the NORMAL window state
    // so DWM keeps the rounded corners (a truly maximized window is squared).
    bool m_fakeMax = false;
    RECT m_restoreRect = {};
    void ToggleMaximize();

    // tab bar geometry, recomputed each frame (now inside the title bar)
    float m_tabBarH = 0.0f;
    struct TabRect { float x, w; };
    std::vector<TabRect> m_tabRects;

    // timing
    LARGE_INTEGER m_qpcFreq = {}, m_qpcStart = {};
    double m_lastFrameTime = 0.0;
    float m_time = 0.0f, m_dt = 1.0f / 120.0f;
    double m_lastColorSpaceCheck = 0.0;
    float m_fps = 0.0f, m_frameMs = 0.0f;
    double m_fpsAccum = 0.0;
    int m_fpsFrames = 0;

    // options
    float m_bloomStrength = 0.5f;
    bool m_vsync = true;
    bool m_showOverlay = false;
    bool m_fullscreen = false;
    bool m_syntaxTint = true;

    // menu-driven settings (persisted in settings.json)
    HMENU m_menu = nullptr;
    bool m_densityAuto = false;
    uint32_t m_densityPpc = 96;        // High Clarity default
    uint32_t m_autoPpc = 96;           // adaptive value when Auto is on
    double m_autoLastAdjust = 0.0;
    bool m_crispCore = true;           // Particle + Crisp Core (vs Particles Only)
    int m_paletteId = 0;               // 0 = Amber Miami, 1 = Classic xterm
    int m_termTypeId = 0;              // 0 xterm-256color, 1 screen-256color, 2 xterm-direct
    int m_bloomLevel = 1;              // 0 low, 1 medium, 2 high
    int m_twinkleLevel = 1;            // 0 off, 1 subtle, 2 full
    int m_trailLevel = 1;              // 0 off, 1 low, 2 medium, 3 high
    int m_speedLevel = 1;              // 0..3 presets; 4 = custom multiplier
    float m_customSpeed = 1.0f;        // 0.1x .. 5x, used when m_speedLevel == 4
    int m_fontStyle = 0;               // 0 modern, 1 dot-matrix 8, 2 dot-matrix 12
    int m_fontFace = 0;                // index into kFontFaces
    // Font directories searched for bundled (exe\fonts) and downloaded
    // (%LOCALAPPDATA%\AmberSSH\fonts) faces.
    std::vector<std::wstring> m_fontDirs;
    bool m_symbolsFetchStarted = false;   // one nerd-symbols fetch per run
    int m_themeId = 0;                 // index into kThemes; last = Custom
    // Per-host theme rules from settings ("hostThemes": "prod*=5,*.dev=0"):
    // a connecting session whose host matches a pattern gets that theme while
    // it is the active tab — production boxes glow different on sight.
    std::string m_hostThemes;
    int MatchHostTheme(const std::string& host) const;
    // Exit-code flash (OSC 133;D) + CRT power-off close animation.
    double m_exitFlashStart = -1e9;
    float m_exitFlashCol[3] = { 0, 0, 0 };
    float m_exitFlashAmt = 0.0f;
    static constexpr double kCrtOffSecs = 0.45;
    int m_closingTab = -1;
    double m_closeStart = 0.0;
    bool m_broadcast = false;          // type into both split panes at once

    // Eye-candy set.
    bool m_fxHeat = true;              // activity heat map
    bool m_fxGhost = true;             // latency ghosting (RTT cursor smear)
    bool m_fxAudio = false;            // audio-reactive turbulence (opt-in)
    bool m_fxBoot = true;              // boot sequence + phosphor warm-up
    int m_saverSecs = 300;             // digital-rain screensaver idle; 0 = off
    double m_lastInputTime = 0.0;      // any key/mouse activity (app time)
    float m_rainAmt = 0.0f;            // screensaver blend 0..1 (ramped)
    amber::AudioLevel m_audio;         // WASAPI loopback level monitor
    std::string BootSequenceText(const std::string& host) const;

    // Notifications, snippets, output triggers, Windows Hello gate.
    amber::TrayNotifier m_tray;
    bool m_helloUnlock = false;        // require Windows Hello for stored secrets
    struct Snippet { std::string name, text; };
    std::vector<Snippet> m_snippets;
    std::vector<std::pair<std::string, std::regex>> m_triggers;
    long long m_snippetsMtime = 0, m_triggersMtime = 0;
    double m_userFilesChecked = 0.0;
    void LoadSnippets();
    void LoadTriggers();
    void CheckUserFiles();             // reload when edited (polled)
    void EditUserFile(const char* name, const char* tmpl);
    void RunSnippet(int index);
    void ScanTriggers(amber::Session& s, const uint8_t* d, size_t n);
    // rowsBelow: complete lines that followed the matched one in the same
    // drained chunk (the cursor is already past them).
    void FireTrigger(amber::Session& s, const std::string& pattern,
                     const std::string& line, int rowsBelow);

    // asciinema v2 recording / playback.
    void ToggleRecording();
    void RecordCast(amber::Session& s, const uint8_t* d, size_t n);
    void PlayRecording();

    // Remote vitals (title-bar sparklines), port forwarding, quake mode.
    amber::VitalsMonitor m_vitals;
    bool m_vitalsOn = false;
    int m_sharpness = 1;               // text: 0 Soft, 1 Crisp, 2 Razor (post-bloom core)

    // Live effects batch: tide marks, running-command pulse, typing echo
    // ring, momentum cursor, connection weather.
    bool m_fxLive = true;
    bool m_fxPersist = true;           // phosphor persistence (afterglow + afterimages)

    // Compiz-style cube on session switch: the departing frame is frozen
    // (scene + bloom) and rotates away while the live scene rotates in.
    bool m_fxCube = true;
    ComPtr<ID3D12Resource> m_cubeSnap, m_cubeSnapBloom;
    uint32_t m_cubeSnapSrv = UINT32_MAX, m_cubeSnapBloomSrv = UINT32_MAX;
    D3D12_RESOURCE_STATES m_cubeSnapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES m_cubeSnapBloomState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t m_cubeSnapW = 0, m_cubeSnapH = 0, m_cubeSnapBW = 0, m_cubeSnapBH = 0;
    bool m_cubePending = false;        // freeze the outgoing frame next frame
    int m_cubeDir = 1;                 // +1 next (right), -1 previous (left)
    double m_cubeStart = -1e9;
    void EnsureCubeSnapshots();
    void SnapshotForCube(ID3D12GraphicsCommandList* cl);
    struct TrailPt { float x, y; double t; };
    std::vector<TrailPt> m_cursorTrail;
    float m_lastCurPx = -1.0f, m_lastCurPy = -1.0f;
    float m_weatherTurb = 0.0f;
    uint32_t m_prevRtt = 0, m_lastRetrans = 0;
    double m_lastWeatherSample = 0.0;
    void DrawLiveEffects();
    void UpdateWeather();
    void OnShellMark(amber::Session& s, char kind, int code);   // OSC 133
    void AddEmber(amber::Session& s, bool severe, int rowsBelow);   // error line → red smoulder
    double m_shakeStart = -1e9;        // screen shake anchor
    float m_scrollBarAlpha = 0.0f;     // scroll-position phosphor bar fade
    int m_lastViewOffset = 0;
    double m_scrollChangedAt = -1e9;
    void SelectBlockAt(int row, int col);   // Ctrl+Shift+click: command output block
    void SyncVitals();                 // start/stop to follow the active tab
    void DrawVitals(float rightEdgeX); // sparklines in the title bar
    void EditForwards();
    bool m_quake = false;              // global Ctrl+` dropdown terminal
    bool m_quakeHidden = false;
    RECT m_quakeRestore = {};
    void ApplyQuakeHotkey();
    void QuakeToggle();

    // Inline images: GPU textures for Session::InlineImage placements.
    struct GpuImage
    {
        uint64_t key = 0;              // (session ptr hash << 32) ^ image id
        ComPtr<ID3D12Resource> tex, staging;
        uint32_t srv = 0;
        int w = 0, h = 0;
        double lastUsed = 0.0;
        bool uploaded = false;
    };
    std::vector<GpuImage> m_gpuImages;
    std::vector<uint32_t> m_imageSrvPool;
    int OnInlineImage(amber::Session& s, amber::DecodedImage&& img, int cols, int rows);
    void DrawInlineImages(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                          D3D12_GPU_VIRTUAL_ADDRESS cb);
    GpuImage* EnsureGpuImage(ID3D12GraphicsCommandList* cl, amber::Session& s,
                             amber::Session::InlineImage& im);

    // Command palette (Ctrl+Shift+P): fuzzy access to every action/setting.
    struct PalItem
    {
        std::string label;             // "Theme: Emerald CRT"
        int cmd;                       // menu id routed via HandleMenuCommand
    };
    bool m_palOpen = false;
    std::string m_palQuery;
    int m_palSel = 0;
    std::vector<PalItem> m_palAll;
    std::vector<int> m_palHits;        // indices into m_palAll, best first
    void TogglePalette();
    void BuildPaletteItems();
    void FilterPalette();
    bool PaletteKey(WPARAM vk);        // navigation / run / dismiss
    void DrawPalette();

    // Command journal (Ctrl+Shift+J): every command run through AmberSSH,
    // captured from the OSC 133 marks, searchable by command, host or
    // directory, with its exit status and how long it took.
    amber::CommandJournal m_journal;
    bool m_journalOn = true;           // settings "journal" — capture on/off
    bool m_jrnOpen = false;
    std::string m_jrnQuery;
    int m_jrnSel = 0;
    std::vector<size_t> m_jrnHits;     // indices into m_journal.Entries()
    // The rectangle of whichever overlay panel is open this frame, in pixels.
    // Published to the particle field (which is evicted from it) and used to
    // suppress the crisp glyph cores underneath, so the panel is genuinely
    // opaque rather than merely dark. Zero width = no panel.
    struct PanelRect { float x = 0, y = 0, w = 0, h = 0; };
    PanelRect m_panelRect;
    void SetPanelRect(float x, float y, float w, float h);

    void ToggleJournal();
    void FilterJournal();
    bool JournalKey(WPARAM vk);
    void DrawJournal();
    // Reads the typed command off the grid between the OSC 133 B and C marks.
    std::string LiftCommandText(const amber::Session& s) const;
    int m_bgStyle = 0;                 // 0 off, 1 Embers, 2 Starfield, 3 Cosmic Dust
    int m_appearance = 0;              // 0 dark, 1 light (paper)
    int m_shadowLevel = 2;             // light-mode text shadow: 0 off..3 strong
    void ApplyAppearance();            // pushes light/dark + shadow into tunables
    uint32_t m_customTheme[3] = { 0x7A5500, 0xFFB000, 0xFFF3C4 };  // sRGB
    // Menu chrome derived from the active theme (sRGB COLORREFs).
    struct MenuColors
    {
        COLORREF bg, sel, text, textHot, dim, border, barBg, barText;
    } m_menuColors = {};
    HBRUSH m_menuBgBrush = nullptr;   // MENUINFO brush = the menu-bar strip
    HFONT m_menuFont = nullptr;
    // Owner-draw menu labels: dwItemData points into these (stable storage).
    std::vector<std::unique_ptr<std::wstring>> m_menuStrings;
    bool m_fxShockwave = true;         // shockwave on Enter/Backspace/Delete
    bool m_fxCascade = true;           // paced output so listings materialize
    bool m_fxBell = true;              // BEL rings a particle shockwave
    std::string m_searchTerm;          // scrollback search state
    int m_searchAbsRow = -1;           // absolute row of the last match
    bool m_miamiSelection = true;
    bool m_fxBloom = true;
    bool m_fxScanlines = true;
    bool m_fxVignette = true;
    bool m_fxDrift = true;             // curl-noise gas drift
    bool m_fxPointerForce = true;      // Alt/middle-drag force field
    bool m_diagMode = false;

    // perf logging (AMBERSSH_PERFLOG=<path> appends one CSV row per second)
    std::string m_perfLogPath;
    double m_perfLogLast = 0.0;
    RECT m_savedRect = {};
    LONG m_savedStyle = 0;
    bool m_focused = true;
    bool m_minimized = false;

    // transient status message ("Copied 148 characters")
    std::string m_status;
    double m_statusUntil = 0.0;

    // ---- bottom status bar -------------------------------------------------
    // A thin strip along the foot of the window: connection state, host and
    // remote directory on the left, the transient status message in the
    // middle, and clickable chips for the actions worth one click on the
    // right. It reserves its own height, so it never covers terminal output
    // the way the old floating status text did.
    struct StatusChip
    {
        float x = 0, w = 0;      // laid out during the draw, hit-tested after
        int cmd = 0;             // menu id routed through HandleMenuCommand
        std::string label;
        bool on = false;         // toggles render lit when active
    };
    bool m_statusBar = true;              // settings "statusBar"
    float m_statusBarH = 0.0f;            // reserved height, 0 when hidden
    int m_sbHover = -1;                   // index into m_sbChips
    std::vector<StatusChip> m_sbChips;
    float StatusBarH() const;
    void DrawStatusBar();
    int StatusChipAt(int px, int py) const;   // -1 = not on a chip

    // ---- smart paste guard -------------------------------------------------
    // Text containing a newline submits commands the instant it lands, which
    // is the most common way a terminal user wrecks a machine. Anything with a
    // line break (or anything very long) is previewed and confirmed first.
    bool m_pasteGuard = true;          // settings "pasteGuard"
    bool m_pasteOpen = false;
    std::string m_pastePending;        // already normalised for the pty
    int m_pasteLines = 0;
    void SendPasteText(const std::string& norm);
    void DrawPasteGuard();
    bool PasteGuardKey(WPARAM vk);

    // ---- jump between commands in the scrollback ---------------------------
    // Steps the view to the previous/next prompt mark, naming the command it
    // lands on in the status bar. dir < 0 = older, dir > 0 = newer.
    void JumpToMark(int dir);
    std::string RowText(const amber::Session& s, uint64_t rowId) const;

    // ---- output folding ----------------------------------------------------
    // A completed command's output collapses to a single summary line. The
    // map from display rows to real grid rows is rebuilt each frame and is
    // consulted by BOTH the compose pass and the pixel-to-cell helper, so
    // selection, links and mouse reporting all stay aligned with what is on
    // screen. Collapsing changes many cells at once, so the active motion
    // style animates it exactly like any other bulk repaint.
    // ---- workspaces --------------------------------------------------------
    // A named set of sessions, restored in one click from the menu or the
    // taskbar jump list.
    amber::WorkspaceStore m_workspaces;
    void SaveWorkspaceAs();
    void OpenWorkspace(const std::string& name);
    void DeleteWorkspace(const std::string& name);
    void RebuildWorkspaceMenu();
    HMENU m_workspaceMenu = nullptr;
    std::vector<std::string> m_workspaceNames;   // parallel to the menu items

    // ---- the effects added alongside the departure styles -------------------
    // The style the USER picked. The style actually simulated lives in
    // tun.animStyle and may differ, because a per-host rule can override it —
    // keeping the two apart is what stops an override rewriting the setting.
    int m_motionStyle = 0;
    int m_departStyle = 0;             // settings "departStyle"
    bool m_fxParallax = false;         // settings "fxParallax"
    bool m_fxSlosh = false;            // settings "fxSlosh"
    bool m_fxWarmup = true;            // settings "fxWarmup"
    bool m_fxSpotlight = false;        // settings "fxSpotlight"
    int m_nightShift = 0;              // 0 off, 1 auto by clock, 2 always warm
    // Time dial: Alt+wheel scales the passage of effect time, for showing the
    // motion off and for looking at what a style is actually doing.
    float m_timeDial = 1.0f;
    RECT m_lastWindowRect = {};        // for the slosh impulse
    std::string m_hostMotion;          // "prod*=14,*.dev=7" — per-host style
    int MotionForHost(const std::string& host) const;
    float NightShiftAmount() const;
    void UpdateFieldEffects();         // parallax, warm-up, slosh decay

    void BuildFoldMap(amber::Session& s);
    Cell FoldedCell(const amber::Session& s, int viewRow, int col) const;
    int FoldSummaryAtPx(int px, int py) const;   // fold index, or -1
    void FoldAll(bool collapsed);
    void ToggleFoldAtCursor();

    // ---- taskbar progress --------------------------------------------------
    int m_tbState = -1;                // last state pushed, avoids COM churn
    int m_tbPct = -1;
    double m_tbErrorUntil = 0.0;       // hold a red taskbar this long
    void UpdateTaskbarProgress();

    // cursor orbit: phase only advances while the cursor is travelling, so
    // the border swirl coasts to a stop ~1 s after the cursor parks.
    uint32_t m_cursorPrevCell = 0xFFFFFFFFu;
    float m_cursorMovedAt = -10.0f;
    float m_cursorPhase = 0.0f;

    // input state
    bool m_swallowChar = false;
    wchar_t m_pendingHighSurrogate = 0;
    int m_wheelAccum = 0;
    bool m_forceDrag = false;          // Alt/middle-drag particle force
    int m_mouseBtnDown = -1;           // button held while reporting (-1 none)
    int m_mouseRepR = -1, m_mouseRepC = -1;   // motion-report dedupe cell
    int m_lastMousePx = 0, m_lastMousePy = 0; // for wheel reports

    std::string m_titleBase = "AmberSSH";
};
