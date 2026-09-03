#include "vtparser.h"

#include <algorithm>
#include <cstdlib>

using amber::CellColor;
using amber::ColPalette;
using amber::ColRgb;
using amber::kColorDefault;

// DEC Special Graphics (ESC ( 0) — the line-drawing charset ncurses may use.
static char32_t DecGraphics(char32_t c)
{
    switch (c)
    {
    case 'j': return U'┘'; case 'k': return U'┐'; case 'l': return U'┌';
    case 'm': return U'└'; case 'n': return U'┼'; case 'q': return U'─';
    case 't': return U'├'; case 'u': return U'┤'; case 'v': return U'┴';
    case 'w': return U'┬'; case 'x': return U'│'; case 'a': return U'▒';
    case '0': return U'█'; case '~': return U'·'; case '`': return U'◆';
    case 'f': return U'°'; case 'g': return U'±'; case 'o': return U'⎺';
    case 'p': return U'⎻'; case 'r': return U'⎼'; case 's': return U'⎽';
    case 'y': return U'≤'; case 'z': return U'≥'; case '{': return U'π';
    case '|': return U'≠'; case '}': return U'£'; default: return c;
    }
}

void VtParser::Reset()
{
    m_state = State::Ground;
    m_modes = TermModes{};
    m_modes.autowrap = m_feat.autowrapDefault;
    m_modes.appCursorKeys = m_feat.appCursorDefault && m_feat.allowAppCursor;
    m_modes.appKeypad = m_feat.appKeypadDefault && m_feat.allowAppKeypad;
    m_brush = Cell{};
    m_links.clear();
    m_strKind = 0;
    m_kittyCtl.clear();
    m_kittyData.clear();
    m_utfNeed = 0;
    m_utfCp = 0;
    m_charsetG0Graphics = m_charsetG1Graphics = false;
    m_useG1 = false;
    m_grid.ResetAll();
    m_grid.SetAutowrap(m_modes.autowrap);
}

void VtParser::SetFeatures(const TermFeatures& f)
{
    m_feat = f;
    // Reset-state defaults apply now; a running application keeps whatever
    // modes it already set unless the feature was just disallowed.
    m_modes.autowrap = f.autowrapDefault;
    m_grid.SetAutowrap(m_modes.autowrap);
    if (!f.allowAppCursor) m_modes.appCursorKeys = false;
    if (!f.allowAppKeypad) m_modes.appKeypad = false;
    if (!f.allowMouse) { m_modes.mouseMode = 0; m_modes.mouseSgr = false; }
    if (!f.allowAltScreen && m_grid.AltActive()) m_grid.ExitAlt();
    if (f.appCursorDefault && f.allowAppCursor) m_modes.appCursorKeys = true;
    if (f.appKeypadDefault && f.allowAppKeypad) m_modes.appKeypad = true;
}

