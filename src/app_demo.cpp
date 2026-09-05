// app_demo.cpp — the built-in demonstrations.
//
// Two of them, both self-contained: they contact nothing, need no server
// and leave no trace beyond the tab they open.
//
//   The SSH demonstration is a scripted terminal. It is NOT a connection
//   and says so in its own first lines: the text is played back into a
//   local session's parser at the cascade rate, so it types itself the way
//   real output arrives, and the host it pretends to fetch from is under
//   .invalid, a name reserved so that it can never resolve. It walks every
//   motion style in turn, which is the part that is hard to show any other
//   way — a particle terminal's character is in how it moves.
//
//   The VNC demonstration stands up the in-process RFB server that
//   --vnc-selfcheck uses, connects a real VNC tab to it over loopback, and
//   drives it: every redraw style against a real change, every shockwave
//   style against a real click, sustained load, a resize, and keys and
//   pointer events that the server counts and reports back. Everything in
//   it is the shipping code path; only the far end is ours.
//
// Both are steered from Tick by a step and a due time, so a demonstration
// never blocks the loop and can be abandoned at any point by closing its
// tab.
#include "app.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "vnc/Keysyms.h"
#include "vnc/SelfCheckServer.h"

namespace
{

// One beat of the scripted terminal: what to say, and how long to hold
// before the next. The cascade pacing types it out; the hold is what is
// left to read it by.
struct Beat
{
    double hold;
    std::string text;
};

std::string Rule(const char* title)
{
    std::string s = "\r\n\x1b[38;5;214m── ";
    s += title;
    s += " ";
    for (size_t i = std::string(title).size(); i < 58; ++i)
        s += "-";
    s += "\x1b[0m\r\n";
    return s;
}

const char* kPrompt = "\x1b[38;5;114mdemo\x1b[0m@\x1b[38;5;214mamberssh\x1b[0m:\x1b[38;5;75m~\x1b[0m$ ";

std::vector<Beat> ScriptedSession()
{
    std::vector<Beat> b;
    auto say = [&](double hold, std::string text) { b.push_back({ hold, std::move(text) }); };

    say(1.4,
        "\x1b[2J\x1b[H\x1b[38;5;214m"
        "    _              _               ___ ___ _  _\r\n"
        "   /_\\  _ __  _ _ | |__  ___ _ _  / __/ __| || |\r\n"
        "  / _ \\| '  \\| '_>| '_ \\/ -_) '_| \\__ \\__ \\ __ |\r\n"
        " /_/ \\_\\_|_|_|_.__|_.__/\\___|_|   |___/___/_||_|\r\n"
        "\x1b[0m\r\n"
        "\x1b[1mThis tab is a demonstration.\x1b[0m It is not a connection.\r\n"
        "Nothing was contacted, no bytes left this machine, and the\r\n"
        "output below is played back from inside the client itself.\r\n"
        "\r\nWhat it shows is the terminal: the parser, the colours, the\r\n"
        "command blocks, and the particle field that draws all of it.\r\n");

    say(1.1, Rule("a listing") + kPrompt + "ls -la --color\r\n");
    say(2.2,
        "total 148\r\n"
        "drwxr-xr-x  8 demo demo  4096 Sep  5 06:41 \x1b[1;34m.\x1b[0m\r\n"
        "drwxr-xr-x  3 root root  4096 Aug 30 11:02 \x1b[1;34m..\x1b[0m\r\n"
        "-rw-------  1 demo demo  8214 Sep  5 06:38 .bash_history\r\n"
        "drwx------  2 demo demo  4096 Aug 30 11:04 \x1b[1;34m.ssh\x1b[0m\r\n"
        "drwxr-xr-x  4 demo demo  4096 Sep  4 22:17 \x1b[1;34mprojects\x1b[0m\r\n"
        "-rwxr-xr-x  1 demo demo 12288 Sep  2 09:51 \x1b[1;32mdeploy.sh\x1b[0m\r\n"
        "-rw-r--r--  1 demo demo 90112 Sep  5 06:12 notes.md\r\n"
        "lrwxrwxrwx  1 demo demo    11 Sep  1 18:33 \x1b[1;36mlatest\x1b[0m -> notes.md\r\n"
        "-rw-r--r--  1 demo demo  8192 Aug 28 14:20 \x1b[1;31marchive.tar.gz\x1b[0m\r\n");

    say(1.0, "\r\n" + std::string(kPrompt) + "df -h /\r\n");
    say(1.8,
        "Filesystem      Size  Used Avail Use% Mounted on\r\n"
        "/dev/vda1        40G   27G   11G  \x1b[33m72%\x1b[0m /\r\n");

    say(1.0, "\r\n" + std::string(kPrompt) + "systemctl is-active nginx postgresql redis\r\n");
    say(1.6, "\x1b[32mactive\x1b[0m\r\n\x1b[32mactive\x1b[0m\r\n\x1b[31mfailed\x1b[0m\r\n");

    say(1.2, Rule("a download") + kPrompt + "curl -O https://files.example.invalid/amberssh-0.1.0.zip\r\n");
    return b;
}

// The download's progress line, rewritten in place. `t` runs 0 to 1.
std::string ProgressLine(double t)
{
    const int width = 34;
    const int filled = std::clamp(static_cast<int>(t * width + 0.5), 0, width);
    std::string bar;
    for (int i = 0; i < width; ++i)
        bar += i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91";   // full block, light shade
    char line[256];
    snprintf(line, sizeof line, "\r  \x1b[38;5;214m%s\x1b[0m %3d%%  %5.1f MB  %4.1f MB/s", bar.c_str(),
             static_cast<int>(t * 100.0 + 0.5), t * 18.4, 3.2 + 1.4 * (1.0 - t));
    return line;
}

} // namespace

