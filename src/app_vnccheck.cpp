// app_vnccheck.cpp — --vnc-selfcheck and --vnc-bench.
//
// The faithful contract, measured. An RFB server in-process
// (vnc/SelfCheckServer) serves a known picture; a VNC tab at solidity 1,
// density 1, size 1 draws it; the scene target — before bloom and
// composite, in linear light — is read back and every pixel compared with
// the decoded colour to within one 8-bit step after the sRGB curve. Then:
//
//   * one block is recoloured: the picture must follow it and nothing else
//     may move; the disturbance energy texture must be non-zero inside the
//     block and zero outside; a key and a pointer event are counted on the
//     far side;
//   * the desktop is resized (DesktopSize, then the full picture): the
//     buffers are rebuilt and the picture at the new size must be exact;
//   * 150 frames of sustained updates (a block dragged across the screen,
//     one update per request): when they stop, the picture must be exact.
//
// --vnc-bench serves a 3840x2160 desktop and measures frame times over
// three loads — static, a dragged block, and the whole frame every request
// — with vsync off. It records the hardware and the settings and claims
// nothing beyond the numbers.
//
// What neither checks, and says so: the composite stage (the scene is
// compared, not the swap chain), and anything a real server does
// differently from this one. Reports: %TEMP%\vnc-selfcheck.txt and
// %TEMP%\vnc-bench.txt; the self-check's exit code is 0 or 1.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "vnc/SelfCheckServer.h"

using amber::Session;
using amber::VncTab;
namespace vnc = amber::vnc;

namespace
{

float HalfToFloat(uint16_t v)
{
    const uint32_t sign = (v >> 15) & 1u, exp = (v >> 10) & 0x1Fu, mant = v & 0x3FFu;
    float f;
    if (exp == 0)
        f = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31)
        f = mant ? NAN : INFINITY;
    else
        f = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -f : f;
}

int LinearToSrgbByte(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(s * 255.0f));
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w)
        s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

} // namespace

struct App::VncSelfCheck
{
    static constexpr uint16_t kW = 320, kH = 200;
    static constexpr uint16_t kBlockX = 64, kBlockY = 48, kBlockW = 96, kBlockH = 64;
    static constexpr uint32_t kBlockColour = 0x0020A0FFu;   // not in the pattern
    static constexpr uint16_t kResizeW = 400, kResizeH = 260;
    static constexpr uint16_t kBenchW = 3840, kBenchH = 2160;
    static constexpr int kSustainFrames = 150;
    static constexpr int kBenchFrames = 240;

    bool bench = false;
    vnc::SelfCheckServer server;
    // check: 0 start, 1 first picture, 2 read, 3 after change, 4 read,
    // 5 resize, 6 read, 7 sustain, 8 settle, 9 read, 10 done
    // bench: 0 start, 1 first picture, 2 static, 3 drag, 4 video, 5 done
    int phase = 0;
    int framesInPhase = 0;
    uint32_t updatesAtSettle = 0;
    int settleFrames = 0;
    double startedAt = 0.0;
    int failures = 0;
    int tabIndex = -1;
    std::string report;

    struct Measure
    {
        const char* name = "";
        int frames = 0;
        double sumDt = 0, minDt = 1e9, maxDt = 0;
        double sumGpu = 0, sumUp = 0, sumSim = 0, sumDraw = 0, sumDecode = 0;
        uint64_t bytes0 = 0, bytes1 = 0;
        double t0 = 0, t1 = 0;
    } meas[3];

    void Check(const char* what, bool ok, const std::string& detail = {})
    {
        char line[640];
        snprintf(line, sizeof line, "%-72s %s  %s\n", what, ok ? "ok  " : "FAIL", detail.c_str());
        report += line;
        if (!ok)
            ++failures;
    }
    void Info(const std::string& text) { report += "    " + text + "\n"; }
};

