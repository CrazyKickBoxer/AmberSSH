// app_reel.cpp — the demo reel: a timed performance of the client for a
// camera, cut to a beat.
//
// --reel runs a scripted sequence and nothing else: a terminal that types
// itself, every motion style on successive beats, the interface styles and
// appearances, then a real connection to a VNC profile of the user's with
// real input into its desktop, and back out. Every step sits on a beat of
// the track the reel will be cut to (AMBER_REEL_BPM), so a scene change
// lands where the ear expects one. The screen is grabbed from outside by
// FFmpeg; the reel only performs.
//
// Two markers make the cut exact without any clock being shared: the reel
// holds a black screen, then flashes the whole frame light for a tenth of
// a second at beat zero, and again at its last beat. The post-process finds
// both with blackdetect and trims to them, so the audio's first beat is
// laid on the first flash to within a frame.
//
// It borrows the motion style, the skin, the appearance and the fullscreen
// state, and puts all four back. It never saves settings.
//
//   AMBER_REEL_BPM      the track's tempo (default 90.5)
//   AMBER_REEL_PROFILE  the id of the VNC profile to connect to (required
//                       for the desktop act; without it the act is skipped)
//   AMBER_REEL_STRETCH  multiplies every beat position (default 1)
#include "app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "ui/Chrome.h"
#include "vnc/Keysyms.h"

namespace
{

// keysyms the desktop act needs beyond Keysyms.h's set
constexpr uint32_t kReturn = 0xFF0D, kEscape = 0xFF1B, kF2 = 0xFFBF, kF4 = 0xFFC1, kF7 = 0xFFC4;
constexpr uint32_t kLeft = 0xFF51, kUp = 0xFF52, kRight = 0xFF53, kDown = 0xFF54;

std::string Env(const char* name)
{
    char buf[512] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return (n == 0 || n >= sizeof buf) ? std::string() : std::string(buf, n);
}

// Nerd Font glyphs, from the Private Use Area. AmberSSH's fallback chain
// resolves them out of %LOCALAPPDATA%\AmberSSH\fonts (SymbolsNerdFontMono),
// which is what the prompt-icon auto-fetch puts there. Written as escapes
// rather than literal bytes so the source's own encoding cannot matter;
// each is its own literal because a C++ hex escape is greedy.
constexpr const char* kNfSep     = "\xEE\x82\xB0";   // U+E0B0 powerline separator
constexpr const char* kNfBranch  = "\xEE\x82\xA0";   // U+E0A0 git branch
constexpr const char* kNfFolder  = "\xEF\x81\xBB";   // U+F07B folder
constexpr const char* kNfFile    = "\xEF\x85\x9B";   // U+F15B file
constexpr const char* kNfFedora  = "\xEF\x85\xBC";   // U+F17C linux (U+F30A, fedora, is not in the symbols font)
constexpr const char* kNfDisk    = "\xEF\x82\xA0";   // U+F0A0 disk
constexpr const char* kNfChip    = "\xEF\x8B\x9B";   // U+F2DB chip
constexpr const char* kNfCheck   = "\xEF\x80\x8C";   // U+F00C check
constexpr const char* kNfCross   = "\xEF\x80\x8D";   // U+F00D cross
constexpr const char* kNfDown    = "\xEF\x80\x99";   // U+F019 download
constexpr const char* kNfLock    = "\xEF\x80\xA3";   // U+F023 lock
constexpr const char* kNfGear    = "\xEF\x80\x93";   // U+F013 cog
constexpr const char* kNfArchive = "\xEF\x87\x86";   // U+F1C6 archive
constexpr const char* kNfCode    = "\xEF\x84\xA1";   // U+F121 code
constexpr const char* kNfKey     = "\xEF\x82\x84";   // U+F084 key
constexpr const char* kNfClock   = "\xEF\x80\x97";   // U+F017 clock

// A powerline prompt: user segment, path segment, and the separators that
// make them read as one ribbon.
std::string Prompt()
{
    return std::string("\x1b[48;5;24m\x1b[38;5;255m ") + kNfKey + " josh@fedora \x1b[0m" +
           "\x1b[38;5;24m\x1b[48;5;238m" + kNfSep + "\x1b[0m" +
           "\x1b[48;5;238m\x1b[38;5;252m ~ \x1b[0m\x1b[38;5;238m" + kNfSep + "\x1b[0m ";
}

// One row of a listing, the way eza --icons draws it: mode, owner, size,
// date, then the type's glyph and the name in the colour ls would use.
std::string Row(const char* mode, const char* size, const char* date, const char* icon, const char* colour,
                const char* name)
{
    return std::string("\x1b[38;5;244m") + mode + "\x1b[0m josh josh \x1b[38;5;180m" + size +
           "\x1b[0m \x1b[38;5;108m" + date + "\x1b[0m  " + colour + icon + "  " + name + "\x1b[0m\r\n";
}

std::string Bar(double t)
{
    const int width = 30;
    const int filled = std::clamp(static_cast<int>(t * width + 0.5), 0, width);
    std::string bar;
    for (int i = 0; i < width; ++i)
        bar += i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91";
    char line[200];
    snprintf(line, sizeof line, "\r  \x1b[38;5;214m%s\x1b[0m %3d%%  %5.1f MB", bar.c_str(),
             static_cast<int>(t * 100.0 + 0.5), t * 18.4);
    return line;
}

} // namespace

