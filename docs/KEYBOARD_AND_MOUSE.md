# AmberSSH — Keyboard and Mouse

## Application shortcuts

These are handled locally and are never forwarded to the remote shell.

| Shortcut | Action |
|---|---|
| `Ctrl+N`, `Ctrl+Shift+T` | Open the connection manager in a new tab |
| `Ctrl+Shift+W` | Close the current tab (confirms if still connected) |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous tab |
| `Ctrl+1` … `Ctrl+9` | Jump to tab by number |
| `Ctrl+Shift+C` | Copy selection |
| `Ctrl+Shift+V`, `Shift+Insert` | Paste |
| `Ctrl+Shift+D` | Disconnect the current session |
| `Ctrl` + mouse wheel | Font size up / down |
| `Shift+PgUp` / `Shift+PgDn` | Scroll back / forward one page |
| `F2` | Toggle VSync |
| `F3` | Toggle the performance read-out |
| `F11`, `Alt+Enter` | Full screen |

Ordinary `Ctrl` combinations (`Ctrl+C`, `Ctrl+D`, `Ctrl+Z`, `Ctrl+Space` …) are
sent to the remote shell. Local shortcuts deliberately use `Ctrl+Shift` so they
cannot collide with terminal control codes — `Ctrl+C` interrupts the remote
process, `Ctrl+Shift+C` copies.

## Mouse

| Gesture | Action |
|---|---|
| Left drag | Select text |
| Release left button | **Copies the selection automatically** and reports "Copied N characters" in the status line |
| Click without dragging | Clear the selection |
| Right click | Paste |
| `Alt` + left drag | Particle force field — pushes particles aside; text and selection are unaffected |
| Wheel | Scroll the scrollback |
| Drag past the top/bottom edge | Auto-scrolls while selecting |
| Click a tab | Switch session |

Selection never modifies the terminal model: it is a view over real text cells,
so what lands on the clipboard is exactly what the server sent.

## Selection appearance

Selected cells switch from the amber ramp to the **Miami Sunset** gradient
(`#6C3BFF → #D92BFF → #FF2D95 → #FF6A3D → #FFD166`), computed per particle in
the simulation shader. The gradient drifts slowly across the selected region
and eases in and out so toggling never pops. Unselected text stays amber.

## Typing behaviour

- Typing returns the view to the live bottom if you had scrolled back.
- Pasted text is sent as UTF-8; bracketed-paste mode is honoured when the
  remote application enables it.
- Keyboard input goes through `ToUnicodeEx`, so dead keys and non-US layouts
  produce the right characters. Surrogate pairs are recombined before encoding.
