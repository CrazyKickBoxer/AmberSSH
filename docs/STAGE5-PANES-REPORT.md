# Stage 5 — Advanced Panes, Broadcast Sets and Workspaces

Implementation report. What the investigation found, what was built, what is
proven by test, what was proven by running it, and what was deliberately not
built and why.

## 1. Investigation

| Item | State before Stage 5 |
|---|---|
| Tab model | `std::vector<SessionPtr> m_sessions`, one entry per tab, `m_active` index |
| Pane model | **one level, two panes.** `Session::pane` was a single optional second `Session`, `paneVertical` picked side-by-side or stacked, the ratio was hardcoded 50/50, and `SplitPane` refused when a tab was already split |
| Session ownership | a tab owns its pane; both destruct with the tab |
| Window ownership | **one.** `main.cpp` creates one HWND and one `App` on the stack |
| Renderer surface | **one of everything** — one `Device` (one swap chain), `ParticleRenderer`, `Bloom`, `Composite`, `PrimRenderer`, `GlyphSampler`, all members of that single `App` |
| Input focus | `paneFocus` (0 or 1); `Foc()` returned one of the two |
| Resize routing | `UpdateGridDims` halved the grid for a split tab and called `RequestResize` per session |
| Workspace serialisation | one profile id per tab plus an optional `splitProfileId` — and **splits were never actually restored**: the loader printed "split panes are reopened as separate tabs" |
| Broadcast | one global `bool m_broadcast`, mirroring keystrokes into "the other pane" |

Two things stood out. The pane model could not express nesting at all, and
every renderer path — compose, images, spotlight, notices, block gutter,
afterimages, `CellFromPx`, selection — hardcoded "the session and its pane"
with fixed offsets. And the workspace loader had a feature that did nothing.

## 2. What was built

**`src/sessions/PaneLayout.{h,cpp}` — a real binary layout tree.** A leaf
holds a pane id; a split holds two children, an orientation and a ratio.
Either child of a split can itself be a split, so nesting is arbitrary.

Pure: no Windows, no sessions, no renderer. It stores **pane ids, not Session
pointers**, so every operation the spec lists — split, close, resize, swap,
move, rotate, zoom, restore, and neighbour navigation — is a transformation
that can be tested exhaustively without a terminal.

Splitting works **in cells**, not pixels: each divider takes one cell, the
first child gets a whole number of them and the second gets the remainder, so
a pane's column and row count is exact and the two always add up.

**Zoom is a view state, not a structural change.** `Rects()` gives the zoomed
pane the whole area and everything else an empty rect; the tree is untouched.
Restoring therefore returns the exact previous layout *by construction* rather
than by remembering it — which is why the test can compare every rect before
and after and demand equality.

**A tab is its root session plus `extraPanes`.** The root session IS pane 0;
ids 1..N live in a vector at index id-1, with a null slot for a closed pane so
**ids are never reused**. That matters for broadcast: a target set cannot come
to mean a different pane than the one it was pointed at.

**Every renderer path now iterates the tree.** Compose, inline images, the
afterimage pass, sizing, hit-testing and the divider drawing all walk
`layout.Rects()`. Dividers are drawn per split from `DividerAt`, and the
focused pane's edges are brighter, which is how the eye finds where input is
going in a four-way layout.

**Read-only panes.** One check, in `SendToShell`, so every route into the
shell — typing, paste, snippets, the journal, block rerun, broadcast — is
covered by one test rather than seven. Output, selection, copy and search keep
working; the pane carries a `READ-ONLY` badge, because a lock the user cannot
see is not a lock they can rely on.

**Broadcast sets (Part B).** The global boolean is gone. A tab holds an
explicit **list of pane ids**, and the picker is a checklist of that tab's
panes with each one named. Consequences of it being a list rather than "all":

- a pane opened after the set was chosen is **not** in it;
- closing a pane removes it from the set;
- marking a pane read-only removes it from the set;
- "Select All" excludes read-only panes, so the count is never a lie.