// Single-byte charsets (Window > Translation). Bytes 0x80-0xFF only; ASCII
// is shared by every one of them.
char32_t VtParser::DecodeHighByte(uint8_t b) const
{
    static const char16_t kCp437[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
        0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
        0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
        0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
        0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
        0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
        0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
        0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
    };
    static const char16_t kCp1252Hi[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    switch (m_feat.charset)
    {
    case 3:  return kCp437[b - 0x80];
    case 2:  return (b < 0xA0) ? kCp1252Hi[b - 0x80] : static_cast<char32_t>(b);
    default: return static_cast<char32_t>(b);   // Latin-1
    }
}

void VtParser::EmitReply(const std::string& s)
{
    if (m_write)
        m_write(s.data(), s.size());
}

void VtParser::PrintChar(char32_t cp)
{
    bool graphics = m_useG1 ? m_charsetG1Graphics : m_charsetG0Graphics;
    if (graphics && cp >= 0x60 && cp <= 0x7E)
    {
        if (m_feat.poorMansLineDrawing)
        {
            // Window > Translation: "poor man's" line drawing (+ - |).
            switch (cp)
            {
            case 'q': cp = U'-'; break;
            case 'x': cp = U'|'; break;
            case 'a': cp = U'#'; break;
            case 'j': case 'k': case 'l': case 'm': case 'n':
            case 't': case 'u': case 'v': case 'w': cp = U'+'; break;
            default:  cp = DecGraphics(cp); break;
            }
        }
        else
            cp = DecGraphics(cp);
    }
    m_grid.PutChar(cp, m_brush, m_modes.insertMode);
}

void VtParser::DispatchString()
{
    switch (m_strKind)
    {
    case 1:  DispatchDcs(); break;
    case 2:  DispatchApc(); break;
    default: DispatchOsc(); break;
    }
    m_strKind = 0;
}

// DCS: "<params>q<sixel data>". Other DCS payloads (DECRQSS etc.) are
// consumed silently.
void VtParser::DispatchDcs()
{
    size_t q = m_oscBuf.find('q');
    if (q == std::string::npos || q > 16 || !m_image)
        return;
    for (size_t i = 0; i < q; ++i)
        if (!isdigit(static_cast<unsigned char>(m_oscBuf[i])) && m_oscBuf[i] != ';')
            return;
    amber::DecodedImage img;
    if (!amber::DecodeSixel(m_oscBuf.substr(q + 1), img))
        return;
    int rows = m_image(std::move(img), 0, 0);
    // Sixel leaves the cursor on the line after the image, at column 0.
    m_grid.CarriageReturn();
    for (int r = 0; r < rows; ++r)
        m_grid.LineFeed();
}

// APC "G<k=v,...>;<base64>" — Kitty graphics. Supports direct transmission
// (t=d) of PNG (f=100) and raw RGB/RGBA (f=24/32), chunked (m=1), with
// transmit+display (a=T). Replies OK when an image id was given so
// clients that wait for acknowledgement (kitty icat) proceed.
void VtParser::DispatchApc()
{
    if (m_oscBuf.size() < 2 || m_oscBuf[0] != 'G')
        return;
    size_t semi = m_oscBuf.find(';');
    std::string ctl = m_oscBuf.substr(1, semi == std::string::npos ? std::string::npos : semi - 1);
    std::string data = semi == std::string::npos ? std::string() : m_oscBuf.substr(semi + 1);

    auto key = [](const std::string& c, char k, const std::string& def) -> std::string {
        size_t pos = 0;
        while (pos < c.size())
        {
            size_t comma = c.find(',', pos);
            std::string kv = c.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (kv.size() >= 2 && kv[0] == k && kv[1] == '=')
                return kv.substr(2);
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        return def;
    };

    // Chunk accumulation: the first chunk carries the control keys.
    bool more = key(ctl, 'm', "0") == "1";
    if (m_kittyData.empty() && m_kittyCtl.empty())
        m_kittyCtl = ctl;
    if (m_kittyData.size() + data.size() <= (16u << 20))
        m_kittyData += data;
    if (more)
        return;
    std::string allCtl = m_kittyCtl, allData = m_kittyData;
    m_kittyCtl.clear();
    m_kittyData.clear();

    std::string action = key(allCtl, 'a', "t");
    std::string id = key(allCtl, 'i', "");
    bool quiet = key(allCtl, 'q', "0") != "0";
    auto reply = [&](const char* msg) {
        if (!id.empty() && !quiet && m_write)
        {
            std::string r = "\x1b_Gi=" + id + ";" + msg + "\x1b\\";
            m_write(r.data(), r.size());
        }
    };
    if (action == "d")
        return;   // delete: images are transient here; nothing to do
    if (action != "T" && action != "t" && action != "p")
    {
        reply("ENOTSUPPORTED");
        return;
    }
    if (key(allCtl, 't', "d") != "d")
    {
        reply("EBADF: only direct transmission is supported");
        return;
    }
    if (key(allCtl, 'o', "") == "z")
    {
        reply("ENOTSUPPORTED: compression");
        return;
    }
    // Base64 → bytes.
    std::string bytes;
    {
        auto val = [](char ch) -> int {
            if (ch >= 'A' && ch <= 'Z') return ch - 'A';
            if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
            if (ch >= '0' && ch <= '9') return ch - '0' + 52;
            if (ch == '+') return 62;
            if (ch == '/') return 63;
            return -1;
        };
        bytes.reserve(allData.size() * 3 / 4);
        int acc = 0, bits = 0;
        for (char ch : allData)
        {
            int v = val(ch);
            if (v < 0) continue;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                bytes.push_back(static_cast<char>((acc >> bits) & 0xFF));
            }
        }
    }
    int fmt = atoi(key(allCtl, 'f', "32").c_str());
    amber::DecodedImage img;
    bool ok;
    if (fmt == 100)
        ok = amber::DecodePng(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), img);
    else
        ok = amber::DecodeKittyRaw(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), fmt,
                            atoi(key(allCtl, 's', "0").c_str()),
                            atoi(key(allCtl, 'v', "0").c_str()), img);
    if (!ok)
    {
        reply("EINVAL: could not decode image");
        return;
    }
    if (action == "t")
    {
        reply("OK");   // transmitted without display: nothing kept (transient)
        return;
    }
    if (m_image)
    {
        int cols = atoi(key(allCtl, 'c', "0").c_str());
        int rows = atoi(key(allCtl, 'r', "0").c_str());
        int adv = m_image(std::move(img), cols, rows);
        // C=1 asks the terminal not to move the cursor.
        if (key(allCtl, 'C', "0") != "1")
        {
            for (int r = 0; r < adv; ++r)
                m_grid.LineFeed();
        }
    }
    reply("OK");
}