struct App::Reel
{
    struct Step
    {
        double beat;
        std::function<void()> act;
    };
    std::vector<Step> steps;
    size_t next = 0;
    double t0 = 0.0;          // beat zero, absolute
    double beatSecs = 60.0 / 90.5;
    bool started = false;
    // what was borrowed
    int motion = 0, chrome = 0, theme = 0, appearance = 0;
    bool wasFullscreen = false;
    std::string profileId;
    bool done = false;
    // Everything the terminal has been told, so the whole screen can be put
    // back in one frame: the particle choreography fires when a cell's glyph
    // changes, so a redraw of every cell at once is what makes the text
    // reform under a new motion style on the beat.
    std::string screen;
    bool cascade = true;      // the pacing setting, borrowed for the motion act
    bool cascadeBorrowed = false;
};

void App::ReelStop()
{
    if (!m_reel)
        return;
    Reel& r = *m_reel;
    if (r.started)
    {
        m_motionStyle = r.motion;
        m_chromeId = r.chrome;
        amber::SetChrome(m_chromeId);
        m_themeId = r.theme;
        m_appearance = r.appearance;
        ApplyAppearance();
        ApplyTheme();
        if (r.cascadeBorrowed)
            m_fxCascade = r.cascade;
        UpdateMenuChecks();
    }
    delete m_reel;
    m_reel = nullptr;
}

// The reel's own tabs: the terminal it types into, and the desktop.
static amber::Session* ReelTerminal(std::vector<std::unique_ptr<amber::Session>>& sessions)
{
    for (auto& sp : sessions)
        if (sp->label == "reel")
            return sp.get();
    return nullptr;
}
static amber::Session* ReelDesktop(std::vector<std::unique_ptr<amber::Session>>& sessions, const std::string& id)
{
    for (auto& sp : sessions)
        if (sp->vnc && sp->profile.id == id)
            return sp.get();
    return nullptr;
}
static int IndexOf(std::vector<std::unique_ptr<amber::Session>>& sessions, const amber::Session* s)
{
    for (size_t i = 0; i < sessions.size(); ++i)
        if (sessions[i].get() == s)
            return static_cast<int>(i);
    return -1;
}

