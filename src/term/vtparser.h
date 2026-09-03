// vtparser.h — xterm-256color escape-sequence state machine. Incoming colors
// are remapped to luminance for the amber intensity ramp.
#pragma once

#include "../common.h"
#include "ImageDecode.h"
#include "grid.h"
#include <functional>

struct TermModes
{
    bool appCursorKeys = false;   // DECCKM  ?1
    bool appKeypad = false;       // DECKPAM ESC = / DECKPNM ESC >
    bool autowrap = true;         // DECAWM  ?7
    bool bracketedPaste = false;  // ?2004
    bool insertMode = false;      // IRM 4
    // Mouse reporting: 0 = off, else the active protocol (1000 clicks,
    // 1002 clicks+drag, 1003 any motion). mouseSgr = ?1006 extended coords.
    int  mouseMode = 0;
    bool mouseSgr = false;
};

// Per-session behaviour switches (PuTTY's Terminal / Features / Translation
// / Colours pages). Defaults reproduce the terminal's historical behaviour.
struct TermFeatures
{
    bool allowAppCursor = true;        // honour DECCKM
    bool allowAppKeypad = true;        // honour DECKPAM
    bool allowMouse = true;            // honour ?1000-1003 / ?1006
    bool allowRemoteResize = true;     // honour CSI 8 ; rows ; cols t
    bool allowAltScreen = true;        // honour ?47 / ?1047 / ?1049
    bool allowRemoteTitle = true;      // honour OSC 0 / 2
    bool allowScrollbackClear = true;  // honour CSI 3 J
    bool allowAnsiColours = true;      // SGR 30-37 / 40-47 / 90-97 / 100-107
    bool allow256Colours = true;       // SGR 38 / 48 / 58 (256 and truecolor)
    bool implicitCrInLf = false;       // LF also returns the carriage
    bool implicitLfInCr = false;       // CR also feeds a line
    bool autowrapDefault = true;       // DECAWM state after a reset
    bool appCursorDefault = false;     // DECCKM state after a reset
    bool appKeypadDefault = false;     // DECKPAM state after a reset
    int  charset = 0;                  // 0 UTF-8, 1 Latin-1, 2 CP1252, 3 CP437
    bool poorMansLineDrawing = false;  // DEC graphics as + - |
    std::string answerback = "AmberSSH";   // reply to ENQ (0x05)
};

class VtParser
{
public:
    using WriteFn = std::function<void(const char*, size_t)>;   // replies → SSH
    using TitleFn = std::function<void(const std::string&)>;    // OSC 0/2
    using ClipFn  = std::function<void(const std::string&)>;    // OSC 52 base64
    // OSC 133 marks. hasCode is false when "133;D" arrived with no status
    // after it: the shell said the command ENDED, not that it succeeded, and
    // reporting that as exit 0 would invent a result.
    using MarkFn  = std::function<void(char, int, bool)>;
    using CwdFn   = std::function<void(const std::string&)>;    // OSC 7 cwd
    using ResizeFn = std::function<void(int cols, int rows)>;   // CSI 8;r;c t
    // Inline image at the cursor (Sixel / Kitty). cols/rows are the
    // requested cell span (0 = derive from pixels). Returns the number of
    // rows the cursor should advance (the sink knows the cell size).
    using ImageFn = std::function<int(amber::DecodedImage&&, int cols, int rows)>;

    VtParser(Grid& grid) : m_grid(grid) {}
    void SetWriter(WriteFn fn) { m_write = std::move(fn); }
    void SetTitleSink(TitleFn fn) { m_title = std::move(fn); }
    void SetClipboardSink(ClipFn fn) { m_clip = std::move(fn); }
    // Shell-integration marks: kind 'A' prompt start, 'B' command start,
    // 'C' output start, 'D' command finished (code = exit status).
    void SetMarkSink(MarkFn fn) { m_mark = std::move(fn); }
    // OSC 7 "file://host/path" — the shell's current directory (path only).
    void SetCwdSink(CwdFn fn) { m_cwd = std::move(fn); }
    void SetImageSink(ImageFn fn) { m_image = std::move(fn); }
    // Remote-controlled resize request (xterm CSI 8 t), when allowed.
    void SetResizeSink(ResizeFn fn) { m_resize = std::move(fn); }

    // Behaviour switches; applies the reset-state defaults immediately.
    void SetFeatures(const TermFeatures& f);
    const TermFeatures& Features() const { return m_feat; }