void VtParser::DispatchOsc()
{
    // OSC 0/2 — window title. The buffer cap is sized for OSC 52 clipboard
    // payloads; a title is clamped to something a title bar could ever show.
    if (m_oscBuf.size() >= 2 &&
        (m_oscBuf[0] == '0' || m_oscBuf[0] == '2') && m_oscBuf[1] == ';')
    {
        if (m_title && m_feat.allowRemoteTitle)
            m_title(m_oscBuf.substr(2, 1024));
        return;
    }
    // OSC 7 — current working directory: "7;file://host/percent-encoded-path".
    if (m_oscBuf.rfind("7;", 0) == 0 && m_cwd)
    {
        std::string uri = m_oscBuf.substr(2);
        std::string path = uri;
        if (uri.rfind("file://", 0) == 0)
        {
            size_t slash = uri.find('/', 7);
            path = (slash == std::string::npos) ? "/" : uri.substr(slash);
        }
        std::string dec;
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == '%' && i + 2 < path.size())
            {
                dec.push_back(static_cast<char>(strtoul(path.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            }
            else
                dec.push_back(path[i]);
        }
        m_cwd(dec);
        return;
    }
    // OSC 8 — hyperlink: "8;params;URI" starts a link span, "8;;" ends it.
    // Targets are interned so every cell only carries a small id.
    if (m_oscBuf.rfind("8;", 0) == 0)
    {
        size_t semi = m_oscBuf.find(';', 2);
        std::string uri = (semi == std::string::npos) ? std::string()
                                                       : m_oscBuf.substr(semi + 1);
        if (uri.empty() || uri.size() > 2048)
        {
            m_brush.link = 0;
            return;
        }
        uint16_t id = 0;
        for (size_t i = 0; i < m_links.size(); ++i)
            if (m_links[i] == uri)
            {
                id = static_cast<uint16_t>(i + 1);
                break;
            }
        if (!id && m_links.size() < 4000)
        {
            m_links.push_back(uri);
            id = static_cast<uint16_t>(m_links.size());
        }
        m_brush.link = id;
        return;
    }
    // OSC 133 — FinalTerm shell-integration marks: "133;A" prompt start,
    // "133;B" command start, "133;C" output start, "133;D;<code>" finished.
    if (m_oscBuf.rfind("133;", 0) == 0 && m_oscBuf.size() >= 5)
    {
        char kind = m_oscBuf[4];
        int code = 0;
        if (kind == 'D' && m_oscBuf.size() >= 7 && m_oscBuf[5] == ';')
            code = atoi(m_oscBuf.c_str() + 6);
        if (m_mark)
            m_mark(kind, code);
        return;
    }
    // OSC 52 — remote clipboard write: "52;<targets>;<base64>". Only writes
    // are honored; a '?' query is dropped so a remote host can never READ
    // the local clipboard.
    if (m_oscBuf.rfind("52;", 0) == 0)
    {
        size_t semi = m_oscBuf.find(';', 3);
        if (semi != std::string::npos && m_clip)
        {
            std::string b64 = m_oscBuf.substr(semi + 1);
            if (b64 != "?")
                m_clip(b64);
        }
    }
}

void VtParser::HandleC0(uint8_t b)
{
    switch (b)
    {
    case 0x07: m_grid.bellPending = true; break;     // BEL — visual bell
    case 0x05:                                       // ENQ -> answerback
        if (!m_feat.answerback.empty())
            EmitReply(m_feat.answerback);
        break;
    case 0x08: m_grid.Backspace(); break;
    case 0x09: m_grid.Tab(); break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        m_grid.LineFeed();
        if (m_feat.implicitCrInLf)                   // Terminal: CR in every LF
            m_grid.CarriageReturn();
        break;
    case 0x0D:
        m_grid.CarriageReturn();
        if (m_feat.implicitLfInCr)                   // Terminal: LF in every CR
            m_grid.LineFeed();
        break;
    case 0x0E: m_useG1 = true; break;                // SO
    case 0x0F: m_useG1 = false; break;               // SI
    default: break;
    }
}

void VtParser::Feed(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        uint8_t b = data[i];

        // OSC consumes everything until BEL or ST.
        if (m_state == State::Osc)
        {
            if (b == 0x07)
            {
                m_state = State::Ground;
                DispatchString();
            }
            else if (b == 0x1B)
                m_state = State::OscEsc;
            else
                // Bounded: a hostile or broken server must not be able to
                // grow this buffer without limit. Excess bytes are dropped;
                // the sequence still terminates normally.
                if (m_oscBuf.size() < kMaxOscLen)
                    m_oscBuf.push_back(static_cast<char>(b));
            continue;
        }
        if (m_state == State::OscEsc)
        {
            if (b == '\\')   // ST
            {
                m_state = State::Ground;
                DispatchString();
            }
            else
            {
                m_state = State::Osc;
                // Bounded: a hostile or broken server must not be able to
                // grow this buffer without limit. Excess bytes are dropped;
                // the sequence still terminates normally.
                if (m_oscBuf.size() < kMaxOscLen)
                    m_oscBuf.push_back(static_cast<char>(b));
            }
            continue;
        }

        // C0 controls are handled in any non-OSC state (except ESC starts).
        if (b == 0x1B)
        {
            m_state = State::Esc;
            m_escInter = 0;
            m_utfNeed = 0;
            continue;
        }
        if (b < 0x20)
        {
            if (b == 0x18 || b == 0x1A)   // CAN/SUB abort sequences
            {
                m_state = State::Ground;
                continue;
            }
            HandleC0(b);
            continue;
        }

        switch (m_state)
        {
        case State::Ground:
        {
            // UTF-8 decode.
            if (m_utfNeed > 0)
            {
                if ((b & 0xC0) == 0x80)
                {
                    m_utfCp = (m_utfCp << 6) | (b & 0x3F);
                    if (--m_utfNeed == 0)
                        PrintChar(m_utfCp > 0x10FFFF ? U'�' : m_utfCp);
                }
                else
                {
                    m_utfNeed = 0;
                    PrintChar(U'�');
                    --i;   // reprocess this byte
                }
            }
            else if (b < 0x80)
                PrintChar(b);
            else if (m_feat.charset != 0)
                PrintChar(DecodeHighByte(b));       // single-byte charset
            else if ((b & 0xE0) == 0xC0) { m_utfCp = b & 0x1F; m_utfNeed = 1; }
            else if ((b & 0xF0) == 0xE0) { m_utfCp = b & 0x0F; m_utfNeed = 2; }
            else if ((b & 0xF8) == 0xF0) { m_utfCp = b & 0x07; m_utfNeed = 3; }
            else
                PrintChar(U'�');
            break;
        }
        case State::Esc:
            HandleEsc(b);
            break;
        case State::EscInter:
            // Charset designation: ESC ( X or ESC ) X
            if (m_escInter == '(')
                m_charsetG0Graphics = (b == '0');
            else if (m_escInter == ')')
                m_charsetG1Graphics = (b == '0');
            m_state = State::Ground;
            break;
        case State::Csi:
            if (b >= '0' && b <= '9')
            {
                if (m_paramCount == 0)
                    m_paramCount = 1;
                int& p = m_params[m_paramCount - 1];
                p = std::min(p * 10 + (b - '0'), 65535);
                m_paramSet[m_paramCount - 1] = true;
            }
            else if (b == ';' || b == ':')
            {
                // ':' introduces a sub-parameter (SGR 38:2::r:g:b form).
                if (m_paramCount < kMaxParams)
                    ++m_paramCount;
                if (m_paramCount == 1)
                    m_paramCount = 2;   // leading separator: first param empty
                if (m_paramCount <= kMaxParams)
                    m_paramSub[m_paramCount - 1] = (b == ':');
            }
            else if (b == '?' || b == '>' || b == '<' || b == '=')
            {
                m_privateMode = true;
                m_privateChar = static_cast<char>(b);
            }
            else if (b >= 0x20 && b <= 0x2F)
                m_intermediate = static_cast<char>(b);
            else if (b >= 0x40 && b <= 0x7E)
            {
                HandleCsi(b);
                m_state = State::Ground;
            }
            else
                m_state = State::Ground;
            break;
        default:
            m_state = State::Ground;
            break;
        }
    }
}

