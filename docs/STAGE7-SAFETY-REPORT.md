# Stage 7 — Host Sigils, Blast Radius and Privacy Cloak

Implementation report for the first slice of the *Next Generation Inventions
Roadmap*: the deterministic safety-and-identity layer the document keeps
returning to. What was built, what is proven by test, what was seen on screen,
what was deliberately not claimed, and what is still not implemented.

## 1. Scope — and what was left out, with reasons

The roadmap spans Stages 7–13 and fourteen inventions. Three of them cannot be
built here, for reasons that are not effort:

| Not built | Why |
|---|---|
| **X11 forwarding with a bundled server** | requires shipping a third-party X server. The roadmap itself says to do a licensing and security review first, and downloading one needs the user's permission. There is also no Linux host on this machine to forward from. |
| **AmberBridge helper** | requires cross-compiled, signed Linux binaries and a signing key. The roadmap's own rule — never store private helper signing keys — means the key cannot live here either. |
| **Wayland RemoteApp** | a remote-desktop codec stack, and again no Linux host. |

Everything else in the roadmap that needs a live remote (agent forwarding
audit, port-forward binding rules, mosh-style prediction) is untestable here
for the reason established in every earlier stage: **there is no SSH server and
no WSL on this machine.**

What *is* deliverable and provable without a server is the layer that decides
things: who a host is, what a command will do, and what must not be shown. All
three are pure functions of text, which means all three can be tested
exhaustively. That is what this stage is.

## 2. What was built

Three modules, each with no Windows, renderer, libssh2 or terminal dependency,
plus two skinned modals and the wiring.

### `src/security/HostSigil.{h,cpp}` — the host's figure

A 43-character base64 fingerprint is not compared by anyone. `MakeSigil` turns
one into a small drawing: five to eight nodes on a jittered ring, ring edges
plus one to three chords, corner cuts, and a border rhythm — all derived from a
splitmix64 stream seeded by an FNV-1a hash of the fingerprint body.

Three design rules, each of which is a test:

* **The same key is always the same figure.** FNV-1a and splitmix64 are used
  precisely because they are identical on every compiler and every machine. A
  sigil that drifted between builds would be worse than no sigil: people would
  learn to ignore the change that matters.
* **The key is the identity, not the way it was printed.** `SigilSourceText`
  normalises `ssh-ed25519 SHA256:x`, `SHA256:x` and `SHA256:x user@host` to the
  same body, so the same host gets the same figure from any source.
* **Hue is never the only cue.** Colour is one signal among node count, filled
  versus hollow nodes, chords, corner cuts and border rhythm. The test asserts
  that *every* pair of hosts whose hues fall within 15° differs in geometry.

`SigilDescribe` gives a screen reader a sentence that names the shape and never
mentions colour. `SigilMnemonic` gives a four-character pronounceable name for
saying out loud on a call — and a test records, deliberately, that mnemonics
*do* collide, so the UI can never present one as proof of identity.

### `src/security/BlastRadius.{h,cpp}` — what a command will do

The roadmap's rule is explicit: **AmberSSH must not use an LLM as the authority
for destructive-command safety.** So this is a hand-written parser, and being
the authority means being pinned down by tests.

`AnalyseCommand` splits the line the way a shell would (quotes, escapes,
unterminated-quote reporting), walks past `sudo`/`doas`/`env`/`nohup`/`VAR=`
prefixes to the real program, and dispatches to an analyser for `rm`, `mv`/`cp`/
`rsync`, `chmod`/`chown`, `systemctl`, `docker`/`podman`, `git`, `kubectl`/
`helm`, package managers, `mkfs`, `dd`, `shutdown`, and SQL. It returns a level,
a list of findings, and — the part that matters — a flag saying what it could
**not** read.

* A pipeline or a chain is truncated at the first separator and the report says
  "only the first command in the chain was examined". It does not analyse the
  tail: `rm -rf ./tmp | tee removed.log` naming `removed.log` as something being
  deleted would be a confident lie, and a warning that names the wrong file
  teaches people to ignore warnings.
* `$(...)`, backticks and `$VAR` make the report explicitly incomplete, and any
  finding resting on one is marked `uncertain`. The headline then ends "(unable
  to determine exactly what this expands to)".
* `sudo` creates no risk on its own. It raises an already-flagged command by one
  level, because it removes the permission error that would otherwise have
  stopped it.