    void Feed(const uint8_t* data, size_t len);
    void Reset();
    const TermModes& Modes() const { return m_modes; }
    // Malformed UTF-8 sequences this parser has recovered from. Surfaced by
    // the debug overlay: a rising count means the far end is not sending what
    // it says it is.
    uint64_t Utf8Errors() const { return m_utfErrors; }
    // Live OSC 8 hyperlink targets held by this parser.
    size_t LinkCount() const { return m_links.size(); }
    // OSC 8 targets refused because they carried control characters.
    uint64_t LinkRejects() const { return m_linkRejects; }
    // Inline-image payloads that failed to decode (malformed or over budget).
    uint64_t ImageDecodeFails() const { return m_imageDecodeFails; }
    // OSC 8 hyperlink target for a Cell::link id (empty for 0 / unknown).
    const std::string& LinkUri(uint16_t id) const
    {
        static const std::string none;
        return (id > 0 && id <= m_links.size()) ? m_links[id - 1] : none;
    }

private:
    enum class State { Ground, Esc, EscInter, Csi, Osc, OscEsc };

    void HandleC0(uint8_t b);
    void HandleEsc(uint8_t b);
    void HandleCsi(uint8_t final);
    void DispatchString();       // routes a finished OSC / DCS / APC string
    void DispatchOsc();          // completed OSC payload (title, OSC 52)
    void DispatchDcs();          // DCS: Sixel images
    void DispatchApc();          // APC: Kitty graphics protocol
    void DispatchITerm();        // OSC 1337: iTerm2 inline images
    static std::string DecodeB64(const std::string& in);
    void HandleSgr();
    void HandleMode(bool set);
    void PrintChar(char32_t cp);
    void EmitReply(const std::string& s);
    // Consumes an extended-color argument list (38/48/58) starting after the
    // introducer at index i; returns the parsed color and advances i.
    amber::CellColor ParseExtendedColor(int& i);
    // Single-byte charset decode (Latin-1 / CP1252 / CP437) for bytes >= 0x80.
    char32_t DecodeHighByte(uint8_t b) const;

    Grid& m_grid;
    WriteFn m_write;
    TitleFn m_title;
    ClipFn m_clip;
    MarkFn m_mark;
    CwdFn m_cwd;
    ImageFn m_image;
    ResizeFn m_resize;
    TermFeatures m_feat;
    // Which string introducer opened m_oscBuf: 0 OSC (]), 1 DCS (P), 2 APC (_).
    int m_strKind = 0;
    // Kitty chunked transmission in progress (m=1 chunks accumulate here).
    std::string m_kittyCtl;      // control keys of the first chunk
    std::string m_kittyData;     // concatenated base64 payload
    TermModes m_modes;

    State m_state = State::Ground;

    // CSI parsing. m_paramSub marks parameters introduced by ':' — the
    // sub-parameter form modern applications emit for SGR 38/48/58.
    static constexpr int kMaxParams = 32;
    int m_params[kMaxParams] = {};
    bool m_paramSet[kMaxParams] = {};
    bool m_paramSub[kMaxParams] = {};
    int m_paramCount = 0;
    bool m_privateMode = false;
    char m_privateChar = 0;
    char m_intermediate = 0;

    // Longest control string we will retain: sized for Sixel images and
    // Kitty PNG chunks (a full-screen Sixel runs to several MB), still
    // bounded against a hostile server. Titles are clamped at dispatch.
    static constexpr size_t kMaxOscLen = 8u << 20;
    std::string m_oscBuf;
    char m_escInter = 0;

    // UTF-8 accumulation
    uint32_t m_utfCp = 0;
    uint32_t m_utfMin = 0;      // smallest value this length may legally encode
    int m_utfNeed = 0;
    uint64_t m_utfErrors = 0;   // invalid sequences recovered from (diagnostics)
    uint64_t m_linkRejects = 0; // OSC 8 targets refused for control characters
    uint64_t m_imageDecodeFails = 0;  // graphics payloads that would not decode

    // Current brush (SGR state)
    Cell m_brush;
    // OSC 8 link targets, interned; Cell::link is 1-based index.
    std::vector<std::string> m_links;

    // Charsets: G0/G1, DEC special graphics support
    bool m_charsetG0Graphics = false;
    bool m_charsetG1Graphics = false;
    bool m_useG1 = false;
};