void VtParser::HandleEsc(uint8_t b)
{
    switch (b)
    {
    case '[':
        m_state = State::Csi;
        memset(m_params, 0, sizeof(m_params));
        memset(m_paramSet, 0, sizeof(m_paramSet));
        memset(m_paramSub, 0, sizeof(m_paramSub));
        m_paramCount = 0;
        m_privateMode = false;
        m_privateChar = 0;
        m_intermediate = 0;
        return;
    case ']':
        m_strKind = 0;
        m_state = State::Osc;
        m_oscBuf.clear();
        return;
    case 'P':   // DCS — Sixel graphics (and anything else we ignore)
        m_strKind = 1;
        m_state = State::Osc;
        m_oscBuf.clear();
        return;
    case '_':   // APC — Kitty graphics protocol
        m_strKind = 2;
        m_state = State::Osc;
        m_oscBuf.clear();
        return;
    case '(':
    case ')':
        m_state = State::EscInter;
        m_escInter = static_cast<char>(b);
        return;
    case '7': m_grid.SaveCursor(); break;
    case '8': m_grid.RestoreCursor(); break;
    case 'D': m_grid.LineFeed(); break;              // IND
    case 'E': m_grid.NextLine(); break;              // NEL
    case 'M': m_grid.ReverseIndex(); break;          // RI
    case 'H': m_grid.SetTabStop(); break;            // HTS
    case 'c': Reset(); break;                        // RIS
    case 'Z': EmitReply("\x1b[?62c"); break;         // DECID
    case '=': if (m_feat.allowAppKeypad) m_modes.appKeypad = true; break;   // DECKPAM
    case '>': m_modes.appKeypad = false; break;                             // DECKPNM                       // keypad modes — ignored
    case '\\': break;                                // stray ST
    default: break;
    }
    m_state = State::Ground;
}

