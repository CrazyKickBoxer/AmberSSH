// Session.h — one SSH tab: its own terminal grid, parser, worker thread,
// selection state and connection status. Sessions are independent; a stalled
// or disconnected session must never block another.
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../profiles/ConnectionProfile.h"
#include "../ssh/session.h"
#include "../term/ImageDecode.h"
#include "../term/grid.h"
#include "../term/vtparser.h"
#include "../utility/SecureString.h"

namespace amber
{

enum class SessionState
{
    Connecting,
    VerifyingHost,
    Authenticating,
    Connected,
    Reconnecting,
    Disconnected,
    Error,
};

inline const char* SessionStateName(SessionState s)
{
    switch (s)
    {
    case SessionState::Connecting:     return "connecting";
    case SessionState::VerifyingHost:  return "verifying host";
    case SessionState::Authenticating: return "authenticating";
    case SessionState::Connected:      return "connected";
    case SessionState::Reconnecting:   return "reconnecting";
    case SessionState::Disconnected:   return "disconnected";
    case SessionState::Error:          return "error";
    }
    return "unknown";
}

struct Session
{
    // --- terminal ----------------------------------------------------------
    Grid grid;
    VtParser parser{ grid };
    SshSession ssh;

    // --- identity ----------------------------------------------------------
    ConnectionProfile profile;
    std::string label;        // "user@host" — the default tab caption
    std::string remoteTitle;  // OSC 0/2 title, used when the setting allows it
    std::string status;       // last status or error text

    SessionState state = SessionState::Connecting;
    bool unread = false;      // output arrived while this tab was not active
    int themeOverride = -1;   // per-host danger theme (-1 = user's theme)
    double bornAt = 0.0;      // app time at creation — phosphor warm-up anchor
    std::string cwd;          // remote working directory (OSC 7), for previews

    // --- live effects (shell-integration marks, command timing, echo) --------
    struct Mark { uint64_t rowId; char kind; };   // 'A' prompt, 'C' output start
    std::vector<Mark> marks;
    bool cmdRunning = false;     // between OSC 133 C and D
    double cmdStart = 0.0;
    // --- command journal capture (OSC 133 B -> C -> D) ---------------------
    // 'B' marks where the prompt ends and the user's typing begins; the text
    // between there and the cursor at 'C' is the command itself.
    uint64_t promptRowId = 0;    // absolute row of the 'B' mark
    int promptCol = -1;          // column of the 'B' mark, -1 = none pending
    std::string pendingCmd;      // lifted at 'C', filed at 'D'
    std::string pendingCwd;      // the remote directory it ran in
    int64_t pendingStartedAt = 0;   // Unix seconds

    // --- output folding ----------------------------------------------------
    // One fold per completed command: the rows its output occupies, plus the
    // summary shown in their place when it is collapsed. Rows are ABSOLUTE
    // ids (TotalPushed + screen row) so they survive scrolling.
    struct Fold
    {
        uint64_t firstRow = 0;    // first output row, inclusive
        uint64_t lastRow = 0;     // last output row, inclusive
        int exitCode = 0;
        double durationSec = 0.0;
        int lines = 0;
        bool collapsed = false;
        std::u32string summary;   // rendered in place of the folded rows
    };
    uint64_t outputStartRow = 0;   // absolute row of the OSC 133 'C' mark
    std::vector<Fold> folds;
    // Display row -> what to draw there, rebuilt each frame. src is a raw
    // grid view row when foldIndex < 0, otherwise the fold to summarise.
    struct RowSlot { int src = 0; int foldIndex = -1; };
    std::vector<RowSlot> rowMap;
    bool AnyCollapsed() const
    {
        for (const Fold& f : folds)
            if (f.collapsed)
                return true;
        return false;
    }
    bool echoPending = false;    // keystroke sent, no output back yet
    double echoSentAt = 0.0;
    double echoClosedAt = -1.0;  // when the ring snapped shut
    float echoRttMs = 0.0f;
    double lastOutputAt = 0.0;   // tab activity spark
    double lastTriggerAt = -1e9;
    struct Ember { uint64_t rowId; double t; bool severe; };
    std::vector<Ember> embers;   // error lines smouldering red

    // --- phosphor persistence: previous frame's glyph per cell, and the
    //     afterimages of glyphs that were replaced -------------------------
    std::vector<char32_t> prevCp;
    std::vector<uint32_t> prevRgb;    // bit 24 = explicit fg, low 24 = sRGB
    uint64_t prevPushed = 0;
    bool prevAlt = false;
    int prevViewOffset = 0;
    // rate: 1 on the main screen; 10 inside a full-screen app, whose ghosts
    // must clear as fast as its values change.
    struct After { float x, y; char32_t cp; float rgb[3]; double t; float rate; };
    std::vector<After> afterimages;

    // --- PuTTY-style per-profile runtime state --------------------------------
    std::string lineBuf;         // local line editing: the line being composed
    bool lineEditing = false;    // effective "local line editing" this session
    bool localEcho = false;      // effective "local echo" this session
    int bellBurst = 0;           // bells inside the current overload window
    double bellBurstStart = 0.0;
    double bellMutedUntil = -1.0;
    bool selRect = false;        // rectangular (column) selection in progress
    bool closeRequested = false; // "close window on exit" fired for this tab