// ---- shared pieces --------------------------------------------------------------
namespace
{

// Every pixel of the served picture against the scene target. Returns the
// detail line; `mismatches` and `maxDev` are filled in.
std::string ComparePicture(const std::vector<uint8_t>& raw, uint32_t W, uint32_t H, const VncTab& t,
                           const vnc::SelfCheckServer& server, int& mismatches, int& maxDev)
{
    mismatches = 0;
    maxDev = 0;
    long long checked = 0;
    std::string first;
    for (uint32_t y = 0; y < server.Height(); ++y)
        for (uint32_t x = 0; x < server.Width(); ++x)
        {
            const uint32_t e = server.PixelAt(x, y);
            const int er = (e >> 16) & 0xFF, eg = (e >> 8) & 0xFF, eb = e & 0xFF;
            const int sx = static_cast<int>(t.dstX) + static_cast<int>(x);
            const int sy = static_cast<int>(t.dstY) + static_cast<int>(y);
            ++checked;
            int dev = 255, gr = -1, gg = -1, gb = -1;
            if (sx >= 0 && sy >= 0 && sx < static_cast<int>(W) && sy < static_cast<int>(H))
            {
                const uint8_t* tx = raw.data() + (static_cast<size_t>(sy) * W + static_cast<size_t>(sx)) * 8;
                gr = LinearToSrgbByte(HalfToFloat(static_cast<uint16_t>(tx[0] | (tx[1] << 8))));
                gg = LinearToSrgbByte(HalfToFloat(static_cast<uint16_t>(tx[2] | (tx[3] << 8))));
                gb = LinearToSrgbByte(HalfToFloat(static_cast<uint16_t>(tx[4] | (tx[5] << 8))));
                dev = std::max({ std::abs(gr - er), std::abs(gg - eg), std::abs(gb - eb) });
            }
            maxDev = std::max(maxDev, dev);
            if (dev > 1)
            {
                ++mismatches;
                if (first.empty())
                    first = "; first at (" + std::to_string(x) + "," + std::to_string(y) + "): got " +
                            std::to_string(gr) + "," + std::to_string(gg) + "," + std::to_string(gb) + " expected " +
                            std::to_string(er) + "," + std::to_string(eg) + "," + std::to_string(eb);
            }
        }
    char d[200];
    snprintf(d, sizeof d, "%lld pixels, %d off by more than 1/255, max deviation %d", checked, mismatches, maxDev);
    return d + first;
}

} // namespace