void VtParser::HandleMode(bool set)
{
    for (int i = 0; i < std::max(1, m_paramCount); ++i)
    {
        int p = (i < m_paramCount) ? m_params[i] : 0;
        if (m_privateMode)
        {
            switch (p)
            {
            case 1:    if (m_feat.allowAppCursor) m_modes.appCursorKeys = set; break;
            case 7:    m_modes.autowrap = set; m_grid.SetAutowrap(set); break;
            case 12:   break;                          // cursor blink — always on
            case 25:   m_grid.SetCursorVisible(set); break;
            case 47:
            case 1047:
                if (!m_feat.allowAltScreen) break;
                if (set) m_grid.EnterAlt(); else m_grid.ExitAlt();
                break;
            case 1048:
                if (set) m_grid.SaveCursor(); else m_grid.RestoreCursor();
                break;
            case 1049:
                if (!m_feat.allowAltScreen) break;
                if (set)
                {
                    m_grid.SaveCursor();
                    m_grid.EnterAlt();
                    m_grid.EraseDisplay(2, m_brush);
                }
                else
                {
                    m_grid.ExitAlt();
                    m_grid.RestoreCursor();
                }
                break;
            case 1000: case 1002: case 1003:
                // Highest-numbered protocol wins while set; reset turns
                // reporting off entirely (apps reset what they enabled).
                m_modes.mouseMode = (set && m_feat.allowMouse) ? p : 0;
                break;
            case 1006:
                m_modes.mouseSgr = set && m_feat.allowMouse;   // SGR extended coords
                break;
            case 1005: case 1015:
                break;                                 // legacy encodings

            case 2004: m_modes.bracketedPaste = set; break;
            default: break;
            }
        }
        else
        {
            switch (p)
            {
            case 4: m_modes.insertMode = set; break;   // IRM
            default: break;
            }
        }
    }
}

