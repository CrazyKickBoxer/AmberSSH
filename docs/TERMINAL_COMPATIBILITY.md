# AmberSSH — Terminal Compatibility

Announced as `TERM=xterm-256color`.

## Supported

**Control characters** — BEL, BS, HT, LF, VT, FF, CR, and UTF-8 decoding
including sequences split across socket reads.

**ESC** — `ESC 7` / `ESC 8` (save/restore cursor), `ESC D` (index),
`ESC M` (reverse index), `ESC E` (next line), `ESC c` (reset),
`ESC ( 0` / `ESC ( B` (DEC special graphics / ASCII charset selection).

**CSI** — CUU/CUD/CUF/CUB, CNL/CPL, CHA, VPA, CUP/HVP, ED (0/1/2/3),
EL (0/1/2), ICH, DCH, ECH, IL, DL, SU, SD, DECSTBM (scroll region),
SCP/RCP, DSR (cursor position report), DA (primary), DECSCUSR (cursor style),
SGR, and DEC private mode set/reset.

**SGR** — reset, bold, dim, italic, underline, blink, reverse, hidden,
strikeout, the standard 16 colours, `38;5;n` / `48;5;n` indexed colour, and
`38;2;r;g;b` / `48;2;r;g;b` truecolor. Colours are mapped onto the amber
intensity ramp in amber-monochrome mode and preserved in ANSI-phosphor mode.

**DEC private modes** — `?1` application cursor keys, `?7` autowrap,
`?25` cursor visibility, `?1049` / `?47` alternate screen, `?2004` bracketed
paste, `?1000`–`?1003` mouse tracking (accepted).

**OSC** — `0` and `2` set the window title, terminated by BEL or ST.
Unrecognised OSC commands are consumed and ignored. **No escape sequence can
run an operating-system command.**

## Partially supported

- Mouse tracking modes are parsed and accepted, but pointer events are not yet
  forwarded to the remote application.
- OSC 8 hyperlinks are consumed but not yet made clickable.
- Focus reporting is not implemented.

## Not supported

- Sixel and ReGIS graphics.
- Double-width / double-height line attributes (DECDWL / DECDHL).
- Rectangular-area operations (DECCRA / DECFRA).

## Robustness

The parser is written against hostile input:

- Parameter arrays are bounded at 16 entries; values clamp at 65535.
- OSC payloads are bounded at 4096 bytes — a 200 KB title attempt is covered by
  a regression test.
- Malformed and truncated sequences recover without corrupting terminal state.
- Invalid UTF-8 is replaced rather than throwing.
- There is no unchecked indexing on any parser path.

A truncated `ESC [` consumes the next byte in the final range. That is correct
VT behaviour and matches xterm; a test asserts it explicitly.

## Verified by test, not by claim

Every item under "Supported" has unit coverage in `tests/AnsiParserTests.cpp`
and `tests/TerminalBufferTests.cpp` (102 test cases total). Behaviour against
real full-screen applications — `vim`, `htop`, `top`, `less` — has **not** been
exercised, because no SSH server was available in the build environment.