void App::VncSelfCheckTick()
{
    if (!m_vncCheck)
    {
        m_vncCheck = new VncSelfCheck;
        VncSelfCheck& c = *m_vncCheck;
        c.bench = m_vncBenchRequested;
        m_vncSelfCheckRequested = false;
        m_vncBenchRequested = false;
        c.startedAt = m_time;
        const uint16_t w = c.bench ? VncSelfCheck::kBenchW : VncSelfCheck::kW;
        const uint16_t h = c.bench ? VncSelfCheck::kBenchH : VncSelfCheck::kH;
        const bool up = c.server.Start(w, h);
        c.Check("in-process RFB server started", up,
                up ? "127.0.0.1:" + std::to_string(c.server.Port()) + ", " + std::to_string(w) + "x" + std::to_string(h)
                   : "bind failed");
        if (!up)
        {
            c.phase = c.bench ? 5 : 10;
            return;
        }
        m_appearance = 0;
        if (c.bench)
            m_vsync = false;   // a frame-time measurement, not a refresh-rate one
        // debug builds: collect validation messages for the report rather
        // than breaking on the first one with no debugger to break into
        m_device.SetDebugBreaks(false);
        amber::ConnectionRequest req;
        req.profile.id = amber::MakeUuid();
        req.profile.name = c.bench ? "vnc bench" : "vnc self-check";
        req.profile.protocol = amber::Protocol::Vnc;
        req.profile.host = "127.0.0.1";
        req.profile.port = c.server.Port();
        req.profile.vncSolidity = 100;
        req.profile.vncDensity = 1;
        req.profile.vncParticleSize = 1;
        req.profile.vncDisturbance = 100;
        req.profile.vncCursorMode = 2;
        const bool started = StartSession(req);
        c.Check("VNC tab started", started);
        c.tabIndex = started ? m_active : -1;
        c.phase = started ? 1 : (c.bench ? 5 : 10);
        return;
    }

    VncSelfCheck& c = *m_vncCheck;
    const int done = c.bench ? 5 : 10;
    Session* s = (c.tabIndex >= 0 && c.tabIndex < static_cast<int>(m_sessions.size()))
                     ? m_sessions[static_cast<size_t>(c.tabIndex)].get()
                     : nullptr;
    VncTab* t = s ? s->vnc.get() : nullptr;
    if (c.phase < done && !t)
    {
        c.Check("the VNC tab is still there", false);
        c.phase = done;
    }
    const double limit = c.bench ? 120.0 : 60.0;
    if (c.phase < done && m_time - c.startedAt > limit)
    {
        c.Check("completed within the time limit", false,
                "phase " + std::to_string(c.phase) + ", tab " + (s ? SessionStateName(s->state) : "gone") +
                    (s ? ": " + s->status : std::string()));
        c.phase = done;
    }

    auto ready = [&] { return s->state == amber::SessionState::Connected && t->updatesSeen >= 1 && t->desk && t->desk->Ready(); };
    auto measure = [&](int idx, const char* name) {
        VncSelfCheck::Measure& m = c.meas[idx];
        if (m.frames == 0)
        {
            m.name = name;
            m.t0 = m_time;
            m.bytes0 = t->session->GetStats().bytesIn;
        }
        ++m.frames;
        m.sumDt += m_dt;
        m.minDt = std::min(m.minDt, static_cast<double>(m_dt));
        m.maxDt = std::max(m.maxDt, static_cast<double>(m_dt));
        m.sumGpu += m_device.GpuFrameMs();
        m.sumUp += m_device.GpuDesktopUploadMs();
        m.sumSim += m_device.GpuDesktopSimMs();
        m.sumDraw += m_device.GpuDesktopDrawMs();
        m.sumDecode += t->session->GetStats().decodeMsPerSec;
        m.t1 = m_time;
        m.bytes1 = t->session->GetStats().bytesIn;
    };

    if (c.bench)
    {
        switch (c.phase)
        {
        case 1:
            if (ready() && ++c.framesInPhase >= 60)
            {
                c.Check("connected and the first picture arrived", true,
                        std::to_string(t->fbW) + "x" + std::to_string(t->fbH));
                c.phase = 2;
                c.framesInPhase = 0;
            }
            break;
        case 2:
            measure(0, "static desktop");
            if (++c.framesInPhase >= VncSelfCheck::kBenchFrames)
            {
                c.server.SetLoad(1);
                t->session->RequestFullUpdate();   // make sure a request is outstanding
                c.phase = 3;
                c.framesInPhase = 0;
            }
            break;
        case 3:
            if (c.framesInPhase >= 20)   // let the stream establish first
                measure(1, "dragging a 400x300 block, one update per request");
            if (++c.framesInPhase >= VncSelfCheck::kBenchFrames + 20)
            {
                c.server.SetLoad(2);
                c.phase = 4;
                c.framesInPhase = 0;
            }
            break;
        case 4:
            if (c.framesInPhase >= 20)
                measure(2, "video: the whole 3840x2160 frame every request");
            if (++c.framesInPhase >= VncSelfCheck::kBenchFrames + 20)
            {
                c.server.SetLoad(0);
                c.phase = 5;
            }
            break;
        default:
            break;
        }
    }
    else
    {
        switch (c.phase)
        {
        case 1:
            if (ready() && ++c.framesInPhase >= 30)
            {
                c.Check("connected and the first picture arrived", true,
                        std::to_string(t->fbW) + "x" + std::to_string(t->fbH) + ", RFB 3." +
                            std::to_string(t->session->GetStats().rfbMinor));
                c.phase = 2;
            }
            break;
        case 3:
            if (t->updatesSeen >= 2 && ++c.framesInPhase >= 20)
                c.phase = 4;
            break;
        case 5:
            // after the DesktopSize: the tab follows the new size, then the full picture
            if (t->fbW == VncSelfCheck::kResizeW && t->fbH == VncSelfCheck::kResizeH && t->updatesSeen >= 4 &&
                t->desk && t->desk->Ready() && ++c.framesInPhase >= 30)
            {
                c.Check("the desktop resized and rebuilt its buffers", t->desk->GetLayout().fbW == VncSelfCheck::kResizeW,
                        std::to_string(t->desk->GetLayout().fbW) + "x" + std::to_string(t->desk->GetLayout().fbH) +
                            ", particles " + std::to_string(t->desk->GetLayout().particles));
                c.phase = 6;
            }
            break;
        case 7:
            if (++c.framesInPhase >= VncSelfCheck::kSustainFrames)
            {
                c.server.SetLoad(0);
                c.updatesAtSettle = t->updatesSeen;
                c.settleFrames = 0;
                c.phase = 8;
                c.framesInPhase = 0;
            }
            break;
        case 8:
            // wait until the update count has been still for 20 frames
            if (t->updatesSeen != c.updatesAtSettle)
            {
                c.updatesAtSettle = t->updatesSeen;
                c.settleFrames = 0;
            }
            else if (++c.settleFrames >= 20)
            {
                c.Info("sustained load: " + std::to_string(c.updatesAtSettle) + " updates received in total");
                c.phase = 9;
            }
            break;
        default:
            break;
        }
    }

    if (c.phase != done)
        return;

    // ---- the report -------------------------------------------------------------
    const DesktopParticles::Layout L = t && t->desk ? t->desk->GetLayout() : DesktopParticles::Layout{};
    char buf[512];
    snprintf(buf, sizeof buf, "adapter: %s, video memory %.0f MiB, window %ux%u, vsync %s",
             Narrow(m_device.AdapterName()).c_str(), static_cast<double>(m_device.VideoMemoryBytes()) / (1024.0 * 1024.0),
             m_device.Width(), m_device.Height(), m_vsync ? "on" : "off");
    c.Info(buf);
    snprintf(buf, sizeof buf,
             "layout: framebuffer %ux%u, particles %u, density %u%s, stride %u, sampled %ux%u, gpu bytes %.1f MiB, "
             "placement scale %.3f (%s)",
             L.fbW, L.fbH, L.particles, L.density, L.clamped ? " (clamped)" : "", L.stride, L.sampledW, L.sampledH,
             static_cast<double>(L.gpuBytes) / (1024.0 * 1024.0), t ? t->scale : 0.0f,
             t && t->nativeScale ? "native" : "scaled to fit the window");
    c.Info(buf);
    snprintf(buf, sizeof buf, "settings: solidity 100, density 1, particle size 1, disturbance 100, motion style %u, appearance dark",
             m_particles.tun.animStyle);
    c.Info(buf);
    if (c.bench)
    {
        for (const VncSelfCheck::Measure& m : c.meas)
        {
            if (m.frames == 0)
                continue;
            const double avgMs = m.sumDt / m.frames * 1000.0;
            const double secs = m.t1 - m.t0;
            snprintf(buf, sizeof buf,
                     "%-56s %4d frames  avg %6.2f ms (%5.1f fps)  min %5.2f  max %6.2f  |  gpu frame %5.2f  upload %5.2f  "
                     "sim %5.2f  draw %5.2f ms  |  decode %5.1f ms/s  net %6.1f MiB/s",
                     m.name, m.frames, avgMs, m.sumDt > 0 ? m.frames / m.sumDt : 0.0, m.minDt * 1000.0, m.maxDt * 1000.0,
                     m.sumGpu / m.frames, m.sumUp / m.frames, m.sumSim / m.frames, m.sumDraw / m.frames,
                     m.sumDecode / m.frames,
                     secs > 0 ? static_cast<double>(m.bytes1 - m.bytes0) / secs / (1024.0 * 1024.0) : 0.0);
            c.report += std::string(buf) + "\n";
        }
        c.Info("frame times are the application's own frame clock; GPU times are timestamp queries; "
               "decode is the worker's CPU time per wall second");
    }
    std::string dbg;
    const int errors = m_device.DrainDebugMessages(dbg);
    if (errors >= 0)
    {
        // errors and corruption fail the check; warnings and information
        // are listed by id with their counts, for reading
        c.Check("d3d12 debug layer reported no error-severity messages", errors == 0,
                std::to_string(errors) + " error(s)");
        if (!dbg.empty())
            c.report += dbg;
    }
    else
        c.Info(dbg.substr(0, dbg.size() - 1));
    c.report += c.bench ? "\nBENCH COMPLETE\n" : (c.failures ? "\nSELFCHECK FAILED\n" : "\nSELFCHECK PASSED\n");

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + (c.bench ? L"vnc-bench.txt" : L"vnc-selfcheck.txt");
    if (FILE* f = _wfopen(path.c_str(), L"wb"))
    {
        fwrite(c.report.data(), 1, c.report.size(), f);
        fclose(f);
    }
    const int code = (!c.bench && c.failures) ? 1 : 0;
    // The worker first: it joins and its socket closes, so a server thread
    // mid-frame sees a failed send instead of a client that has stopped
    // reading; then the server; then out. The tab itself stays — closing
    // the last tab opens the connection dialog, a modal, in front of the
    // WM_QUIT this is about to post — and goes with the application.
    if (t && t->session)
        t->session->Disconnect();
    c.server.Stop();
    delete m_vncCheck;
    m_vncCheck = nullptr;
    PostQuitMessage(code);
}