// Parses the argument list of SGR 38/48/58 starting after the introducer.
// Handles both the semicolon form (38;5;n / 38;2;r;g;b) and the colon
// sub-parameter form (38:5:n / 38:2:r:g:b / 38:2::r:g:b with a colorspace id).
// On success returns the color and leaves i on the last consumed parameter;
// on a malformed list returns kColorDefault having consumed what it examined.
amber::CellColor VtParser::ParseExtendedColor(int& i)
{
    bool colonForm = (i + 1 < m_paramCount) && m_paramSub[i + 1];

    // Collect the argument run. In colon form it is exactly the consecutive
    // sub-parameters; in semicolon form we read positionally.
    int args[6];
    int argSet[6];
    int argCount = 0;
    int j = i + 1;
    while (j < m_paramCount && argCount < 6)
    {
        if (colonForm && !m_paramSub[j])
            break;
        args[argCount] = m_params[j];
        argSet[argCount] = m_paramSet[j] ? 1 : 0;
        ++argCount;
        ++j;
    }
    if (argCount == 0)
        return kColorDefault;

    if (args[0] == 5 && argCount >= 2)
    {
        i += 2;
        return ColPalette(static_cast<uint32_t>(std::clamp(args[1], 0, 255)));
    }
    if (args[0] == 2)
    {
        // Colon form may carry a colorspace id: 38:2:<cs>:r:g:b (five args,
        // the id slot usually empty — 38:2::r:g:b). Four args means no id.
        // The semicolon form is always positional: 38;2;r;g;b.
        int base = (colonForm && argCount >= 5) ? 2 : 1;
        if (base + 2 < argCount || (!colonForm && argCount >= 4))
        {
            i += colonForm ? argCount : 4;
            return ColRgb(std::clamp(args[base], 0, 255),
                          std::clamp(args[base + 1], 0, 255),
                          std::clamp(args[base + 2], 0, 255));
        }
    }
    (void)argSet;
    // Unknown colorspace — consume the colon run so we do not misparse the
    // remaining parameters as separate SGR codes.
    if (colonForm)
        i += argCount;
    return kColorDefault;
}

