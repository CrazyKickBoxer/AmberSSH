# Stage 4 — Modern Terminal Compatibility

Implementation report. What the audit found, what was built, what is proven by
test, what was proven by running it, and — at some length — what is not.

## 1. The audit

The spec asked for an audit of the cell model before any code. Findings:

| Item | State before Stage 4 |
|---|---|
| UTF-8 split across reads | **correct.** The accumulator persists across `Feed`, so a sequence broken by a socket boundary decodes |
| Combining marks | **broken.** A width-0 code point was *discarded*. "e"+U+0301 rendered and copied as "e" |
| Zero-width joiner | dropped; each emoji of a ZWJ sequence became its own cell |
| Variation selectors | correct — recorded as `CellEmojiVS` / `CellTextVS` flags and re-emitted on copy |
| Skin-tone modifiers | width 2 each, matching glibc; not joined |
| Regional-indicator flags | width 1 each (correct), but rendered as two separate letters |
| East Asian full-width | correct — `CellWideLead` / `CellWideTail`, with `HealOrphan` and `ClearWideAt` |
| Ambiguous width | resolved narrow, matching glibc |
| Surrogate-free internal form | **broken.** The decoder rejected > U+10FFFF but accepted surrogates and overlong forms |
| Font fallback | present and cached: active face → configured fallback → bundled faces → curated system families, memoised per code point |
| Grapheme clusters | **absent.** One `char32_t` per cell, with no way to express more |
| Cursor across wide cells | correct |
| Deleting/overwriting wide glyphs | correct |
| OSC 8 | target already stored separately, interned, bounded (2048 bytes, 4000 links); Ctrl+click already scheme-restricted to http/https/mailto |
| Kitty graphics | present (direct transmission, PNG and raw, chunked) |
| Sixel | present |
| iTerm2 | **absent** |
| Image memory | **unbounded in bytes.** A count cap of 64 only; 64 × 8 Mpx is 2 GB of RGBA |
| Compatibility fixtures | absent |
| Protocol diagnostics | absent |

So the work was: fix the two real corruptions, add clusters, add iTerm2, put a
real budget on images, and build the harness.

## 2. What was built

**`src/term/Graphemes.{h,cpp}` — clusters without breaking the cell count.**

The hard constraint is stated in `charwidth.h`: the number of *cells* a
sequence occupies must keep agreeing with the server's `wcwidth`, or every
erase and redraw after it desyncs. So the cell count is unchanged; what
changed is that a cell can carry a cluster.

Clusters are interned by content and addressed by an alias code point in
Supplementary Private Use Area-A. A cell stores the alias in its `cp`, which
means the glyph atlas, the particle pipeline and the GPU cell data — all keyed
by `char32_t` — needed no changes at all. Copy expands the alias back to the
real code points; the sampler expands it to shape the cluster.

Bounded: 4096 distinct clusters, 16 code points each, both counted in the
diagnostics. Past the cap the base character is kept and the mark dropped —
the same degradation as before, rather than unbounded growth.

**Deliberately not merged: ZWJ emoji sequences and skin-tone modifiers.**
glibc `wcwidth` gives every emoji in a ZWJ sequence its own two columns, so a
server lays out a family emoji as six. Joining it would render it beautifully
and desync the cursor from the shell — the one failure this terminal cannot
afford. **Regional indicator pairs are merged**, because both halves are width
1 either way, so the flag occupies the same two columns joined or not.

**`GlyphSampler::RasterizeCluster`** shapes the cluster with a Direct2D text
layout — DirectWrite applies the font's own mark-positioning tables, which
reproducing by hand would mean reimplementing script analysis — and feeds the
result into the *same* `Raster` → `ExtractPoints` pipeline as every other
glyph. So "é" is particles like the rest of the text, not a pasted bitmap.
A cluster that will not shape falls back to its base character rather than to
the missing-glyph box.

**UTF-8 decoder.** Overlong encodings and UTF-16 surrogate halves are now
refused. `C0 80` decoded to U+0000 and reached a cell; `ED A0 80` put U+D800 —
not a scalar value — into one. Both become U+FFFD and are counted.

