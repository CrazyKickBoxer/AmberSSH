#include "grid.h"
#include "charwidth.h"
#include "Graphemes.h"

#include <algorithm>

void Grid::Init(int cols, int rows)
{
    m_cols = std::max(2, cols);
    m_rows = std::max(2, rows);
    m_main.assign(static_cast<size_t>(m_cols) * m_rows, Cell{});
    m_alt.assign(static_cast<size_t>(m_cols) * m_rows, Cell{});
    m_curX = m_curY = 0;
    m_scrollTop = 0;
    m_scrollBot = m_rows - 1;
    m_wrapPending = false;
    m_viewOffset = 0;
    m_tabs.assign(m_cols, false);
    for (int i = 0; i < m_cols; i += 8)
        m_tabs[i] = true;
}

void Grid::Resize(int cols, int rows)
{
    if (m_main.empty())
    {
        Init(cols, rows);
        return;
    }
    cols = std::max(2, cols);
    rows = std::max(2, rows);
    if (cols == m_cols && rows == m_rows)
        return;

    auto resizeScreen = [&](std::vector<Cell>& scr)
    {
        std::vector<Cell> next(static_cast<size_t>(cols) * rows, Cell{});
        int copyRows = std::min(rows, m_rows);
        int copyCols = std::min(cols, m_cols);
        for (int y = 0; y < copyRows; ++y)
            for (int x = 0; x < copyCols; ++x)
                next[static_cast<size_t>(y) * cols + x] =
                    scr[static_cast<size_t>(y) * m_cols + x];
        scr.swap(next);
    };
    resizeScreen(m_main);
    resizeScreen(m_alt);

    m_cols = cols;
    m_rows = rows;
    m_curX = std::min(m_curX, m_cols - 1);
    m_curY = std::min(m_curY, m_rows - 1);
    m_scrollTop = 0;
    m_scrollBot = m_rows - 1;
    m_wrapPending = false;
    m_viewOffset = 0;
    m_tabs.assign(m_cols, false);
    for (int i = 0; i < m_cols; i += 8)
        m_tabs[i] = true;
}

void Grid::BlankLine(std::vector<Cell>& line, const Cell& brush) const
{
    Cell b;
    b.bg = brush.bg;
    line.assign(m_cols, b);
}

void Grid::PushScrollback(int row)
{
    if (m_altActive)
        return;
    std::vector<Cell> line(m_cols);
    memcpy(line.data(), &m_main[static_cast<size_t>(row) * m_cols],
           sizeof(Cell) * m_cols);
    m_scrollback.push_back(std::move(line));
    ++m_totalPushed;
    while (m_scrollback.size() > m_scrollbackMax)
        m_scrollback.pop_front();
}

void Grid::ScrollUpRegion(int top, int bot, int n, const Cell& brush, bool toHistory)
{
    n = std::clamp(n, 0, bot - top + 1);
    if (n == 0)
        return;
    auto& scr = Screen();
    for (int i = 0; i < n && toHistory; ++i)
        PushScrollback(top + i);
    for (int y = top; y + n <= bot; ++y)
        memcpy(&scr[static_cast<size_t>(y) * m_cols],
               &scr[static_cast<size_t>(y + n) * m_cols],
               sizeof(Cell) * m_cols);
    Cell b;
    b.bg = brush.bg;
    for (int y = bot - n + 1; y <= bot; ++y)
        std::fill_n(&scr[static_cast<size_t>(y) * m_cols], m_cols, b);
}

void Grid::ScrollDownRegion(int top, int bot, int n, const Cell& brush)
{
    n = std::clamp(n, 0, bot - top + 1);
    if (n == 0)
        return;
    auto& scr = Screen();
    for (int y = bot; y - n >= top; --y)
        memcpy(&scr[static_cast<size_t>(y) * m_cols],
               &scr[static_cast<size_t>(y - n) * m_cols],
               sizeof(Cell) * m_cols);
    Cell b;
    b.bg = brush.bg;
    for (int y = top; y < top + n; ++y)
        std::fill_n(&scr[static_cast<size_t>(y) * m_cols], m_cols, b);
}