void VtParser::HandleSgr()
{
    // SGR reset never ends a hyperlink span — links are OSC 8 state, not
    // rendition state.
    auto resetBrush = [&]() {
        uint16_t link = m_brush.link;
        m_brush = Cell{};
        m_brush.link = link;
    };
    if (m_paramCount == 0)
    {
        resetBrush();
        return;
    }
    for (int i = 0; i < m_paramCount; ++i)
    {
        int p = m_paramSet[i] ? m_params[i] : 0;
        switch (p)
        {
        case 0: resetBrush(); break;
        case 1: m_brush.attr |= AttrBold; m_brush.attr &= ~AttrDim; break;
        case 2: m_brush.attr |= AttrDim; m_brush.attr &= ~AttrBold; break;
        case 3: m_brush.attr |= AttrItalic; break;
        case 4:
            // 4:0 = no underline, 4:2 = double (colon sub-parameter form).
            if (i + 1 < m_paramCount && m_paramSub[i + 1])
            {
                int style = m_params[i + 1];
                ++i;
                m_brush.attr &= ~(AttrUnderline | AttrDblUnder);
                if (style == 2)
                    m_brush.attr |= AttrDblUnder;
                else if (style != 0)
                    m_brush.attr |= AttrUnderline;
            }
            else
                m_brush.attr |= AttrUnderline;
            break;
        case 5:
        case 6: m_brush.attr |= AttrBlink; break;
        case 7: m_brush.attr |= AttrInverse; break;
        case 8: m_brush.attr |= AttrConceal; break;
        case 9: m_brush.attr |= AttrStrike; break;
        case 21: m_brush.attr |= AttrDblUnder; break;    // double underline
        case 22: m_brush.attr &= ~(AttrBold | AttrDim); break;
        case 23: m_brush.attr &= ~AttrItalic; break;
        case 24: m_brush.attr &= ~(AttrUnderline | AttrDblUnder); break;
        case 25: m_brush.attr &= ~AttrBlink; break;
        case 27: m_brush.attr &= ~AttrInverse; break;
        case 28: m_brush.attr &= ~AttrConceal; break;
        case 29: m_brush.attr &= ~AttrStrike; break;
        case 39: m_brush.fg = kColorDefault; break;
        case 49: m_brush.bg = kColorDefault; break;
        case 59: m_brush.ul = kColorDefault; break;
        case 38: { amber::CellColor c = ParseExtendedColor(i); if (m_feat.allow256Colours) m_brush.fg = c; break; }
        case 48: { amber::CellColor c = ParseExtendedColor(i); if (m_feat.allow256Colours) m_brush.bg = c; break; }
        case 58: { amber::CellColor c = ParseExtendedColor(i); if (m_feat.allow256Colours) m_brush.ul = c; break; }
        default:
            if (!m_feat.allowAnsiColours)
                break;                                // Colours: ANSI colours disallowed
            if (p >= 30 && p <= 37)
                m_brush.fg = ColPalette(static_cast<uint32_t>(p - 30));
            else if (p >= 90 && p <= 97)
                m_brush.fg = ColPalette(static_cast<uint32_t>(p - 90 + 8));
            else if (p >= 40 && p <= 47)
                m_brush.bg = ColPalette(static_cast<uint32_t>(p - 40));
            else if (p >= 100 && p <= 107)
                m_brush.bg = ColPalette(static_cast<uint32_t>(p - 100 + 8));
            break;
        }
    }
}