struct App::Demo
{
    bool vnc = false;
    int step = 0;
    double nextAt = 0.0;
    double startedAt = 0.0;
    bool finished = false;

    // the scripted terminal
    std::vector<Beat> beats;
    size_t beat = 0;
    int savedMotion = 0;
    int savedStyleShown = -1;

    // the desktop
    std::unique_ptr<amber::vnc::SelfCheckServer> server;
    int savedTransition = 0;
    int shown = 0;                 // which style is being shown
    bool tabOpen = false;
};

void App::DemoStop()
{
    if (!m_demo)
        return;
    if (!m_demo->vnc)
        m_motionStyle = m_demo->savedMotion;   // the cycle borrowed it
    if (m_demo->server)
        m_demo->server->Stop();
    delete m_demo;
    m_demo = nullptr;
}

// The demo's own session, if it is still open. A demonstration the user
// closed is over: nothing is restarted and nothing is written to a tab
// that is no longer theirs.
static amber::Session* FindDemoTab(std::vector<std::unique_ptr<amber::Session>>& sessions, bool vnc)
{
    for (auto& sp : sessions)
        if (sp->label == (vnc ? "demo desktop" : "demo") && (!vnc || sp->vnc))
            return sp.get();
    return nullptr;
}


void App::DemoTick()
{
    if (m_demoRequested && !m_demo)
    {
        m_demoRequested = false;
        m_demo = new Demo();
        m_demo->vnc = m_demoVnc;
        m_demo->startedAt = m_time;
        // Step 0 opens the tab and runs in this same call, deliberately: the
        // frame loop is never asked to draw with no sessions at all. Nothing
        // else can produce that state — closing the last tab opens the
        // connection manager — so nothing else has to survive it.
        m_demo->nextAt = m_time;
        m_demo->savedMotion = m_motionStyle;
        m_demo->savedTransition = 0;
        if (!m_demo->vnc)
            m_demo->beats = ScriptedSession();
    }
    if (!m_demo || m_time < m_demo->nextAt)
        return;
    if (m_demo->vnc)
        DemoTickVnc();
    else
        DemoTickSsh();
}