**OSC 8.** A target carrying a control character is refused *entirely* rather
than stripped, because a stripped URI is a different target from the one the
server named and it would be the one opened. Hovering a link now shows its
destination in the status bar, and says when the scheme is one Ctrl+click will
not launch — a link whose visible text says one thing and whose target says
another is the oldest trick there is.

**iTerm2 `OSC 1337`.** Inline images only. `SetUserVar`, `CurrentDir`,
`RemoteHost`, `ReportVariable` and the shell-integration commands are ignored:
several of them set state or answer queries on the application's behalf, and
honouring a namespace that open on a remote host's say-so is how a terminal
ends up doing something nobody asked for. Without `inline=1` it is a file
transfer, which is declined — a terminal silently writing files a remote host
sends is not a feature.

**Image bounds.** A pixel budget (8 Mpx) enforced *before* allocation in all
three decoders, including inside Sixel's incremental canvas growth, where a
dimension cap alone let a declared 60000×60000 raster through. Per session: a
count cap, a 96 MB byte budget, and eviction of images whose rows have left
the scrollback — which nothing did before.

**Diagnostics.** F3 gains a protocol line: UTF-8 errors recovered, clusters
interned / on screen / shaped / fallen back, wide cells, live OSC 8 targets
and rejected ones, live images and their bytes, and image decode failures.

**Layer order.** Inline graphics moved to just above the terminal background
and *below* the text core and the particle field, which is the order the spec
specifies. They previously drew after the particles.

## 3. Bugs found and fixed

**Combining marks were discarded.** The headline: any decomposed text —
`e`+U+0301, Hebrew points, Arabic harakat, Thai tones, Devanagari matras —
rendered and *pasted* as the bare base letter. Silent, total loss of the mark.

**Overlong UTF-8 and surrogates reached the grid.** `C0 80` is the classic
overlong NUL used to slip a byte past a filter.

**An ESC inside a control string was swallowed.** `ESC ] 8 ; ; <uri> ESC [ 31 m`
appended `[31m` to the URI instead of setting a colour: the ESC was dropped and
the following bytes kept accumulating into the string. xterm ends the string
there; now so does this. This is both a correctness fix and the removal of a
way to smuggle text past a terminator.

**Sixel could allocate past its own budget** by growing its canvas
incrementally, since the size check only ran on the declared dimensions.

**`restoreForwards`-style dead setting, image edition:** images were capped by
count but not by bytes, and were never dropped when their rows scrolled away.

## 4. Verified by automated test

`tests/CompatFixtureTests.cpp` — 26 fixtures, each a captured byte stream fed
through the real `VtParser`, asserted against the **canonical grid state and
metadata**, never a screenshot. That is the point: a rendering change must be
free to alter every pixel and none of these may notice.

Suite total: **3663 assertions in 224 test cases, all passing.** Against the
spec's fixture list:

| Fixture | Covered |
|---|---|
| vim/neovim | yes — absolute addressing, tilde column, modeline, alt screen |
| tmux | yes — DECSTBM scroll region moves only its own rows |
| htop/btop | **only as alt-screen behaviour**; no captured htop stream |
| less | yes — smcup, reverse-video status, rmcup restores the screen |
| git coloured output | yes — SGR spans and that the reset really resets |
| truecolor gradients | yes — 32 distinct values, plus the colon sub-parameter form |
| OSC 7 | yes, BEL- and ST-terminated |
| OSC 8 | yes — target held separately from the text, span ends, control chars, oversize |
| OSC 133 | yes — marks change nothing in the grid |
| mouse tracking | yes — 1000/1002/1003/1006 recorded, nothing printed |
| bracketed paste | yes |
| alternate screen | yes — separate, restored, contributes nothing to scrollback |
| Unicode edge cases | yes — split reads, combining marks, flags, wide cells, overlong, surrogate, truncated, stray continuation, 5 KB of garbage |
| Kitty | yes — an absurd declared size is refused, a plausible one is accepted |
| iTerm2 | yes — decoded and placed, `inline=1` required, px/% sizing, other 1337 commands ignored, corrupt payload counted |
| Sixel | yes — a declared 60000×60000 canvas is not honoured |