void Grid::HealOrphan(int x, int y)
{
    if (x < 0 || x >= m_cols || y < 0 || y >= m_rows)
        return;
    Cell& c = At(x, y);
    if ((c.flags & CellWideLead) &&
        (x + 1 >= m_cols || !(At(x + 1, y).flags & CellWideTail)))
    {
        c.cp = U' ';
        c.flags = 0;
    }
    else if ((c.flags & CellWideTail) &&
             (x <= 0 || !(At(x - 1, y).flags & CellWideLead)))
    {
        c.cp = U' ';
        c.flags = 0;
    }
}

void Grid::ClearWideAt(int x, int y)
{
    Cell& c = At(x, y);
    if (c.flags & CellWideLead)
    {
        if (x + 1 < m_cols && (At(x + 1, y).flags & CellWideTail))
        {
            Cell& t = At(x + 1, y);
            t.cp = U' ';
            t.flags = 0;
        }
    }
    else if (c.flags & CellWideTail)
    {
        if (x > 0 && (At(x - 1, y).flags & CellWideLead))
        {
            Cell& l = At(x - 1, y);
            l.cp = U' ';
            l.flags = 0;
        }
    }
}

// Adds a combining mark to the cell at (x, y), re-interning its cluster.
// Refused interns (the table is full, or the cluster hit its length cap)
// leave the cell exactly as it was: the base character stays correct and only
// the mark is lost, which is the same degradation as before this existed.
void Grid::AttachMark(int x, int y, char32_t mark)
{
    Cell& c = At(x, y);
    // A mark can only attach to something that was drawn. A wide tail carries
    // no ink of its own, so the mark belongs to its lead.
    if (c.flags & CellWideTail)
    {
        if (x > 0)
            AttachMark(x - 1, y, mark);
        return;
    }
    std::u32string text;
    if (amber::IsClusterAlias(c.cp))
        text = amber::Clusters().Text(c.cp);
    else
        text.push_back(c.cp == 0 ? U' ' : c.cp);
    if (text.empty())
        return;
    text.push_back(mark);
    if (const char32_t alias = amber::Clusters().Intern(text))
        c.cp = alias;
}

void Grid::PutChar(char32_t cp, const Cell& brush, bool insertMode)
{
    int w = TermCharWidth(cp);

    // Zero-width: no cell of its own, matching the server's wcwidth. The
    // variation selectors retro-flag the previous glyph's presentation; a
    // combining mark ATTACHES to it, becoming part of that cell's cluster.
    //
    // Attaching is what makes "e" + U+0301 render and copy as "é". Before
    // this the mark was dropped on the floor and the text was silently wrong.
    if (w == 0)
    {
        const bool haveLast = m_lastX >= 0 && m_lastX < m_cols &&
                              m_lastY >= 0 && m_lastY < m_rows;
        if (!haveLast)
            return;
        if (cp == 0xFE0F)
        {
            At(m_lastX, m_lastY).flags |= CellEmojiVS;
            return;
        }
        if (cp == 0xFE0E)
        {
            At(m_lastX, m_lastY).flags |= CellTextVS;
            return;
        }
        if (amber::IsCombining(cp))
            AttachMark(m_lastX, m_lastY, cp);
        return;
    }

    // A regional indicator following another one completes a flag. Both
    // halves are width 1, so the pair occupies the same two columns joined or
    // not — which is why this one CAN be merged without lying to the server
    // about the cursor. The cluster goes in the first cell as a wide lead.
    if (amber::IsRegionalIndicator(cp) && m_lastX >= 0 && m_lastX < m_cols &&
        m_lastY >= 0 && m_lastY < m_rows && m_curX == m_lastX + 1 &&
        m_curY == m_lastY && !m_wrapPending)
    {
        Cell& lead = At(m_lastX, m_lastY);
        const char32_t base = amber::ClusterBase(lead.cp);
        if (amber::IsRegionalIndicator(base) && !(lead.flags & CellWideLead))
        {
            std::u32string text;
            if (amber::IsClusterAlias(lead.cp))
                text = amber::Clusters().Text(lead.cp);
            else
                text.push_back(lead.cp);
            text.push_back(cp);
            if (const char32_t alias = amber::Clusters().Intern(text))
            {
                lead.cp = alias;
                lead.flags |= CellWideLead;
                Cell tail = brush;
                tail.cp = U' ';
                tail.flags = CellWideTail;
                At(m_curX, m_curY) = tail;
                m_lastX = m_curX - 1;   // the flag stays the "last" cell
                if (m_curX + 1 >= m_cols)
                {
                    m_curX = m_cols - 1;
                    m_wrapPending = true;
                }
                else
                    ++m_curX;
                return;
            }
        }
    }

    if (m_wrapPending && m_autowrap)
    {
        m_wrapPending = false;
        m_curX = 0;
        LineFeed();
    }
    // A wide char that doesn't fit in the last column wraps early (xterm
    // behavior); the orphaned last cell is blanked with the brush colors.
    if (w == 2 && m_curX == m_cols - 1)
    {
        Cell blank = brush;
        blank.cp = U' ';
        blank.flags = 0;
        ClearWideAt(m_curX, m_curY);
        At(m_curX, m_curY) = blank;
        if (m_autowrap)
        {
            m_curX = 0;
            LineFeed();
        }
        else
            m_curX = std::max(0, m_cols - 2);
    }
    if (insertMode)
        InsertChars(w, brush);

    ClearWideAt(m_curX, m_curY);
    Cell c = brush;
    c.cp = cp;
    c.flags = (w == 2) ? CellWideLead : 0;
    At(m_curX, m_curY) = c;
    m_lastX = m_curX;
    m_lastY = m_curY;

    if (w == 2)
    {
        ClearWideAt(m_curX + 1, m_curY);
        Cell t = brush;
        t.cp = U' ';
        t.flags = CellWideTail;
        At(m_curX + 1, m_curY) = t;
    }

    if (m_curX + w >= m_cols)
    {
        m_curX = m_cols - 1;
        m_wrapPending = true;
    }
    else
        m_curX += w;
}