* Over-reporting is treated as its own failure. `rm -rf /` is Critical;
  `rm -rf ~/project/build` is High; `rm notes.txt` is Medium; `git push` is
  nothing. If every `rm` were Critical the Critical confirmation would stop
  meaning anything.

`ConfirmFor` reserves the typed-hostname friction for Critical alone. Making
every warning require typing is how people learn to type without reading.

### `src/security/PrivacyCloak.{h,cpp}` — masking likely secrets

The roadmap's honesty rule is quoted verbatim in the header, and there is a test
whose only job is to enforce it:

> AmberSSH says "potential secrets are being masked".
> AmberSSH must **never** say "this session is safe to share".

`CloakStatusText()` is the single place that claim is made. The test asserts the
string contains "potential secrets" and "does not guarantee", and does **not**
contain "safe", "secure" or "all secrets".

`FindSecrets` returns spans, so the grid is never modified — masking is a
presentation decision applied at draw time like every other overlay. Detectors:
known credential prefixes (AWS, GitHub, Slack, Stripe, Google, GitLab, npm,
DigitalOcean, SendGrid, JWT), `NAME=value` where the name looks like a secret,
`--password`/`--token`/`-p` arguments, credentials in URLs and query strings,
connection-string fields, PEM blocks, and a generic long-token rule.

Three false-positive guards, each of which exists because a masker that fires on
ordinary output gets switched off and then protects nobody:

* The generic rule requires mixed case **and** digits, which keeps it off
  English words, paths, commit hashes and UUIDs.
* `=` is not a base64 body character, only trailing padding. Treating it as one
  glued `NAME=value` into a single token and covered the name too.
* A connection-string field ends at its semicolon. With no semicolon it is not a
  connection string, and reading it as one covered the rest of the line — which
  on a terminal row means the rest of the command. **This was found on screen,
  not in a test**, and the test came after.

`Authorization: Bearer <token>` hides the token and keeps the word "Bearer":
covering the scheme while leaving the token visible would be worse than doing
nothing, because the line looks handled and is not.

### `src/ui/SafetyDialog.{h,cpp}` — the two modals

Painted, not composed from system controls, so both take the active interface
skin. Neither has a default that says yes: focus lands on the safe button, Esc
is No, and there is no Enter-is-Yes — a decision that can be made by a keystroke
already in flight is not a decision.

* **Host key.** The sigil in a well, the mnemonic under it, the fingerprint
  verbatim in a monospaced well, and the shape written out in words. When the
  key has *changed*, an alarm band across the top, the sigil and mnemonic in the
  skin's danger colour, and wording that says plainly this is both what a
  rebuild looks like and what an interception looks like.
* **Blast radius.** The level band, the command **exactly as typed** in a well,
  the findings worst-first, what the parser could not read, and — for Critical —
  the hostname that must be typed before the Run button enables.

## 3. Wiring

| Surface | Where |
|---|---|
| Host-key confirmation | replaces the `MessageBoxW` in the `hostKeyPending` branch of `PumpSshEvents` |
| Blast radius | `App::SendToShell`, the single choke point every route into the shell passes through — typing, paste, snippets, block rerun, broadcast |
| Privacy cloak (screen) | `App::RebuildCloak` builds a per-cell mask before compose; the one place compose reads a cell applies it |
| Privacy cloak (copies) | `App::CloakText` uses `MaskLine`, whose fixed `[redacted]` marker also hides the length |
| Menu | File → Safety: the cloak, its two opt-in classes, and the five risk policies |
| Palette | all eight commands, with their current state |
| Status bar | a `cloak N` chip carrying the number of covered runs on screen |
| Keyboard | Ctrl+Shift+M toggles the cloak — it is reached in the second before a screen share starts, not from a settings page |
| Settings | `cloak`, `cloakAddrs`, `cloakHome`, `riskPolicy` |

Defaults: the cloak is **off** (masking every address while debugging a network
makes the terminal useless), the risk policy is **High and critical** (a
confirmation nobody asked for is far cheaper than the command it stops, and at
that threshold the box stays rare enough to read).

## 4. What is proven by test

The suite went from 321 test cases / 47,054 assertions to **322 / 47,067**, of
which the three new files contribute 53 cases and roughly 34,500 assertions
(most of them from the sigil's property sweeps over hundreds of generated
hosts).

