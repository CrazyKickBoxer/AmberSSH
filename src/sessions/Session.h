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
#include "CommandBlocks.h"
#include "PaneLayout.h"
#include "Guardian.h"
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

    // --- command blocks (sessions/CommandBlocks.h) --------------------------
    // One block per command the shell reported through OSC 133: the rows its
    // prompt, input and output occupy, plus what it was and how it went.
    // Rows are ABSOLUTE ids (TotalPushed + screen row) so they survive
    // scrolling. A block is metadata REFERENCING the grid — never a copy of
    // it — so selection, search and copy keep reading the grid itself.
    uint64_t outputStartRow = 0;   // absolute row of the OSC 133 'C' mark
    std::vector<CommandBlock> blocks;
    uint64_t nextBlockId = 1;
    // A reconnect or a full reset restarts Grid::TotalPushed at zero, and
    // every row id a block holds then points at a row that no longer exists.
    // Detected as the push counter going backwards, and answered by dropping
    // the blocks: they reference rows, so rows that are gone take them.
    uint64_t blocksPushedSeen = 0;
    uint64_t runningBlockId = 0;   // the block between C and D, 0 = none
    uint64_t runningBytes = 0;     // output bytes counted since the C mark
    bool notifyRunning = false;    // "notify me when this one finishes"
    // Display row -> what to draw there, rebuilt each frame. src is a raw
    // grid view row when blockIndex < 0, otherwise the block to summarise.
    struct RowSlot { int src = 0; int blockIndex = -1; };
    std::vector<RowSlot> rowMap;
    bool AnyCollapsed() const
    {
        for (const CommandBlock& b : blocks)
            if (b.collapsed)
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

    // Partial line held back by masked session logging. Redaction has to be
    // line-at-a-time — the detectors reason about a whole line — so a line
    // split across two socket reads waits here for its newline rather than
    // being written in halves that each look innocent.
    std::string logMaskBuf;

    // --- privacy cloak ---------------------------------------------------------
    // One byte per visible cell: 1 = covered at draw time. Rebuilt only when
    // the visible text changes (cloakStamp), because running the detector over
    // every row every frame would cost more than the rest of compose.
    //
    // This is a DISPLAY overlay. The grid keeps the real characters, so
    // selection, search and copy still see the truth and turning the cloak off
    // loses nothing.
    std::vector<uint8_t> cloakMask;
    uint64_t cloakStamp = 0;
    int cloakCount = 0;          // covered runs on screen, for the status bar

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

    // --- session guardian ----------------------------------------------------
    // Secrets are retained in locked memory only while this session lives, so
    // reconnects and the SFTP browser work without re-prompting; scrubbed on
    // destruction.
    SecureString savedPassword, savedPassphrase, savedProxyPassword;
    bool logRaw = false;         // Logging: all session output (else printable)
    bool logFlush = false;       // Logging: flush after every write
    // The reconnect state machine (sessions/Guardian.h). Everything about
    // "did it drop, will it come back, how many times have we tried" lives
    // there; this struct only carries what the UI has to draw.
    Guardian guardian;
    bool everConnected = false;
    bool userClosed = false;    // manual disconnect — never auto-reconnect
    // Sent to the far end once, after the next successful connect. Built by
    // BuildRestorePlan, so it can only ever be a reattach command and a cd.
    std::vector<RestoreStep> restorePlan;
    // What OSC 133 said was running when the link died. Kept so the journal
    // and the annotation can say "interrupted, outcome unknown" instead of
    // inventing an exit code.
    bool hadInterrupted = false;
    InterruptedCommand interrupted;

    // --- view annotations (NOT terminal content) -----------------------------
    // Drawn over the grid at an absolute row id, exactly like the tide marks
    // and the error embers. Nothing here is ever fed to the parser, so a
    // notice cannot appear in a selection, a scrollback search or a session
    // log — the grid stays the truth about what the server sent.
    struct Notice
    {
        uint64_t rowId = 0;
        double t = 0.0;
        int kind = 0;         // 0 info, 1 warning, 2 recovered
        std::string text;
    };
    std::vector<Notice> notices;
    // A reconnect resets the parser and the grid, so every absolute row id
    // from before it points at a row that no longer exists. Detected the way
    // the inline images detect it — the push counter going backwards — and
    // repaired by re-anchoring the surviving notices to the new top.
    uint64_t noticesPushedSeen = 0;

    // --- panes (sessions/PaneLayout.h) ---------------------------------------
    // A tab is its root session plus any panes split off it, laid out by a
    // recursive tree. THIS session is always pane id 0 of its own tab;
    // `extraPanes` holds ids 1..N at index id-1, with a null slot for a pane
    // that has been closed. Ids are never reused, so a broadcast set or a
    // focus reference cannot come to mean a different pane than the one it
    // was pointed at.
    //
    // Only a tab's root session uses these; a pane's own copies stay empty.
    std::vector<std::unique_ptr<Session>> extraPanes;
    PaneLayout layout;
    PaneId focus = 0;            // which pane receives input
    PaneId nextPaneId = 1;
    // Broadcast targets, by pane id. Empty means broadcast is off. Explicit
    // ids rather than "all panes" so a pane opened later is never swept in.
    std::vector<PaneId> broadcast;

    // --- read-only -----------------------------------------------------------
    // A read-only pane still receives output, and can still be selected,
    // copied and searched; it just refuses keyboard input. For watching a
    // production log without being one slip away from typing into it.
    bool readOnly = false;

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
        // A local shell names its tab after itself: cmd and PowerShell set
        // their window title to their own executable path, which is noise.
        if (profile.protocol == Protocol::Local && !label.empty())
            return label;
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