void Grid::LineFeed()
{
    m_wrapPending = false;
    if (m_curY == m_scrollBot)
        ScrollUpRegion(m_scrollTop, m_scrollBot, 1, m_blank,
                       m_scrollTop == 0 && m_scrollBot == m_rows - 1);
    else if (m_curY < m_rows - 1)
        ++m_curY;
}

void Grid::ReverseIndex()
{
    m_wrapPending = false;
    if (m_curY == m_scrollTop)
        ScrollDownRegion(m_scrollTop, m_scrollBot, 1, m_blank);
    else if (m_curY > 0)
        --m_curY;
}

void Grid::Backspace()
{
    m_wrapPending = false;
    if (m_curX > 0)
        --m_curX;
}

void Grid::Tab()
{
    m_wrapPending = false;
    for (int x = m_curX + 1; x < m_cols; ++x)
        if (m_tabs[x])
        {
            m_curX = x;
            return;
        }
    m_curX = m_cols - 1;
}

void Grid::SetTabStop() { m_tabs[m_curX] = true; }

void Grid::ClearTabStop(bool all)
{
    if (all)
        std::fill(m_tabs.begin(), m_tabs.end(), false);
    else
        m_tabs[m_curX] = false;
}

void Grid::MoveCursor(int dx, int dy)
{
    m_wrapPending = false;
    m_curX = std::clamp(m_curX + dx, 0, m_cols - 1);
    m_curY = std::clamp(m_curY + dy, 0, m_rows - 1);
}

void Grid::SetCursor(int x, int y)
{
    m_wrapPending = false;
    m_curX = std::clamp(x, 0, m_cols - 1);
    m_curY = std::clamp(y, 0, m_rows - 1);
}

void Grid::SetCol(int x) { m_wrapPending = false; m_curX = std::clamp(x, 0, m_cols - 1); }
void Grid::SetRow(int y) { m_wrapPending = false; m_curY = std::clamp(y, 0, m_rows - 1); }

void Grid::EraseDisplay(int mode, const Cell& brush)
{
    m_wrapPending = false;
    Cell b;
    b.bg = brush.bg;
    auto& scr = Screen();
    switch (mode)
    {
    case 0: // cursor to end
        EraseLine(0, brush);
        for (int y = m_curY + 1; y < m_rows; ++y)
            std::fill_n(&scr[static_cast<size_t>(y) * m_cols], m_cols, b);
        break;
    case 1: // start to cursor
        EraseLine(1, brush);
        for (int y = 0; y < m_curY; ++y)
            std::fill_n(&scr[static_cast<size_t>(y) * m_cols], m_cols, b);
        break;
    case 3: // all + scrollback
        m_scrollback.clear();
        m_viewOffset = 0;
        [[fallthrough]];
    case 2:
        std::fill(scr.begin(), scr.end(), b);
        break;
    default:
        break;
    }
}