// ---- the scripted terminal --------------------------------------------------------
void App::DemoTickSsh()
{
    Demo& d = *m_demo;
    amber::Session* tab = d.step == 0 ? nullptr : FindDemoTab(m_sessions, false);
    if (d.step > 0 && !tab)
    {
        DemoStop();   // the user closed it
        return;
    }

    // step 0 opens the tab; 1..N play the script; then the motion walk; then
    // a closing word.
    const int kFirstMotion = 1 + static_cast<int>(d.beats.size()) + 12;   // 12 progress beats
    if (d.step == 0)
    {
        StartDiagSession();
        m_sessions.back()->label = "demo";
        d.nextAt = m_time + 0.5;
        ++d.step;
        SetStatus("Demonstration: a scripted terminal. Nothing is connected.", 5.0);
        return;
    }
    if (d.step <= static_cast<int>(d.beats.size()))
    {
        const Beat& b = d.beats[static_cast<size_t>(d.step - 1)];
        tab->localPending += b.text;
        d.nextAt = m_time + b.hold;
        ++d.step;
        return;
    }
    const int prog = d.step - 1 - static_cast<int>(d.beats.size());
    if (prog < 12)
    {
        // the download: the same line rewritten, the way curl does it
        const double t = static_cast<double>(prog + 1) / 12.0;
        tab->localPending += ProgressLine(t);
        if (prog == 11)
            tab->localPending += "\r\n\x1b[32m  saved amberssh-0.1.0.zip (18.4 MB)\x1b[0m\r\n" +
                                 Rule("every motion style") +
                                 "  The field below is the same one the text is drawn in.\r\n"
                                 "  Each style is a force field the particles live under.\r\n";
        d.nextAt = m_time + (prog == 11 ? 1.6 : 0.16);
        ++d.step;
        return;
    }
    const int walk = d.step - kFirstMotion;
    if (walk < kMotionStyleCount)
    {
        m_motionStyle = walk;
        m_particles.ResetInstant();   // so the new field is seen at once
        char line[160];
        snprintf(line, sizeof line, "  \x1b[38;5;214m%02d/%02d\x1b[0m  %s\r\n", walk + 1, kMotionStyleCount,
                 std::string(MotionStyleAt(static_cast<uint32_t>(walk)).name).c_str());
        tab->localPending += line;
        SetStatus(std::string("Motion style: ") + MotionStyleAt(static_cast<uint32_t>(walk)).name, 1.2);
        d.nextAt = m_time + 0.85;
        ++d.step;
        return;
    }
    // done: hand the tab back
    tab->localPending +=
        "\r\n\x1b[38;5;214m── the demonstration is over "
        "-------------------------------\x1b[0m\r\n"
        "  The style is back to what it was. This tab is an ordinary\r\n"
        "  local session now: type in it, or close it.\r\n\r\n" +
        std::string(kPrompt);
    SetStatus("Demonstration finished.", 4.0);
    DemoStop();
}

