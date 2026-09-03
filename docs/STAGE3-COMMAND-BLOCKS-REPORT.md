# Stage 3 — Semantic Command Blocks on OSC 133

Implementation report. What was investigated, what was built, what is proven
by test, what was proven by running it, and what is not proven.

## 1. Investigation

**1. OSC 133 metadata.** Already present, spread across `Session`: `marks`
(row + 'A'/'C'), `promptRowId` / `promptCol` (the 'B' mark), `cmdRunning`,
`cmdStart`, `pendingCmd` / `pendingCwd` / `pendingStartedAt`, `outputStartRow`
and `folds`. All built in one place, `App::OnShellMark`, with the command text
lifted between the 'B' mark and the cursor at 'C' by `LiftCommandText`.

**2. Row ids across scrolling and trimming.** `Grid::TotalPushed() + screenRow`
is a monotonic id that survives scrolling; inline images and the tide marks
already anchor to it. `AbsCell` indexes a *different* space —
`0..ScrollbackSize()+Rows()-1`, counted from the oldest retained line — so the
two need converting between (`AbsIndexForRow`). A row is gone when
`TotalPushed() - ScrollbackSize()` rises past its id.

**3. Folding.** `Session::Fold` held first/last row, exit code, duration, line
count, collapsed flag and a rendered summary. `BuildFoldMap` produces `rowMap`
(display row → source row + fold index) each frame; `FoldedCell` synthesises
the summary row; `CellFromPx` translates a click through `rowMap` so
selection, links and mouse reporting stay aligned with what is drawn.

**4. Journal capture.** `OnShellMark('D')` → `JournalEntry` → `m_journal.Add`,
written as JSON Lines. Stage 2 added the interrupted path.

**5. OSC 7.** `parser.SetCwdSink` → `Session::cwd`, snapshotted into
`pendingCwd` at the 'C' mark so a command keeps the directory it started in.

**6. Exit flash and taskbar.** `OnShellMark('D')` sets the flash colour and
amount, calls `TriggerShockwave()` on failure and lights `m_tbErrorUntil`;
`UpdateTaskbarProgress` shows indeterminate progress while `cmdRunning` and
scrapes a trailing `NN%` off the cursor row.

**7. Search and selection across folds.** Selection and copy read the
canonical grid (`Grid::GetText`), with `CellFromPx` translating the click —
so copy was already grid truth. **Search was not.** It reads the raw grid, so
it always *found* matches inside collapsed output, but then computed a view
row arithmetically and ignored `rowMap` — so a hidden match was reported as
found and the selection landed on whatever happened to be drawn there. Fixed;
see §5.

## 2. What was built

**`src/sessions/CommandBlocks.{h,cpp}` — the model, as a pure module.** No
Windows, no terminal, no renderer. A `CommandBlock` references rows and holds
no output text: prompt row, input row and column, output first/last, the
command, the directory, start and end times, duration, exit code *and whether
one was reported*, interrupted, running, line count, byte count, collapsed and
bookmarked. A block over ten million lines is the same size as one over two.

`Session::Fold` is gone — `Session::blocks` replaced it, so folding is now one
field of a block rather than a parallel structure. That is what stops the
"two separate histories" the spec warns about: the live blocks and the journal
share ids and a session key.

**A truthfulness fix in the parser.** `OSC 133;D` with no status was being
reported as exit 0 — indistinguishable from a real success, and that
fabricated zero reached the block, the journal, the exit flash and the taskbar.
The mark callback now carries `hasCode`, and a bare `D`, an empty status or a
non-numeric one all mean *the command ended, and the shell did not say how*.
`CommandBlock` has three outcomes as a result — succeeded, failed, unknown —
and they are three colours in the gutter and the summary, because painting
"unknown" as either of the others is a claim AmberSSH cannot make.

**Block assembly** handles the awkward cases: a block opens at 'A' (or at 'B'
or 'C' when those are missing), a second 'A' closes a still-open block without
inventing a status for it, a 'D' with no 'C' creates nothing, and a mark
letter AmberSSH does not know is ignored outright rather than disturbing the
block list.

**Folding upgrade.** The summary now reads
`▸ cat build.log  [120 lines, 4.8 KB, 540ms, ok]  [1] compiling module_1.cpp`
— line count, byte count, duration, the real outcome, optionally the working
directory, and optionally the first non-blank line of the output. The bytes
are what they honestly are: bytes received from the transport between the 'C'
and 'D' marks, escape sequences included.

**Actions**, on the block at the cursor (View → Command Block, the palette) or
on the block under the pointer (right-click): Copy Command, Copy Output, Copy
Command + Output, Fold / Expand, Search in Output, Save as Snippet, Bookmark,
Previous / Next Bookmark, Type the Command, Run the Command Now, Notify When
This One Finishes.