void Grid::EraseLine(int mode, const Cell& brush)
{
    m_wrapPending = false;
    Cell b;
    b.bg = brush.bg;
    int x0 = 0, x1 = m_cols;
    if (mode == 0) x0 = m_curX;
    else if (mode == 1) x1 = m_curX + 1;
    auto& scr = Screen();
    std::fill(&scr[static_cast<size_t>(m_curY) * m_cols + x0],
              &scr[static_cast<size_t>(m_curY) * m_cols + x1], b);
    HealOrphan(x0 - 1, m_curY);
    HealOrphan(x1, m_curY);
}

void Grid::InsertLines(int n, const Cell& brush)
{
    if (m_curY < m_scrollTop || m_curY > m_scrollBot)
        return;
    ScrollDownRegion(m_curY, m_scrollBot, std::max(1, n), brush);
}

void Grid::DeleteLines(int n, const Cell& brush)
{
    if (m_curY < m_scrollTop || m_curY > m_scrollBot)
        return;
    ScrollUpRegion(m_curY, m_scrollBot, std::max(1, n), brush, false);
}

void Grid::InsertChars(int n, const Cell& brush)
{
    n = std::clamp(n < 1 ? 1 : n, 1, m_cols - m_curX);
    auto& scr = Screen();
    Cell* row = &scr[static_cast<size_t>(m_curY) * m_cols];
    for (int x = m_cols - 1; x >= m_curX + n; --x)
        row[x] = row[x - n];
    Cell b;
    b.bg = brush.bg;
    for (int x = m_curX; x < m_curX + n; ++x)
        row[x] = b;
    HealOrphan(m_curX - 1, m_curY);
    HealOrphan(m_curX + n, m_curY);
    HealOrphan(m_cols - 1, m_curY);
}

void Grid::DeleteChars(int n, const Cell& brush)
{
    n = std::clamp(n < 1 ? 1 : n, 1, m_cols - m_curX);
    auto& scr = Screen();
    Cell* row = &scr[static_cast<size_t>(m_curY) * m_cols];
    for (int x = m_curX; x + n < m_cols; ++x)
        row[x] = row[x + n];
    Cell b;
    b.bg = brush.bg;
    for (int x = m_cols - n; x < m_cols; ++x)
        row[x] = b;
    HealOrphan(m_curX - 1, m_curY);
    HealOrphan(m_curX, m_curY);
}

void Grid::EraseChars(int n, const Cell& brush)
{
    n = std::clamp(n < 1 ? 1 : n, 1, m_cols - m_curX);
    Cell b;
    b.bg = brush.bg;
    auto& scr = Screen();
    std::fill_n(&scr[static_cast<size_t>(m_curY) * m_cols + m_curX], n, b);
    HealOrphan(m_curX - 1, m_curY);
    HealOrphan(m_curX + n, m_curY);
}

void Grid::ScrollUp(int n, const Cell& brush)
{
    ScrollUpRegion(m_scrollTop, m_scrollBot, std::max(1, n), brush, false);
}

void Grid::ScrollDown(int n, const Cell& brush)
{
    ScrollDownRegion(m_scrollTop, m_scrollBot, std::max(1, n), brush);
}

void Grid::SetScrollRegion(int top, int bot)
{
    if (top < 0) top = 0;
    if (bot <= 0 || bot >= m_rows) bot = m_rows - 1;
    if (top >= bot)
    {
        m_scrollTop = 0;
        m_scrollBot = m_rows - 1;
    }
    else
    {
        m_scrollTop = top;
        m_scrollBot = bot;
    }
    SetCursor(0, m_scrollTop);
}

void Grid::EnterAlt()
{
    if (m_altActive)
        return;
    m_savedMain.x = m_curX;
    m_savedMain.y = m_curY;
    m_savedMain.valid = true;
    m_altActive = true;
    std::fill(m_alt.begin(), m_alt.end(), Cell{});
    m_curX = m_curY = 0;
    m_scrollTop = 0;
    m_scrollBot = m_rows - 1;
    m_wrapPending = false;
    m_viewOffset = 0;
}