    // --- full-screen redraw debounce ------------------------------------------
    // ncurses apps (htop, vim) redraw as erase+rewrite bursts that can straddle
    // frames; a cell only commits to the renderer once its content has been
    // stable for a few frames, so transient states never trigger motion.
    std::vector<Cell> dbLast, dbCommitted;
    std::vector<double> dbSince;

    // --- selection (view coordinates) --------------------------------------
    bool selecting = false;
    bool selActive = false;
    int selStartR = 0, selStartC = 0, selEndR = 0, selEndC = 0;
    double selAnimStart = 0.0;   // Miami gradient ease-in anchor (seconds)

    // --- local diagnostic session (no SSH worker) ---------------------------
    bool diagnostic = false;
    std::string localPending;   // queued local output, drained at cascade rate

    // --- pending host-key decision -----------------------------------------
    bool hostKeyPending = false;
    std::string hostKeyText;

    // --- transcript logging (File → Log Session Output) ---------------------
    FILE* logFile = nullptr;
    int logEscState = 0;        // ESC-sequence stripper state

    // --- output triggers: plain-text line assembly ---------------------------
    std::string trigLine;
    int trigEsc = 0;

    // --- inline images (Sixel / Kitty) -----------------------------------------
    // Anchored to a monotonic row id (Grid::TotalPushed() + cursor row at
    // insertion) so they scroll with the text and into scrollback.
    struct InlineImage
    {
        uint64_t id = 0;
        uint64_t rowId = 0;
        int col = 0;
        int cols = 0, rows = 0;      // cell span
        std::shared_ptr<DecodedImage> img;
    };
    std::vector<InlineImage> images;
    uint64_t nextImageId = 1;
    uint64_t imagesPushedSeen = 0;   // detects grid resets (counter went down)

    // --- asciinema recording / playback --------------------------------------
    FILE* castFile = nullptr;   // .cast v2 being written
    double castStart = 0.0;     // app time of the header
    struct CastEvent { double t; std::string data; };
    std::vector<CastEvent> castEvents;   // playback queue (diagnostic session)
    size_t castNext = 0;
    double castPlayStart = 0.0;

    // --- auto-reconnect ------------------------------------------------------
    // Secrets are retained in locked memory only while this session lives, so
    // reconnects and the SFTP browser work without re-prompting; scrubbed on
    // destruction.
    SecureString savedPassword, savedPassphrase, savedProxyPassword;
    bool logRaw = false;         // Logging: all session output (else printable)
    bool logFlush = false;       // Logging: flush after every write
    int reconnectAttempt = 0;
    double reconnectAt = 0.0;
    bool everConnected = false;
    bool userClosed = false;    // manual disconnect — never auto-reconnect

    // --- split panes ---------------------------------------------------------
    // A tab's secondary pane is owned by its primary session; paneFocus picks
    // which of the two receives input.
    std::unique_ptr<Session> pane;
    bool paneVertical = true;
    int paneFocus = 0;          // 0 = this session, 1 = the pane

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ~Session()
    {
        ssh.Disconnect();
        if (logFile)
        {
            fclose(logFile);
            logFile = nullptr;
        }
        if (castFile)
        {
            fclose(castFile);
            castFile = nullptr;
        }
        savedPassword.Clear();
        savedPassphrase.Clear();
        savedProxyPassword.Clear();
    }

    bool Live() const
    {
        return state == SessionState::Connected ||
               state == SessionState::Connecting ||
               state == SessionState::Authenticating ||
               state == SessionState::VerifyingHost ||
               state == SessionState::Reconnecting;
    }

    // Caption shown on the tab.
    std::string Caption() const
    {
        if (!remoteTitle.empty())
            return remoteTitle;
        if (!label.empty())
            return label;
        return "session";
    }

    void ClearSelection() { selActive = selecting = false; }

    // Normalised selection bounds, ordered top-left to bottom-right.
    void SelectionBounds(int& r0, int& c0, int& r1, int& c1) const
    {
        r0 = selStartR; c0 = selStartC;
        r1 = selEndR;   c1 = selEndC;
        if (r0 > r1 || (r0 == r1 && c0 > c1))
        {
            std::swap(r0, r1);
            std::swap(c0, c1);
        }
    }

    // True when the given view cell falls inside the active selection.
    bool CellSelected(int row, int col) const
    {
        if (!selActive)
            return false;
        int r0, c0, r1, c1;
        SelectionBounds(r0, c0, r1, c1);
        if (row < r0 || row > r1)
            return false;
        if (selRect)
        {
            // Rectangular selection: a column band on every selected row.
            int ca = std::min(selStartC, selEndC), cb = std::max(selStartC, selEndC);
            return col >= ca && col <= cb;
        }
        if (row == r0 && row == r1)
            return col >= c0 && col <= c1;
        if (row == r0)
            return col >= c0;
        if (row == r1)
            return col <= c1;
        return true;
    }
};

using SessionPtr = std::unique_ptr<Session>;

} // namespace amber