// ---- the particle desktop ---------------------------------------------------------
void App::DemoTickVnc()
{
    Demo& d = *m_demo;
    // Every style, against a real change on a real connection.
    static const int kStyles[] = { 5, 7, 8, 9, 10, 1, 2, 3, 4, 6 };
    static const char* kStyleNames[] = { "light speed", "iris",         "sonic boom", "shatter and reform",
                                         "odometer",    "burn",         "dissolve",   "scan wipe",
                                         "emboss flash", "shear plates" };
    constexpr int kStyleCount = 10;

    if (d.step == 0)
    {
        d.server = std::make_unique<amber::vnc::SelfCheckServer>();
        if (!d.server->Start(1024, 640))
        {
            SetStatus("Demonstration: the in-process desktop server could not bind loopback.", 6.0);
            DemoStop();
            return;
        }
        amber::ConnectionRequest req;
        req.profile.id = amber::MakeUuid();
        req.profile.name = "demo desktop";
        req.profile.protocol = amber::Protocol::Vnc;
        req.profile.host = "127.0.0.1";
        req.profile.port = d.server->Port();
        req.profile.vncSolidity = 100;
        req.profile.vncDensity = 1;
        req.profile.vncDesktopSize = 0;      // this server has no ExtendedDesktopSize
        req.profile.vncCursorMode = 0;
        req.profile.vncTransition = kStyles[0];
        if (!StartSession(req))
        {
            SetStatus("Demonstration: the desktop tab could not be started.", 6.0);
            DemoStop();
            return;
        }
        if (HasSession())
            Cur().label = "demo desktop";
        SetStatus("Demonstration: a particle desktop, served from inside this process.", 5.0);
        d.nextAt = m_time + 1.2;
        ++d.step;
        return;
    }

    amber::Session* tab = FindDemoTab(m_sessions, true);
    if (!tab || !tab->vnc || !tab->vnc->session)
    {
        DemoStop();   // the user closed it
        return;
    }
    amber::VncTab& v = *tab->vnc;
    if (tab->vnc->session->State() != amber::vnc::VncState::Connected)
    {
        d.nextAt = m_time + 0.3;
        if (m_time - d.startedAt > 12.0)
        {
            SetStatus("Demonstration: the desktop did not connect.", 6.0);
            DemoStop();
        }
        return;
    }

    const int s = d.step - 1;
    if (s < kStyleCount)
    {
        // one style, one change: the block that changes is what the style acts on
        tab->profile.vncTransition = kStyles[s];
        const uint16_t bx = static_cast<uint16_t>(120 + (s % 4) * 190);
        const uint16_t by = static_cast<uint16_t>(110 + (s / 4) * 170);
        d.server->ChangeBlock(bx, by, 300, 150, 0xFF000000u | (0x2255FFu + static_cast<uint32_t>(s) * 0x1A3B07u));
        SetStatus(std::string("Redraw style: ") + kStyleNames[s], 1.5);
        d.nextAt = m_time + 1.7;
        ++d.step;
        return;
    }
    const int shock = s - kStyleCount;
    if (shock < 4)
    {
        // one shockwave style, one click, at a point on the picture
        static const char* kShockNames[] = { "ring", "water drop", "splash", "vortex" };
        tab->profile.vncShockStyle = shock;
        v.shockX = v.dstX + v.fbW * v.scale * 0.5f;
        v.shockY = v.dstY + v.fbH * v.scale * 0.5f;
        v.shockTime = m_time;
        v.shockAmp = 1.0f;
        SetStatus(std::string("Shockwave: ") + kShockNames[shock], 1.4);
        d.nextAt = m_time + 1.4;
        ++d.step;
        return;
    }
    switch (s - kStyleCount - 4)
    {
    case 0:
        d.server->SetLoad(1);
        SetStatus("Sustained load: a window being dragged.", 2.5);
        d.nextAt = m_time + 3.0;
        break;
    case 1:
        d.server->SetLoad(2);
        SetStatus("Sustained load: full-frame video.", 2.5);
        d.nextAt = m_time + 3.0;
        break;
    case 2:
    {
        d.server->SetLoad(0);
        // input, going out over the real connection: the server counts it
        for (uint32_t k = 0; k < 6; ++k)
        {
            const uint32_t ks = amber::vnc::KeysymFromCodePoint(U'a' + k);
            v.session->SendKey(true, ks);
            v.session->SendKey(false, ks);
        }
        for (int i = 0; i < 8; ++i)
            v.session->SendPointer(0, static_cast<uint16_t>(100 + i * 90), static_cast<uint16_t>(200 + i * 40));
        SetStatus("Keys and pointer events sent to the server.", 2.5);
        d.nextAt = m_time + 1.6;
        break;
    }
    case 3:
        d.server->Resize(800, 500);
        SetStatus("The server resized its desktop.", 2.5);
        d.nextAt = m_time + 2.6;
        break;
    default:
    {
        char msg[240];
        snprintf(msg, sizeof msg,
                 "Demonstration finished: %d updates decoded, %d key events and %d pointer positions reached the "
                 "server (motion coalesces to the newest position, by design). The desktop stays connected — use it.",
                 d.server->UpdatesSent(), d.server->KeyEvents(), d.server->PointerEvents());
        AddNotice(*tab, 0, msg);
        SetStatus("Demonstration finished. The desktop is yours.", 5.0);
        // The tab stays live, so the server has to as well: the demo object
        // is kept until the tab is closed, and DemoTick's own check above
        // stops the server the moment it is.
        d.finished = true;
        d.nextAt = m_time + 1.0;
        return;
    }
    }
    ++d.step;
}