void Grid::ExitAlt()
{
    if (!m_altActive)
        return;
    m_altActive = false;
    m_scrollTop = 0;
    m_scrollBot = m_rows - 1;
    m_wrapPending = false;
    if (m_savedMain.valid)
    {
        m_curX = std::min(m_savedMain.x, m_cols - 1);
        m_curY = std::min(m_savedMain.y, m_rows - 1);
    }
}

void Grid::SaveCursor()
{
    Saved& s = m_altActive ? m_savedAlt : m_savedMain;
    s.x = m_curX;
    s.y = m_curY;
    s.valid = true;
}

void Grid::RestoreCursor()
{
    Saved& s = m_altActive ? m_savedAlt : m_savedMain;
    if (s.valid)
        SetCursor(s.x, s.y);
}

void Grid::ResetAll()
{
    m_altActive = false;
    std::fill(m_main.begin(), m_main.end(), Cell{});
    std::fill(m_alt.begin(), m_alt.end(), Cell{});
    m_curX = m_curY = 0;
    m_scrollTop = 0;
    m_scrollBot = m_rows - 1;
    m_cursorVisible = true;
    m_wrapPending = false;
    m_autowrap = true;
    m_viewOffset = 0;
    m_totalPushed = 0;
    m_tabs.assign(m_cols, false);
    for (int i = 0; i < m_cols; i += 8)
        m_tabs[i] = true;
}

void Grid::ScrollView(int deltaLines)
{
    if (m_altActive)
        return;
    m_viewOffset = std::clamp(m_viewOffset + deltaLines, 0,
                              static_cast<int>(m_scrollback.size()));
}

const Cell& Grid::AbsCell(int absRow, int col) const
{
    static const Cell kEmpty;
    if (col < 0 || col >= m_cols || absRow < 0)
        return kEmpty;
    int sb = static_cast<int>(m_scrollback.size());
    if (absRow < sb)
    {
        const auto& line = m_scrollback[static_cast<size_t>(absRow)];
        return (col < static_cast<int>(line.size())) ? line[col] : kEmpty;
    }
    int r = absRow - sb;
    if (r >= m_rows)
        return kEmpty;
    return Screen()[static_cast<size_t>(r) * m_cols + col];
}

const Cell& Grid::ViewCell(int row, int col) const
{
    static const Cell kEmpty;
    if (col < 0 || col >= m_cols || row < 0 || row >= m_rows)
        return kEmpty;

    if (m_viewOffset > 0 && !m_altActive)
    {
        int sb = static_cast<int>(m_scrollback.size());
        int v = sb - m_viewOffset + row;
        if (v < sb)
        {
            if (v < 0)
                return kEmpty;
            const auto& line = m_scrollback[v];
            return (col < static_cast<int>(line.size())) ? line[col] : kEmpty;
        }
        row = v - sb;
        if (row >= m_rows)
            return kEmpty;
    }
    return Screen()[static_cast<size_t>(row) * m_cols + col];
}

void Grid::CursorViewPos(int& row, int& col, bool& visible) const
{
    col = m_curX;
    row = m_curY;
    visible = m_cursorVisible && (m_viewOffset == 0 || m_altActive);
}

std::string Grid::GetText(int r0, int c0, int r1, int c1) const
{
    if (r1 < r0 || (r1 == r0 && c1 < c0))
    {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    std::string out;
    for (int r = r0; r <= r1; ++r)
    {
        int x0 = (r == r0) ? c0 : 0;
        int x1 = (r == r1) ? c1 : m_cols - 1;
        std::string line;
        for (int c = x0; c <= x1; ++c)
        {
            const Cell& cell = ViewCell(r, c);
            if (cell.flags & CellWideTail)
                continue;   // second half of a wide char — no text of its own
            amber::AppendClusterUtf8(line, cell.cp ? cell.cp : U' ');
            // Presentation selectors are stored as flags, not cells — re-emit
            // them so a copied ❤️ pastes as the colour form elsewhere.
            if (cell.flags & CellEmojiVS)
                AppendUtf8(line, 0xFE0F);
            else if (cell.flags & CellTextVS)
                AppendUtf8(line, 0xFE0E);
        }
        // trim trailing spaces
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out += line;
        if (r != r1)
            out += "\r\n";
    }
    return out;
}