The indicator is deliberately hard to miss: every target pane gets a red
border and a `BROADCAST` badge, the status bar names all the targets, and the
status-bar chip carries the **count** (`bcast 3`) rather than just lighting up.
Clicking that chip while broadcasting is the emergency stop, and Ctrl+Shift+B
both arms and disarms so there is always one chord that stops it.

**The extra safety gate the spec asked for:** broadcasting a *multi-line*
paste to three or more panes raises a second modal, after the existing paste
guard, that lists the hosts by name and defaults to No. Every line runs as it
arrives, on all of them, simultaneously — it is the most destructive thing this
application can be asked to do.

**Workspaces, schema 2 (Part D).** A tab now stores every pane's profile, the
layout tree (as `PaneLayout::Serialize`), the focused pane and which panes were
read-only. The schema-1 fields are **still written**, so a build that predates
pane trees restores the workspace as a single split rather than failing; and a
file with no version field is read as schema 1. A file claiming a *newer*
schema is read as the oldest one this build understands rather than guessed
at. Restoring splits now actually happens.

Two things are deliberately never restored: **broadcast sets** (a workspace
that starts typing into four production hosts is not a feature — there is no
field for it, so the schema cannot express one) and **zoom** (coming back to
one pane filling the window with the rest invisible is a surprise, not a
restoration).

**Keyboard.** All on the existing Ctrl+Shift convention, so nothing a remote
application expects from a plain Ctrl combination is taken: arrows move focus,
Ctrl+Shift+Alt+arrows move the pane, `[`/`]` cycle, Z zooms, R toggles
read-only, B arms/stops broadcast. Every operation is also in the command
palette by name, and in a File → Pane submenu.

## 3. Part C — what was built, and what was not

**Not built: tab tear-out into a second window.** This is the one part of the
spec I decided against implementing, and the reason is structural rather than
effort. A second window needs its own `Device` (swap chain), `ParticleRenderer`,
`Bloom`, `Composite`, `PrimRenderer` and `GlyphSampler` — the last of which
carries a worker thread and three glyph atlases. All six are members of a
single `App` object created on the stack in `main.cpp`, and 96 call sites reach
the active tab through `Cur()`. Multi-window is an application-shell rewrite,
and the spec's own instruction was to extend this model rather than replace it.
Half-building it would have left a window system that mostly worked, which is
worse than not having one.

**Built: the prerequisite.** `App::BindSessionSinks` puts every parser sink
(writer, clipboard, title, OSC 7, images, OSC 133, remote resize) in one call.
It was previously written out inline at four creation sites, **and one of them
was missing the resize sink** — so a session created that way ignored a remote
application's `CSI 8 t`. All four now go through the single call. That
consolidation is exactly what makes a session re-homeable: binding is one
function of `App` and a session, so re-binding it to a different `App` is one
call rather than five scattered lambdas.

**The same root cause has a visible consequence.** A tab's root session object
holds the tab's identity, profile and layout, and a `Session` owns a running
worker thread, so it cannot be moved. **Closing pane 0 on its own is therefore
refused** with a message that says to close another pane or the tab. Lifting
that limitation and enabling tab tear-out are the same piece of work: sessions
have to live in a registry that outlives any one window, rather than being
owned by the object that renders them.

## 4. Verified by automated test

`tests/PaneLayoutTests.cpp` — 16 cases, and the invariant (**no two panes
share a cell, none leaves the grid**) is re-checked after every mutation in
every test. Suite total: **12,260 assertions in 246 test cases, all passing.**

| Spec test | Where |
|---|---|
| deeply nested panes | "a deeply nested layout stays disjoint and inside the grid" — splits until the tree refuses, checking the invariant |
| repeated split/close | "repeated split and close leaves no residue" — 200 rounds, invariant each time, ids still distinct |
| resize storms | "a resize storm cannot push a pane out of existence" — 500 grows then 1000 shrinks; and every grid size from 20x6 to 300x80 |
| zoom/unzoom | "zoom is a view state and restores the exact layout" — every rect compared before and after |
| workspace save/load migration | four cases in `WorkspaceTests.cpp`, including a pre-Stage-5 line with no version field, tabs that must not bleed into each other, and a file claiming schema 99 |
| broadcast target stability | ids are never reused (a closed pane leaves a null slot); asserted in the split/close case and enforced by `Split` refusing a duplicate id |
| local ConPTY + SSH side by side | **verified live**, not by test — see §5 |