void App::ReelBuild()
{
    Reel& r = *m_reel;
    auto at = [&](double beat, std::function<void()> act) { r.steps.push_back({ beat, std::move(act) }); };
    auto say = [this, &r](const std::string& text) {
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += text;
        r.screen += text;
    };
    // A motion style, and the whole screen redrawn in one frame under it:
    // every cell changes at once, so every particle takes the new field's
    // choreography together, on the beat. Needs the cascade pacing off for
    // the act (put back after), or the redraw would type itself instead.
    auto style = [this, &r, say](int i) {
        m_motionStyle = std::clamp(i, 0, kMotionStyleCount - 1);
        const std::string name = MotionStyleAt(static_cast<uint32_t>(m_motionStyle)).name;
        char line[96];
        snprintf(line, sizeof line, "  \x1b[38;5;214m%02d\x1b[0m  %s\r\n", i + 1, name.c_str());
        const std::string whole = "\x1b[2J\x1b[H" + r.screen + line;
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += whole;
        r.screen += line;
        SetStatus("Motion: " + name, 1.0);
    };
    auto skin = [this, say](int i) {
        m_chromeId = std::clamp(i, 0, amber::kChromeCount - 1);
        amber::SetChrome(m_chromeId);
        ApplyTheme();
        UpdateMenuChecks();
        say(std::string("  \x1b[2minterface:\x1b[0m ") + amber::Chrome().name + "\r\n");
        SetStatus(std::string("Interface: ") + amber::Chrome().name, 1.0);
    };
    auto appearance = [this](int i) {
        m_appearance = i;
        ApplyAppearance();
    };
    auto desk = [this, &r]() -> amber::Session* { return ReelDesktop(m_sessions, r.profileId); };
    auto key = [desk](uint32_t ks, bool down) {
        if (amber::Session* d = desk())
            if (d->vnc && d->vnc->session)
                d->vnc->session->SendKey(down, ks);
    };
    auto tap = [key](uint32_t ks) { key(ks, true); key(ks, false); };
    auto chord = [key](uint32_t mod, uint32_t ks) { key(mod, true); key(ks, true); key(ks, false); key(mod, false); };
    auto type = [tap](const std::string& s) {
        for (unsigned char c : s)
            tap(amber::vnc::KeysymFromCodePoint(c));
        tap(kReturn);
    };
    auto redraw = [desk](int styleIdx) {
        if (amber::Session* d = desk())
            d->profile.vncTransition = styleIdx;
    };
    auto shock = [this, desk](int shockStyle, float fx, float fy) {
        amber::Session* d = desk();
        if (!d || !d->vnc || !d->vnc->session)
            return;
        amber::VncTab& v = *d->vnc;
        d->profile.vncShockStyle = shockStyle;
        // a real click on the desktop's background, and the wave from it
        const uint16_t px = static_cast<uint16_t>(fx * v.fbW), py = static_cast<uint16_t>(fy * v.fbH);
        v.session->SendPointer(1, px, py);
        v.session->SendPointer(0, px, py);
        v.shockX = v.dstX + px * v.scale;
        v.shockY = v.dstY + py * v.scale;
        v.shockTime = m_time;
        v.shockAmp = 1.0f;
    };
    auto showTerminal = [this]() {
        if (amber::Session* t = ReelTerminal(m_sessions))
        {
            const int i = IndexOf(m_sessions, t);
            if (i >= 0 && i != m_active)
                SelectTab(i, -1);
        }
    };

    // ---- beat 0: the marker, and the terminal ------------------------------------
    at(0.00, [appearance] { appearance(1); });   // the flash
    at(0.15, [appearance, say] {
        appearance(0);
        say("\x1b[2J\x1b[H\x1b[38;5;214m"
            "    _              _               ___ ___ _  _\r\n"
            "   /_\\  _ __  _ _ | |__  ___ _ _  / __/ __| || |\r\n"
            "  / _ \\| '  \\| '_>| '_ \\/ -_) '_| \\__ \\__ \\ __ |\r\n"
            " /_/ \\_\\_|_|_|_.__|_.__/\\___|_|   |___/___/_||_|\r\n"
            "\x1b[0m\r\n\x1b[1ma particle terminal\x1b[0m"
            "   \x1b[2m(recorded demonstration; the figures are this machine's own)\x1b[0m\r\n");
    });
    // A real listing of the Fedora box's home directory, drawn the way eza
    // draws one: the file-type glyph beside every name.
    at(3.5, [say] { say("\r\n" + Prompt() + "eza -la --icons\r\n"); });
    at(4.6, [say] {
        std::string d;
        d += Row("drwx------.", " 4096", "Sep  5 12:13", kNfFolder, "\x1b[38;5;75m", ".");
        d += Row("drwxr-xr-x.", "   18", "Sep  3 16:03", kNfFolder, "\x1b[38;5;75m", "..");
        d += Row("-rw-------.", " 8245", "Sep  5 12:11", kNfFile, "\x1b[38;5;250m", ".bash_history");
        d += Row("-rw-r--r--.", "  522", "Jan 15  2026", kNfCode, "\x1b[38;5;114m", ".bashrc");
        d += Row("-rw-r--r--.", "  144", "Jan 15  2026", kNfCode, "\x1b[38;5;114m", ".bash_profile");
        d += Row("drwx------.", " 4096", "Sep  5 11:46", kNfFolder, "\x1b[38;5;75m", ".cache");
        d += Row("drwx------.", " 4096", "Sep  5 11:49", kNfGear, "\x1b[38;5;180m", ".config");
        d += Row("drwx------.", "  125", "Sep  5 12:12", kNfLock, "\x1b[38;5;203m", ".gnupg");
        d += Row("drwx------.", "   61", "Sep  5 05:35", kNfKey, "\x1b[38;5;203m", ".ssh");
        d += Row("drwxr-xr-x.", "    6", "Sep  5 05:35", kNfFolder, "\x1b[38;5;75m", "Desktop");
        d += Row("drwxr-xr-x.", "    6", "Sep  5 05:35", kNfFolder, "\x1b[38;5;75m", "Documents");
        d += Row("drwxr-xr-x.", "    6", "Sep  5 05:35", kNfDown, "\x1b[38;5;75m", "Downloads");
        say(d);
    });
    // The machine it is really running on, in a fastfetch-style panel.
    at(11, [say] { say("\r\n" + Prompt() + "fastfetch\r\n"); });
    at(12.2, [say] {
        std::string d;
        d += std::string("      \x1b[38;5;33m") + kNfFedora + "\x1b[0m   \x1b[1;38;5;33mjosh\x1b[0m@\x1b[1;38;5;33mfedora\x1b[0m\r\n";
        d += "           \x1b[38;5;240m--------------------------------\x1b[0m\r\n";
        d += std::string("      \x1b[38;5;33m") + kNfFedora + "\x1b[0m   \x1b[1;38;5;33mOS\x1b[0m      Fedora Linux 44 (Server Edition)\r\n";
        d += std::string("           \x1b[1;38;5;33mKernel\x1b[0m  6.19.10-300.fc44.x86_64\r\n");
        d += std::string("      \x1b[38;5;33m") + kNfChip + "\x1b[0m   \x1b[1;38;5;33mCPU\x1b[0m     AMD Ryzen 5 7535HS\r\n";
        d += std::string("      \x1b[38;5;33m") + kNfDisk + "\x1b[0m   \x1b[1;38;5;33mDisk\x1b[0m    6.8G / 15G (46%)\r\n";
        d += std::string("      \x1b[38;5;33m") + kNfChip + "\x1b[0m   \x1b[1;38;5;33mMemory\x1b[0m  1.0Gi / 3.8Gi\r\n";
        d += std::string("      \x1b[38;5;33m") + kNfClock + "\x1b[0m   \x1b[1;38;5;33mUptime\x1b[0m  up 10 minutes\r\n";
        say(d);
    });
    at(18, [say] {
        say("\r\n" + Prompt() + "systemctl is-active sshd vncserver@:1 firewalld chronyd\r\n");
    });
    at(19.2, [say] {
        std::string d;
        for (const char* s : { "sshd", "vncserver@:1", "firewalld", "chronyd" })
            d += std::string("\x1b[38;5;114m") + kNfCheck + "\x1b[0m active   \x1b[38;5;250m" + s + "\x1b[0m\r\n";
        say(d);
    });
    at(22, [say] {
        say("\r\n" + Prompt() + "git status -sb\r\n" +
            "\x1b[38;5;214m" + kNfBranch + "\x1b[0m \x1b[1mamberx-phase0\x1b[0m\x1b[38;5;240m...origin/amberx-phase0\x1b[0m\r\n" +
            "\x1b[38;5;114m M\x1b[0m src/app_reel.cpp\r\n" +
            "\x1b[38;5;114m M\x1b[0m src/render/desktop.cpp\r\n" +
            "\x1b[38;5;203m??\x1b[0m docs/vnc.md\r\n");
    });
    at(25, [say] {
        say("\r\n" + Prompt() + "curl -O https://files.example.invalid/amberssh.zip\r\n");
    });
    for (int i = 0; i < 10; ++i)
        at(25.8 + i * 0.45, [say, i] {
            say(Bar((i + 1) / 10.0));
            if (i == 9)
                say(std::string("\r\n  \x1b[38;5;114m") + kNfCheck + "\x1b[0m  saved \x1b[38;5;180m" + kNfArchive +
                    "  amberssh.zip\x1b[0m \x1b[38;5;240m(18.4 MB)\x1b[0m\r\n");
        });

    // ---- beats 32..55: every motion style, one a beat, the text reforming on each --
    at(31.5, [this, &r, say] {
        r.cascade = m_fxCascade;
        r.cascadeBorrowed = true;
        m_fxCascade = false;   // the redraws land whole, in one frame each
        say("\r\n\x1b[38;5;214m-- every motion style --\x1b[0m\r\n");
    });
    for (int i = 0; i < kMotionStyleCount; ++i)
        at(32 + i, [style, i] { style(i); });
    at(32 + kMotionStyleCount, [this, &r, style] {
        style(0);
        m_fxCascade = r.cascade;
    });

    // ---- beats 56..71: every interface style, one a beat -----------------------------
    for (int i = 0; i < amber::kChromeCount; ++i)
        at(56 + i, [skin, i] { skin(i); });
    at(56 + amber::kChromeCount, [skin, this, &r] { skin(r.chrome); });

    // ---- beats 72..75: the appearances ---------------------------------------------
    at(72, [appearance] { appearance(1); });
    at(73, [appearance] { appearance(2); });
    at(74, [appearance] { appearance(3); });
    at(75, [appearance] { appearance(0); });

    // ---- beat 76: the desktop ------------------------------------------------------
    if (!r.profileId.empty())
    {
        at(76, [this, &r] { ConnectProfileById(r.profileId); });
        at(77, [desk] {
            // the reel's tab, not the saved profile: no clipboard prompts on camera
            if (amber::Session* d = desk())
                d->profile.vncClipboard = 0;
        });
        at(78, [redraw] { redraw(5); });                       // light speed for the arrival
        // shockwaves on real clicks, on the desktop's background
        at(84, [shock] { shock(0, 0.70f, 0.62f); });
        at(86, [shock] { shock(1, 0.55f, 0.55f); });
        at(88, [shock] { shock(2, 0.75f, 0.45f); });
        at(90, [shock] { shock(3, 0.60f, 0.65f); });
        // A terminal on the far side, opened with the desktop's own
        // Ctrl+Alt+T, then real commands run on the real machine — each one
        // redrawn in a different style as its output lands.
        at(92, [key] {
            key(amber::vnc::XK_Control_L, true);
            key(amber::vnc::XK_Alt_L, true);
            key(amber::vnc::KeysymFromCodePoint(U't'), true);
            key(amber::vnc::KeysymFromCodePoint(U't'), false);
            key(amber::vnc::XK_Alt_L, false);
            key(amber::vnc::XK_Control_L, false);
        });
        at(95, [redraw, type] { redraw(7); type("uname -srm"); });                    // iris
        at(98, [redraw, type] { redraw(8); type("free -h"); });                       // sonic boom
        at(101, [redraw, type] { redraw(9); type("ls -la ~"); });                     // shatter
        at(105, [redraw, type] { redraw(10); type("df -h /"); });                     // odometer
        at(108, [redraw, type] { redraw(6); type("systemctl is-active sshd vncserver@:1"); });   // shear plates
        at(112, [redraw, type] { redraw(5); type("top -b -n1 | head -14"); });        // light speed
        at(117, [redraw, type] { redraw(1); type("exit"); });                         // burn as it closes
        at(120, [redraw] { redraw(2); });                                             // dissolve
    }

    // ---- beats 122..130: back to the terminal, and out ------------------------------
    at(122, [showTerminal, style, say] {
        showTerminal();
        style(1);
        say("\r\n\x1b[38;5;214mAmberSSH\x1b[0m  the particle terminal\r\n"
            "\x1b[2mssh  telnet  serial  local  vnc\x1b[0m\r\n");
    });
    at(126, [style] { style(0); });
    at(130, [appearance] { appearance(1); });   // the end marker
    at(130.15, [this] {
        ApplyAppearance();
        m_reel->done = true;
    });
}