**Rerun is safe by default.** "Type the Command (does not run it)" is the
first of the two items and the one named plainly; running is a separate,
deliberate action. This is the same rule the journal overlay already followed
(Enter types, Ctrl+Enter runs) and it now reads the same way in both places.

**The gutter.** A hairline bar in the left margin spanning each block's rows,
coloured by outcome and brightened for the block under the pointer, which also
shows its metadata (`ok  528ms  120 lines  4.8 KB  /srv/app  04:58`) at the
right-hand end of its topmost visible row. A running command shows a ticking
elapsed clock in the same place. All of it is drawn over the grid from block
metadata — nothing is inserted into the terminal stream, so none of it can
appear in a copy, a search or a session log.

**Completion notifications.** Off / success only / failure only / both, plus a
threshold in seconds, globally and per profile (`-1` follows the global, the
same way `palette` and `themeId` already defer). Below the threshold nothing
is ever reported. In the background it is a tray toast; in the foreground the
status line, because a toast for the window you are looking at is noise.
"Notify when this one finishes" is the per-command override.

**Reduced Motion** keeps the failure colour but drops it to a tint and skips
the shockwave entirely: the information stays, the movement goes.

## 3. Design decisions worth stating

**A block is a reference, and the grid stays the truth.** Copy yields the
terminal's text whether or not something is folded — Copy Output walks the
grid rows the block names. Search reads the grid. Selection is grid
coordinates. Nothing in Stage 3 writes to the parser.

**Search into folded output is explicit, and the fold is opened.** Global
search deliberately includes collapsed blocks — skipping them would make the
result depend on what happened to be folded — and when a match is inside one,
`RevealRow` expands that block, scrolls to it, and the status line says
"expanded it". Silently finding and not showing was the old behaviour and it
was a bug.

**Bookmarks are session-local and die with their rows.** `TrimBlocks` drops a
bookmarked block exactly as it drops any other. A bookmark that outlived the
scrollback would be a durable reference to text that no longer exists, which
is the thing the spec explicitly forbids inventing. The UI says so when you
set one.

**A block with no output cannot be folded.** Collapsing it would put a summary
row where the next prompt belongs. `Fold All` skips them and reports the count
it actually folded.

**Rerun and Save as Snippet sanitise, not silently.** A snippet line is
`name = command`, so a newline or an `=` in either half is replaced, and an
empty name is refused rather than written.

## 4. Verified by automated test

`tests/CommandBlockTests.cpp`, 18 cases. Half of them drive the **real
VtParser with real escape sequences**, because malformed and missing marks
arrive as bytes, not as function calls. Suite total: **3458 assertions in 198
test cases, all passing.** Against the spec's list:

| Spec test | Where |
|---|---|
| well-formed A/B/C/D | "a well-formed A/B/C/D sequence produces four marks" |
| missing marks | "missing marks still yield a usable sequence" — C with no A/B, A with no D, D with no C |
| malformed marks | "malformed marks never become a status" — empty, non-numeric, unknown letters, truncated introducers, ST vs BEL |
| command with no output | the query and trim cases both cover a block whose only row is its prompt |
| multi-line command input | covered by the model (`inputRow`/`inputCol` span); **the lift itself is not unit-tested** — see §7 |
| very large output | "a block over a very large output stays a fixed-size object" |
| interrupted connection | "an unknown outcome counts as not-succeeded, and says so" |
| shell exits mid-command | the bare-`133;D` cases, and the A-with-no-D sequence |
| scrollback trimming | "blocks are dropped when the rows they name are trimmed", including the bookmark rule |
| folding / unfolding | summary content, the no-output rule, clipping |
| selection across boundaries | **not unit-tested** — see §7 |
| search across folded output | **not unit-tested** — see §7 |
| journal linkage | verified live (§5); the fields round-trip through `ProfileStore`/`CommandJournal` |

Plus: the notification policy in full (mode, threshold, edge value, running,
unknown outcomes), completion text that never claims a status it lacks,
duration and byte formatting, a command that is not valid UTF-8, and — the
core rule — **"marks do not disturb the grid they arrive in"**, which feeds
the same text with and without marks and asserts the two grids are identical
cell for cell.

## 5. Verified by running it

A loopback peer playing a scripted shell session with real OSC 133 marks,
driven end to end (`scratchpad/blocks_e2e.ps1`). Captures in the scratchpad.

- **Folding** — `▸ cat build.log  [120 lines, 4.8 KB, 540ms, ok]  [1] compiling
  module_1.cpp -O2 -Wall`. Status line: "Folded 4 command outputs" — four, not
  five, because the `cd` that printed nothing was correctly skipped.
- **The gutter** — bars follow what is *drawn*: a collapsed block occupies one
  row on screen, and the bar shrank to match after the fix in §6.