Plus: splits refused rather than making a pane too small (at every level of
the tree, and without disturbing it), a failed `Move` that keeps every pane,
neighbour navigation by geometry rather than tree shape, serialisation
round-trips, and thirteen malformed layout strings — including one nested 500
deep and one 100 KB long — each rejected without touching the layout.

## 5. Verified by running it

A real local session (`--local cmd`) split several ways
(`scratchpad/panes_e2e.ps1`):

- **Nested layout** — pane 0 full-height on the left, the right side split
  horizontally, its lower half split again: four separate `cmd.exe`
  pseudoconsoles. The narrow panes **wrap their banner at their own width**,
  which is direct evidence that each pane's column count reached its own
  pseudoconsole rather than the tab's.
- **Dividers** drawn per split, with the focused pane's brighter.
- **Broadcast** — three panes with red borders and `BROADCAST` badges, the
  status bar naming all three, and the chip reading `bcast 3`.
- **Read-only** — badge in the pane, and "Pane is READ-ONLY — output only.
  Ctrl+Shift+R unlocks it." in the status bar.
- **Zoom** — the focused pane fills the window while the tab still reports
  four panes and the `split` chip stays lit, so nothing was destroyed.

## 6. Bugs found and fixed

**Workspace splits were never restored.** The loader had a `splitProfileId`
field, read it, and then printed "split panes are reopened as separate tabs"
instead of restoring anything. It now rebuilds the whole tree.

**One of the four session-creation sites was missing the resize sink.** A
session created down that path silently ignored `CSI 8 t`. Consolidating into
`BindSessionSinks` fixed it and made the class of bug impossible.

**The old split model could not be nested and had a fixed ratio.** Not a bug
as such, but "Split Vertical" on an already-split tab simply refused.

## 7. Not proven

| Item | Status |
|---|---|
| Tab tear-out / multi-window | **not built.** §3 explains why. Every test the spec lists for it — moving a tab between windows, window close with connected moved tabs — is therefore not run |
| Closing pane 0 | **refused by design**, same root cause as above |
| Pointer resizing of a divider | **not built.** `DividerAt` exists and returns the divider under a cell with its orientation and neighbours, and keyboard resize works; the drag itself is not wired to the mouse |
| Local ConPTY + SSH side by side | **half.** Four ConPTY panes verified live; no SSH server on this machine, so a mixed layout was not run |
| Read-only input blocking | the single check is in `SendToShell` and the badge was seen; **no test drives a keystroke through it** |
| Multi-line paste guard under broadcast | the gate is implemented and reads the target count; **not exercised** — it needs a clipboard and three connected panes |
| Resize storms against live PTYs | the tree is stress-tested; the *propagation* to a pseudoconsole was seen for four panes at one size, not swept |
| Pane-tree performance at depth | no measurement. `Rects()` walks the tree per frame and per query, which is fine at realistic depths but is O(panes) per call and called several times a frame |

## 8. Acceptance criteria

| Criterion | Status |
|---|---|
| Pane layouts are flexible and stable | yes — arbitrary nesting, and the disjointness invariant is asserted after every operation in every test |
| Moving tabs between windows never drops the session | **not applicable — tab tear-out is not built.** The prerequisite (one-call sink binding) is |
| Broadcast is powerful but hard to misuse | yes — explicit target lists, never auto-including a new pane, read-only excluded, count in the chip, per-pane borders, one chord to stop, and a second modal for multi-line to 3+ hosts |
| Workspace files remain compatible or migrate cleanly | yes — schema 1 still written and still read, no-version files read as 1, a newer version read as 1, four migration tests |
| No render/network ownership bugs during window/tab moves | **untestable here** — there are no window moves |