Plus: an unterminated 20 MB control string cannot grow the buffer or reach the
grid, and OSC 52 writes the clipboard but a `?` query is dropped.

## 5. Verified by running it

A peer sending the whole edge-case set through a real session
(`scratchpad/unicode_e2e.ps1`):

- **`café` and `naïve` render correctly**, from base + combining mark arriving
  in a *separate write* — the fix working end to end through the shaper and
  the particle pipeline.
- **Flags** join into single two-cell clusters (this font has no flag glyphs,
  so DirectWrite draws the letter pair — which is what many terminals show).
- **Three malformed sequences** produce exactly three replacement characters
  and the stream continues intact.
- **The diagnostics line** reads
  `utf8-err 3  clusters 8 (screen 6, shaped 12, fallback 0)  wide 3
   links 1/27 (rejected 0)  images 2 (0.0 MB)  img-fail 0`
  — every counter live, and `utf8-err 3` is exactly the three bad sequences.
- **OSC 8** stores the target and shows nothing of it in the grid.

## 6. Not proven — and one thing that is broken

**Inline images do not render.** This is the important one, and it is not
subtle: both the iTerm2 image and a Kitty image of the same PNG *decode*
(`images 2`, `img-fail 0`), are placed, and reserve their cells — the row gaps
are visible in the capture — and **neither draws any pixels**.

The Kitty path is unchanged from before Stage 4, and behaves identically, so
this is a **pre-existing defect in the shared GPU draw path**, not in the
iTerm2 code added here. I ruled out the obvious causes — the descriptor heap
is bound, the SRV slots are inside it (`AllocSrv` throws on exhaustion and
does not), the shader uses only `color.a` so a zeroed RGB is harmless, and the
staging copy premultiplies correctly. What remains needs the D3D12 debug layer
or a frame capture, which I could not drive from here, and I was not willing
to guess at a fix in a GPU path I had not read properly.

So the acceptance criterion *"Kitty/iTerm2/Sixel image content is bounded and
rendered on a separate GPU layer"* is **half met**: bounded, yes, and on its
own layer in the specified order — rendered, no.

Everything else not proven:

| Item | Status |
|---|---|
| Cluster rendering for complex scripts | **not tested.** Latin combining marks were seen working; Devanagari, Arabic and Thai shaping were not |
| ZWJ emoji | deliberately not joined (§2). Rendered as separate emoji, as before |
| Ligature handling | **not addressed.** The spec asks that ligatures not violate cell semantics; this build does not form ligatures across cells, so the question does not arise — but that is an absence, not an implementation |
| Font-fallback metric drift | the fallback chain is pre-existing and cached; no measurement of advance drift was made |
| htop/btop fixtures | not captured — only the alt-screen behaviour they rely on |
| Image lifetime on resize, alt-screen entry, tab close | **not tested.** The reset and scrollback paths are; resize and alt-screen are not |
| GPU texture eviction under pressure | not exercised — 24 SRV slots with LRU, never filled in a test |
| Sixel against real `img2sixel` output | not run; the Sixel fixtures are hand-written |
| The 8 Mpx and 96 MB budgets | enforced in code and unit-tested for the decode path; **not measured** with a program actually trying to exhaust memory |

## 7. Acceptance criteria

| Criterion | Status |
|---|---|
| Unicode edge cases do not corrupt the grid | yes — and two ways they did are now fixed and tested |
| Wide/combining characters behave under cursor movement and editing | yes for wide (tested); combining marks now survive at all, with the cell count proven unchanged |
| OSC 8 opens semantic targets | yes, already did; now also shows the target and refuses a poisoned one |
| Kitty/iTerm2/Sixel bounded and on a separate GPU layer | bounded yes, layered yes, **rendered no** — see §6 |
| Existing particle text remains exact | yes — clusters go through the same raster→points path; no other glyph's route changed |
| Malformed graphics/OSC input cannot cause runaway allocation or parser corruption | yes — pixel budget before allocation, buffer caps, and the ESC-in-string fix |