void VtParser::HandleCsi(uint8_t final)
{
    // Normalize: at least one param slot for indexing.
    if (m_paramCount == 0)
        m_paramCount = 1;

    auto p1 = [&](int def = 1) {
        return (m_paramSet[0] && m_params[0] > 0) ? m_params[0] : def;
    };

    switch (final)
    {
    case 'A': m_grid.MoveCursor(0, -p1()); break;                   // CUU
    case 'B': m_grid.MoveCursor(0, p1()); break;                    // CUD
    case 'C': m_grid.MoveCursor(p1(), 0); break;                    // CUF
    case 'D': m_grid.MoveCursor(-p1(), 0); break;                   // CUB
    case 'E': m_grid.SetCursor(0, m_grid.CurY() + p1()); break;     // CNL
    case 'F': m_grid.SetCursor(0, m_grid.CurY() - p1()); break;     // CPL
    case 'G': m_grid.SetCol(p1() - 1); break;                       // CHA
    case '`': m_grid.SetCol(p1() - 1); break;                       // HPA
    case 'd': m_grid.SetRow(p1() - 1); break;                       // VPA
    case 'H':                                                        // CUP
    case 'f':
    {
        int row = (m_paramSet[0] && m_params[0] > 0) ? m_params[0] : 1;
        int col = (m_paramCount > 1 && m_paramSet[1] && m_params[1] > 0) ? m_params[1] : 1;
        m_grid.SetCursor(col - 1, row - 1);
        break;
    }
    case 'J':
    {
        int mode = m_paramSet[0] ? m_params[0] : 0;
        if (mode == 3 && !m_feat.allowScrollbackClear)
            break;                                    // Features: keep scrollback
        m_grid.EraseDisplay(mode, m_brush);
        break;
    }
    case 'K': m_grid.EraseLine(m_paramSet[0] ? m_params[0] : 0, m_brush); break;
    case 'L': m_grid.InsertLines(p1(), m_brush); break;
    case 'M': m_grid.DeleteLines(p1(), m_brush); break;
    case 'P': m_grid.DeleteChars(p1(), m_brush); break;
    case '@': m_grid.InsertChars(p1(), m_brush); break;
    case 'X': m_grid.EraseChars(p1(), m_brush); break;
    case 'S': m_grid.ScrollUp(p1(), m_brush); break;
    case 'T': m_grid.ScrollDown(p1(), m_brush); break;
    case 'g': m_grid.ClearTabStop((m_paramSet[0] ? m_params[0] : 0) == 3); break;
    case 'h': HandleMode(true); break;
    case 'l': HandleMode(false); break;
    case 'm': HandleSgr(); break;
    case 'r':                                                        // DECSTBM
    {
        int top = (m_paramSet[0] && m_params[0] > 0) ? m_params[0] : 1;
        int bot = (m_paramCount > 1 && m_paramSet[1] && m_params[1] > 0)
                      ? m_params[1] : m_grid.Rows();
        m_grid.SetScrollRegion(top - 1, bot - 1);
        break;
    }
    case 's': m_grid.SaveCursor(); break;
    case 'u': m_grid.RestoreCursor(); break;
    case 'c':                                                        // DA
        if (!m_privateMode)
            EmitReply("\x1b[?62;22c");                               // primary
        else if (m_privateChar == '>')
            EmitReply("\x1b[>1;10;0c");                              // secondary
        break;
    case 'n':                                                        // DSR
    {
        int p = m_paramSet[0] ? m_params[0] : 0;
        if (p == 5)
            EmitReply("\x1b[0n");
        else if (p == 6)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                     m_grid.CurY() + 1, m_grid.CurX() + 1);
            EmitReply(buf);
        }
        break;
    }
    case 't':                                                        // window ops
        // CSI 8 ; rows ; cols t = resize the terminal (xterm), when allowed.
        if (m_paramSet[0] && m_params[0] == 8 && m_paramCount >= 3 &&
            m_feat.allowRemoteResize && m_resize)
        {
            int rows = m_params[1], cols = m_params[2];
            if (rows > 0 && cols > 0)
                m_resize(cols, rows);
        }
        break;
    default: break;
    }
}
