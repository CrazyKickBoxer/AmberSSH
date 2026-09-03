// grid.h — terminal cell grid: main + alternate screens, 5000-line scrollback,
// scroll regions, cursor, and view scrolling for Shift+PgUp/PgDn.
#pragma once

#include "../common.h"
#include "palette.h"
#include <deque>

enum CellAttr : uint16_t
{
    AttrBold        = 1 << 0,
    AttrDim         = 1 << 1,    // SGR 2 (faint)
    AttrItalic      = 1 << 2,
    AttrUnderline   = 1 << 3,
    AttrDblUnder    = 1 << 4,    // SGR 21
    AttrBlink       = 1 << 5,    // SGR 5 (and 6 — rendered identically)
    AttrInverse     = 1 << 6,
    AttrConceal     = 1 << 7,    // SGR 8 — hidden but still copyable
    AttrStrike      = 1 << 8,    // SGR 9
};

// Cell::flags bits — double-width bookkeeping and emoji presentation.
enum CellFlag : uint16_t
{
    CellWideLead = 1 << 0,   // first cell of a double-width char
    CellWideTail = 1 << 1,   // second cell (no ink of its own)
    CellEmojiVS  = 1 << 2,   // U+FE0F followed: force color-emoji rendering
    CellTextVS   = 1 << 3,   // U+FE0E followed: force mono rendering
};

// One terminal cell: codepoint plus full-fidelity color state. fg/bg/ul are
// packed CellColors (default / palette index / 24-bit RGB — see palette.h);
// resolution to display RGB happens at render time against the active palette.
struct Cell
{
    char32_t cp = U' ';
    amber::CellColor fg = amber::kColorDefault;
    amber::CellColor bg = amber::kColorDefault;
    amber::CellColor ul = amber::kColorDefault;   // SGR 58/59 underline color
    uint16_t attr = 0;
    uint16_t flags = 0;      // CellFlag bits
    uint16_t link = 0;       // OSC 8 hyperlink id (VtParser::LinkUri), 0 = none
};

class Grid
{
public:
    void Init(int cols, int rows);
    void Resize(int cols, int rows);

    int Cols() const { return m_cols; }
    int Rows() const { return m_rows; }

    // --- parser-facing operations ---------------------------------------
    void SetAutowrap(bool on) { m_autowrap = on; }
    void PutChar(char32_t cp, const Cell& brush, bool insertMode);
    void LineFeed();
    void CarriageReturn() { m_curX = 0; m_wrapPending = false; }
    void Backspace();
    void Tab();
    void SetTabStop();
    void ClearTabStop(bool all);
    void ReverseIndex();
    void NextLine() { CarriageReturn(); LineFeed(); }

    void MoveCursor(int dx, int dy);
    void SetCursor(int x, int y);      // 0-based, clamped
    void SetCol(int x);
    void SetRow(int y);
    int CurX() const { return m_curX; }
    int CurY() const { return m_curY; }

    void EraseDisplay(int mode, const Cell& brush);   // 0 below, 1 above, 2 all, 3 +scrollback
    void EraseLine(int mode, const Cell& brush);
    void InsertLines(int n, const Cell& brush);
    void DeleteLines(int n, const Cell& brush);
    void InsertChars(int n, const Cell& brush);
    void DeleteChars(int n, const Cell& brush);
    void EraseChars(int n, const Cell& brush);
    void ScrollUp(int n, const Cell& brush);
    void ScrollDown(int n, const Cell& brush);

    void SetScrollRegion(int top, int bot);           // 0-based inclusive
    void SetCursorVisible(bool v) { m_cursorVisible = v; }
    bool CursorVisible() const { return m_cursorVisible; }

    // BEL arrived; the app clears this after ringing the visual bell.
    bool bellPending = false;

    void EnterAlt();
    void ExitAlt();
    bool AltActive() const { return m_altActive; }

    void SaveCursor();
    void RestoreCursor();
    void ResetAll();

    // --- view / scrollback ----------------------------------------------
    void ScrollView(int deltaLines);   // + = towards history
    void SnapView() { m_viewOffset = 0; }
    // Absolute view position: how many lines back from the live screen the
    // top of the view sits. Used to land on a command mark exactly.
    void SetView(int linesBack)
    {
        int maxBack = static_cast<int>(m_scrollback.size());
        m_viewOffset = linesBack < 0 ? 0 : (linesBack > maxBack ? maxBack : linesBack);
    }
    // Lines of scrollback to retain (0 = none). Trims immediately.
    void SetScrollbackMax(size_t lines)
    {
        m_scrollbackMax = lines;
        while (m_scrollback.size() > m_scrollbackMax)
            m_scrollback.pop_front();
        if (m_viewOffset > static_cast<int>(m_scrollback.size()))
            m_viewOffset = static_cast<int>(m_scrollback.size());
    }
    size_t ScrollbackMax() const { return m_scrollbackMax; }
    int ViewOffset() const { return m_viewOffset; }
    int ScrollbackSize() const { return static_cast<int>(m_scrollback.size()); }
    // Monotonic count of lines ever pushed into scrollback: live row r has
    // the stable id TotalPushed()+r, which survives scrolling (inline
    // images anchor to it). Resets to 0 with ResetAll.
    uint64_t TotalPushed() const { return m_totalPushed; }

    const Cell& ViewCell(int row, int col) const;
    // Absolute-row access for search: 0 = oldest history line,
    // ScrollbackSize()..+Rows()-1 = the live screen, view offset ignored.
    const Cell& AbsCell(int absRow, int col) const;
    // Cursor position in view coordinates; visible=false when scrolled back.
    void CursorViewPos(int& row, int& col, bool& visible) const;

    std::string GetText(int r0, int c0, int r1, int c1) const;  // view coords

private:
    Cell& At(int x, int y) { return Screen()[static_cast<size_t>(y) * m_cols + x]; }
    // Overwriting one half of a double-width pair blanks the other half so a
    // stray lead never keeps painting a full emoji over unrelated cells.
    void ClearWideAt(int x, int y);
    // After a range erase/shift: a lead missing its tail (or tail missing its
    // lead) at the boundary is blanked instead of ghosting half an emoji.
    void HealOrphan(int x, int y);
    std::vector<Cell>& Screen() { return m_altActive ? m_alt : m_main; }
    const std::vector<Cell>& Screen() const { return m_altActive ? m_alt : m_main; }
    void PushScrollback(int row);
    void BlankLine(std::vector<Cell>& line, const Cell& brush) const;
    void ScrollUpRegion(int top, int bot, int n, const Cell& brush, bool toHistory);
    void ScrollDownRegion(int top, int bot, int n, const Cell& brush);

    int m_cols = 80, m_rows = 24;
    std::vector<Cell> m_main, m_alt;
    bool m_altActive = false;

    std::deque<std::vector<Cell>> m_scrollback;
    size_t m_scrollbackMax = 5000;   // SetScrollbackMax (Window > lines of scrollback)
    int m_viewOffset = 0;
    uint64_t m_totalPushed = 0;

    int m_curX = 0, m_curY = 0;
    // Cell most recently written by PutChar — a following variation selector
    // (VS15/VS16) or combining mark applies to it retroactively.
    int m_lastX = -1, m_lastY = -1;
    bool m_cursorVisible = true;
    bool m_wrapPending = false;
    bool m_autowrap = true;
    int m_scrollTop = 0, m_scrollBot = 23;

    struct Saved { int x = 0, y = 0; bool valid = false; };
    Saved m_savedMain, m_savedAlt;

    std::vector<bool> m_tabs;
    Cell m_blank;
};