- **Hover metadata** — `ok  528ms  120 lines  4.8 KB  /srv/app  04:58`, pinned
  to the topmost visible row of the hovered block.
- **A running command** — the elapsed clock ticking at the right of its output
  row, while the shell prompt is untouched.
- **The completion notification** — "cargo build --release finished in 4.0s"
  in the status line, from the long-command policy at a 1s threshold.
- **The journal**, read off disk after the run:

  ```
  {"t":…,"d":1.20802,"x":1, "i":0,"b":2,"k":"bbbbbbbb-…","c":"make check"}
  {"t":…,"d":0,       "x":0, "i":0,"b":3,"k":"bbbbbbbb-…","c":"cd /srv/app"}
  {"t":…,"d":0.400591,"x":-1,"i":1,"b":4,"k":"bbbbbbbb-…","c":"./legacy-script.sh"}
  ```

  The third is the one that matters: the shell sent a bare `133;D`, and it is
  recorded as unknown (`x:-1, i:1`), **not** as success. Every entry carries
  its block id and session key, so the journal and the live blocks are one
  history.

## 6. Bugs found and fixed during the work

**`OSC 133;D` with no status was reported as exit 0.** A shell that says only
"the command ended" was being recorded as having succeeded — in the block, the
journal, the green exit flash and the taskbar. This is the largest correctness
problem Stage 3 found, and it existed before Stage 3.

**Search found matches inside collapsed output and then showed the wrong
row.** It computed a view row from the grid position without consulting the
fold map. It now expands the block that hides the match and says so.

**The gutter bar spanned a folded block's whole unfolded range.** Found by
looking at it: a 120-line block that had collapsed to one row still drew a bar
down the entire screen. Rewritten to walk display rows through the fold map.

**The gutter was O(rows × blocks) per frame** — a `BlockIndexAtRow` per
display row, which at the 4000-block cap is 200,000 comparisons every frame.
Replaced with a single merged walk, since blocks and rows both ascend.

**Stale blocks survived a grid reset.** A reconnect restarts `TotalPushed`, so
every row id a block held pointed at a row that no longer existed. Detected as
the push counter going backwards, and answered by dropping the blocks — they
reference rows, so rows that are gone take them.

**A raw block pointer was held across `TrackPopupMenu`.** The context menu took
`const CommandBlock*` before opening a modal message loop; a block appended
while the menu was up would reallocate the vector and dangle it. Caught before
it shipped — the menu now holds an id.

**`small` is a Windows macro.** `rpcndr.h` defines it as `char`, so a local
named `small` in the tests would not compile. Renamed; noting it because it
will bite again.

## 7. Not proven

| Item | Status |
|---|---|
| Multi-line command input | **not tested.** `LiftCommandText` walks up to ten rows from the 'B' mark and is unchanged from before Stage 3, but no test drives a wrapped or multi-line command through it |
| Selection across command boundaries | **not tested.** Selection is grid coordinates and `CellFromPx` already translated through the fold map before this work; nothing in Stage 3 touches either. That is an argument, not a measurement |
| Search across folded output | the *fix* is live-verified only in the sense that the code path was exercised by hand-reading; there is no automated test, because search lives in `App` and needs a window |
| Copy Output over a partly-trimmed block | the code skips rows that have been trimmed; not exercised with a real trim |
| Performance with thousands of blocks | **not measured.** The per-frame cost is now O(rows + blocks) and the list is capped at 4000, but no session was run to that depth with a frame-time monitor attached |
| Bookmarks over a real trim | unit-tested in `TrimBlocks`; not seen happen live |
| Toast notifications | the background path was not observed — the captures needed the window in front, where the status line is used instead |
| Alternate screen | blocks are suppressed while it is active (`AltActive`), as folding already was; not exercised against a real full-screen app |

**No SSH server on this machine**, as in Stage 2, so all of the above was
driven through a raw-TCP peer emitting the same escape sequences a shell
would. That exercises the parser, the block model and the renderer honestly,
but it is not a real shell with a real prompt.

## 8. Acceptance criteria

| Criterion | Status |
|---|---|
| Blocks derived only from authoritative shell-integration metadata | yes — one builder, `OnShellMark`, fed by the existing OSC 133 sink. No second parser |
| Never corrupt terminal text or copy behaviour | yes — nothing writes to the parser, and a test feeds identical text with and without marks and compares the grids cell for cell |
| Rerun is safe by default | yes — "type it" is the default and the first item; running is a separate action, matching the journal's existing rule |
| Folding / search behaviour is deterministic | yes — search includes folded output and expands what it finds; a block with no output cannot be folded |
| Long-running command notifications without regex triggers | yes — driven by the OSC 133 duration, with a threshold, verified live |
| Performance stable with thousands of blocks | **argued, not measured** — O(rows + blocks) per frame, capped at 4000 |
| Implementation report | this document |