void App::VncSelfCheckAfterFrame()
{
    if (!m_vncCheck || m_vncCheck->bench)
        return;
    VncSelfCheck& c = *m_vncCheck;
    if (c.phase != 2 && c.phase != 4 && c.phase != 6 && c.phase != 9)
        return;
    Session& s = *m_sessions[static_cast<size_t>(c.tabIndex)];
    VncTab& t = *s.vnc;

    std::vector<uint8_t> raw;
    const uint32_t W = m_device.Width(), H = m_device.Height();
    const bool ok = m_device.ReadbackTexture(m_scene.Get(), m_sceneState, kSceneFormat, W, H, 8, raw);
    const char* stage = c.phase == 2 ? "" : c.phase == 4 ? " after the change" : c.phase == 6 ? " after the resize"
                                                                                  : " after sustained updates";
    c.Check((std::string("scene target read back") + stage).c_str(), ok);
    if (ok)
    {
        if (c.phase == 2)
            c.Check("framebuffer placed at native scale", t.nativeScale,
                    "scale " + std::to_string(t.scale) + " at (" + std::to_string(static_cast<int>(t.dstX)) + "," +
                        std::to_string(static_cast<int>(t.dstY)) + ")");
        int mismatches = 0, maxDev = 0;
        const std::string d = ComparePicture(raw, W, H, t, c.server, mismatches, maxDev);
        const char* what = c.phase == 2 ? "solidity 1 at native scale reproduces every decoded pixel (within 1/255)"
                           : c.phase == 4 ? "the changed block shows its new colour and nothing else moved"
                           : c.phase == 6 ? "after the resize the picture at the new size is exact"
                                          : "after 150 frames of sustained updates the picture is exact";
        c.Check(what, mismatches == 0, d);
    }

    switch (c.phase)
    {
    case 2:
        t.session->SendKey(true, 'a');
        t.session->SendKey(false, 'a');
        t.session->SendPointer(0, 10, 10);
        c.server.ChangeBlock(VncSelfCheck::kBlockX, VncSelfCheck::kBlockY, VncSelfCheck::kBlockW, VncSelfCheck::kBlockH,
                             VncSelfCheck::kBlockColour);
        c.phase = 3;
        c.framesInPhase = 0;
        return;
    case 4:
    {
        std::vector<float> e;
        uint32_t ew = 0, eh = 0;
        const bool eok = t.desk->ReadbackEnergy(e, ew, eh);
        c.Check("energy texture read back", eok, std::to_string(ew) + "x" + std::to_string(eh));
        if (eok)
        {
            float inside = 0.0f, outside = 0.0f;
            for (uint32_t y = 0; y < eh; ++y)
                for (uint32_t x = 0; x < ew; ++x)
                {
                    const float v = e[static_cast<size_t>(y) * ew + x];
                    const bool in = x >= VncSelfCheck::kBlockX && x < VncSelfCheck::kBlockX + VncSelfCheck::kBlockW &&
                                    y >= VncSelfCheck::kBlockY && y < VncSelfCheck::kBlockY + VncSelfCheck::kBlockH;
                    if (in) inside = std::max(inside, v);
                    else    outside = std::max(outside, v);
                }
            char d[128];
            snprintf(d, sizeof d, "max inside %.3f", inside);
            c.Check("changed pixels injected disturbance energy", inside > 0.05f, d);
            snprintf(d, sizeof d, "max outside %.4f", outside);
            c.Check("unchanged pixels injected none", outside < 0.01f, d);
        }
        c.Check("a key and a pointer event reached the server",
                c.server.KeyEvents() >= 2 && c.server.PointerEvents() >= 1,
                "keys " + std::to_string(c.server.KeyEvents()) + ", pointer " + std::to_string(c.server.PointerEvents()));
        c.server.Resize(VncSelfCheck::kResizeW, VncSelfCheck::kResizeH);
        c.phase = 5;
        c.framesInPhase = 0;
        return;
    }
    case 6:
        c.server.SetLoad(1);
        c.phase = 7;
        c.framesInPhase = 0;
        return;
    case 9:
        c.Check("one connection served the whole check", c.server.Accepted() == 1,
                "accepted " + std::to_string(c.server.Accepted()) + ", updates sent " +
                    std::to_string(c.server.UpdatesSent()));
        c.phase = 10;
        return;
    default:
        return;
    }
}