**Sigils** — determinism; one changed character changes the geometry, not just
the colour; the three print forms of one key agree; nodes stay inside the box at
every seed; the ring keeps the figure connected; no self-edges; 500 distinct
hosts give 500 distinct figures; every close-hue pair differs in shape; the
mnemonic alphabet excludes the misread consonants; mnemonics collide and the
test says so; the description never mentions colour; `fromFingerprint` is false
for anything that is not one.

**Blast radius** — 24 cases. Ordinary commands are not flagged. The command is
never rewritten. Root paths, recursive deletes, wildcards, `sudo` resolution,
git history rewrites, container and cluster deletions, whole-machine and
whole-disk commands, permission changes, SQL with and without a `WHERE`.
Pipelines are truncated and the tail is proven *not* to appear in the findings.
Substitution and unterminated quotes are admitted. Policy gating in both
directions, and `Off` never flags anything.

**Privacy cloak** — 20 cases. The status text. Disabled means untouched. Known
credential shapes. Values masked, names kept. The auth-header scheme. URL and
query credentials while `page=2` survives. Password flags, and `-p 2222` left
alone. PEM blocks across lines. Ten lines of ordinary terminal output that must
survive byte-for-byte. Opt-in classes. Span ordering and bounds over adversarial
inputs including `""`, `"="` and `"--token"`. Masking twice changes nothing. The
redaction report never contains what it redacted.

## 5. What was seen on screen

A developer flag `--preview-safety [--changed]` opens both modals with sample
content and nothing connected — the same role `--diag` plays for the terminal —
so they can be reviewed on every skin without a server.

* Both boxes captured on **all fifteen interface skins**. LCARS gets its elbow
  frame, block column and foot bar; Blueprint draws the alarm band as an outline
  rather than a fill; Letterpress, Horologe, Tenmoku, Atelier, Reference and
  Solder Mask each get their own ground. The changed-key alarm was captured on
  Tenmoku.
* Three layout defects were found in these captures and fixed: the changed-key
  paragraph overflowing into the fingerprint well (now measured, not assumed);
  the confirmation prompt drawn underneath its own edit control; and
  "ACCEPT AND CONNECT" clipping on the skins that letter in capitals.
* **End to end in a real ConPTY session** (`--local cmd`): a line containing a
  password assignment, an AWS key and a URL credential was echoed, the cloak was
  switched on, and exactly those three runs were covered on both the command and
  its output — `and https://u:` and `@host/x` survived, and the chip read
  `cloak 6`. This is where the connection-string over-masking bug was caught.
* **The blast radius fired from the real send path.** Typing `rm -rf /` at a
  `cmd.exe` prompt produced the Critical box with the correct command text, the
  correct findings, and the note that the text was read back from the screen
  because this shell reports no prompt marks. Esc left `rm -rf /` sitting on the
  line, unsent.

## 6. What is NOT proven

Said plainly, because the whole stage is about not overstating what is known.

* **No SSH server exists here, so the host-key dialog has never been driven by a
  real host key.** It was exercised with a sample fingerprint through
  `--preview-safety`. The wiring into `SshEventType::HostKeyPrompt` compiles and
  reads correctly and has never run.
* **The changed-key detection is a substring test** on the transport's wording
  ("changed", "mismatch", "does not match"). It has never been checked against
  what libssh2 actually reports for a real mismatch, because producing one needs
  two servers.
* **The blast-radius fallback is a heuristic.** With OSC 133 the command text is
  exact. Without it, several readings of the row are produced and the worst is
  taken — safe in direction, but a prompt shaped unlike anything anticipated
  could still yield a reading that finds nothing. The dialog says so; that does
  not make it fixed.
* **The cloak's on-screen cover is as long as the secret.** A fixed grid cannot
  reflow, so the length of a masked value is visible. The copy path uses a
  fixed-width `[redacted]` instead. Both behaviours are deliberate; only one of
  them hides length.
* **Detection is pattern matching and will miss things.** A secret with no
  recognisable shape, in a variable with an innocuous name, passes straight
  through. This is why the status text says what it says.
* Paste, snippet and broadcast routes into `SendToShell` are covered by
  construction — they all pass the same choke point — but only the typed route
  was driven live.
* `RebuildCloak` walks every visible cell once per frame to hash the screen when
  the cloak is on. It costs nothing measurable at the sizes tested and was not
  profiled at 4K.
* Nothing here has been tested at a DPI other than the one on this machine.