void App::ReelTick()
{
    if (m_reelRequested && !m_reel)
    {
        m_reelRequested = false;
        m_reel = new Reel();
        Reel& r = *m_reel;
        const std::string bpm = Env("AMBER_REEL_BPM");
        if (!bpm.empty())
            r.beatSecs = 60.0 / std::max(30.0, atof(bpm.c_str()));
        const std::string stretch = Env("AMBER_REEL_STRETCH");
        const double k = stretch.empty() ? 1.0 : std::max(0.25, atof(stretch.c_str()));
        r.beatSecs *= k;
        r.profileId = Env("AMBER_REEL_PROFILE");
        r.motion = m_motionStyle;
        r.chrome = m_chromeId;
        r.theme = m_themeId;
        r.appearance = m_appearance;
        r.wasFullscreen = m_fullscreen;
        r.started = true;
        // the stage: a dark, empty terminal, full screen; beat zero after a
        // hold of black so the first flash is unmistakable
        StartDiagSession();
        m_sessions.back()->label = "reel";
        m_appearance = 0;
        ApplyAppearance();
        m_motionStyle = 0;
        // the window is already maximized (App::Tick); the reel performs in
        // it as the user would see it, title bar and tab strip included
        r.t0 = m_time + 2.0;
        ReelBuild();
        std::sort(r.steps.begin(), r.steps.end(), [](const Reel::Step& a, const Reel::Step& b) { return a.beat < b.beat; });
        return;
    }
    if (!m_reel)
        return;
    Reel& r = *m_reel;
    if (r.done)
    {
        ReelStop();
        return;
    }
    if (!ReelTerminal(m_sessions))
    {
        ReelStop();   // the stage was closed
        return;
    }
    const double beat = (m_time - r.t0) / r.beatSecs;
    while (r.next < r.steps.size() && r.steps[r.next].beat <= beat)
    {
        r.steps[r.next].act();
        ++r.next;
        if (!m_reel)
            return;   // an act may have ended it
    }
}
