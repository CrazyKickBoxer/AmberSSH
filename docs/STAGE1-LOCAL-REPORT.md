# Stage 1 — Local Terminal, ConPTY, PowerShell and WSL

Implementation report. What was investigated, what was built, what was
verified by test, what was verified by hand, and what is not yet proven.

## 1. Investigation

The spec asked for this before any code. Findings, in its order:

**1. Session abstraction and worker-thread ownership.** `SshSession`
(`src/ssh/session.h`) is one session on one dedicated thread. It already
carried five protocols — SSH, Telnet, Rlogin, Raw and Serial — behind a single
interface, dispatched in `Start()` on `cfg.protocol` to one of three thread
bodies. The application only ever sees `Start / Send / RequestResize /
Disconnect / Output / PollEvent / Running`.

**2. Where bytes enter the parser.** Every transport pushes into the same
`SpscRing` (`m_output`). The UI thread drains it in `App::PumpSession` and
feeds `VtParser`. No transport touches the grid.

**3. Resize.** `RequestResize(cols, rows)` sets two atomics and a flag; each
thread body consumes them at its own pace. Callers are the window resize, the
font-size change and the initial session start.

**4. Keyboard input.** `Session::ssh.Send()`, from the input encoder. One
path, protocol-independent.

**5. Lifecycle.** `Disconnect()` sets a stop flag, unblocks any pending
prompt, aborts the socket or serial handle, then joins. `CleanClose()` decides
whether "close window on exit: only on clean exit" fires.

**6. OSC 7 / OSC 133.** Consumed entirely in the parser and `Session`; they
drive the journal, folding, command jumping and the spotlight. Nothing about
them is transport-specific — a local shell that emits them gets those features
for free.

**7. Profile serialization.** `ConnectionProfile` is a flat struct; the store
writes named JSON keys and reads them with a defaulting `Get<T>`, so an older
file loads and unknown keys are ignored. Schema version 2.

**8. What could become a common transport.** Nothing needed to. `SshSession`
**is** the boundary the spec describes, with the operations it lists under
different names. The lowest-risk design — and the one the spec asked for by
saying *do not start by rewriting all transports* — was to add Local as a
sixth protocol rather than extract an interface. No existing transport was
touched.

| Spec operation | Existing member |
|---|---|
| `Start()` | `Start(const SshConfig&)` |
| `WriteInput(bytes)` | `Send(const char*, size_t)` |
| `Resize(cols, rows)` | `RequestResize(int, int)` |
| `RequestDisconnect()` | `Disconnect()` |
| `ForceTerminate()` | `Disconnect()` + `ConPty::Terminate()` |
| `ReadOutput()` | `Output()` → `SpscRing` |
| `State()` | `Running()`, `PollEvent()`, `CleanClose()` |

## 2. What was built

**`src/platform/ConPty.{h,cpp}` — the pseudoconsole, under RAII.** Owns the
two pipe pairs, the `HPCON`, the attribute list, the process and a job object.
`Close()` releases them in the only order that does not hang: stdin write end,
then the pseudoconsole (which signals the child), then the read end, then the
handles. The job object carries `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so a
backgrounded child cannot outlive its tab.

Also here: shell discovery, WSL discovery, `ResolveExecutable` (expands
`%VARS%`, resolves a bare name against PATH), and the shell-integration
bootstraps.

**`SshSession::ThreadMainLocal`** (`src/ssh/transports.cpp`) — protocol 5.
Shaped like the serial transport, with one difference that matters: a pipe
read blocks with no timeout, so the reader runs on its own thread and the main
loop never waits on the child. Resize, input and disconnect stay responsive
while a build pours output.

**Profile model.** `Protocol::Local`, plus `localShellKey`, `localExe`,
`localArgs`, `localCwd`, `localEnv`, `localShellIntegration`. Serialized,
round-tripped, and validated (a local profile is valid on a shell key alone).
**Elevation is deliberately not a field** — the spec said not to implement it
silently, and a saved profile that could silently launch elevated is exactly
that. The Local page says so.

**UI.** A "Local" connection type and a Connection → Local page (shell picker
built from discovery, executable, arguments, starting directory, environment
overrides, shell-integration toggle). Host and port grey out for Local, as
they already did for Serial. File → New Local Session lists the discovered
shells; the palette carries `New Local: <name>` for each. A local tab is named
for its shell.

**`--local <key>`** opens a console straight away, for scripted testing.

## 3. Design decisions worth stating

**Shell keys, not paths.** A profile stores `"pwsh"` or `"wsl:Ubuntu"` and
resolves it at every launch, so it survives a PowerShell upgrade or a
distribution installed after the profile was saved. Typing into the executable
field pins it instead; that is the user's choice and is never overridden.

**Shell integration is per-session and touches nothing on disk.** The OSC 7 /
OSC 133 bootstrap is passed as launch arguments and dies with the process. A
test asserts the bootstraps contain no dotfile name and no redirection. It is
applied only when the profile has no arguments of its own. `cmd.exe` gets
none — it has no prompt hook that can carry an escape sequence, and inventing
one would mean writing to the registry.

**WSL names are discovered, never hardcoded**, from `wsl.exe --list --quiet`.

## 4. Verified by automated test

`tests/LocalSessionTests.cpp`, 8 cases in the suite of 158 (1125 assertions,
all passing):

- WSL list parsing: CRLF, BOM, blank lines, no trailing newline, empty output,
  hyphens and dots in names, and localised error sentences.
- Executable resolution: absolute paths, `%VARS%`, bare names via PATH, and an
  unresolvable name coming back unchanged so the error can name it.
- Shell integration: OSC 133 A/B/C/D and OSC 7 present for the shells that get
  it, absent for `cmd` and unknown shells, and no dotfile written.
- Discovery: every entry has a key, name and existing executable; WSL entries
  carry `--distribution`.
- Profile round trip for `Protocol::Local`, and validity on a shell key alone.
- Environment-override parsing, mirroring the transport's own loop.

## 5. Verified by hand, on this machine

- **cmd.exe** — launches, prompt in the user's home directory, banner and
  prompt render through the particle path (`docs/` capture `local_cmd.png`).
- **Windows PowerShell** — launches; the status bar shows `/C:/Users/crazy`,
  which is **OSC 7 arriving from the per-session bootstrap**. Shell
  integration works.
- **Tab naming** — reads "Command Prompt", not `C:\Windows\System32\cmd.exe`.
- **Startup is not blocked** — see the bug below.

## 6. Two bugs found and fixed during the work

**An unbounded read in the WSL probe hung startup.** `CaptureOutput` applied
its timeout *after* a read loop that ran until the pipe closed, so a child that
never exits — or one that hands its write end to something that outlives it —
blocked the thread for ever. It ran on the UI thread during `Init`, so the
window never appeared. Found by launching the app rather than by reading the
code. It now peeks before every read and enforces a deadline, terminating the
child if it must. This is precisely the stall path the spec's performance gate
forbids.

**The WSL parser accepted an error sentence as a distribution.** With WSL not
installed, `wsl.exe` prints "The Windows Subsystem for Linux is not
installed…", which my first filter (reject lines with a colon *and* a space)
let through — it has no colon. A distribution name never contains a space,
while every localised header and error does. Caught by a test written for
exactly that case before it was ever seen on screen.

One design mistake was also caught before it shipped: `Disconnect()` calls
`CloseHandle` on whatever sits in `m_serial`, and the local transport had put
a stack pointer there. Local now has its own cancel slot, `m_localRead`, which
`Disconnect` only *cancels* — the `ConPty` owns and closes it.

## 7. Not yet proven

Stated plainly, because the spec asks for a matrix and this is what of it has
actually been run:

| Item | Status |
|---|---|
| cmd.exe | **verified** — launched, prompt, render |
| Windows PowerShell | **verified** — launched, OSC 7 confirmed |
| PowerShell 7 | not run: `pwsh.exe` is not installed on this machine |
| WSL distribution | **cannot be run here** — WSL is not installed; `wsl.exe --list` reports so. Discovery and the parser are unit-tested; a live distro is untested |
| Git Bash | not run: not installed here |
| `vim` / `nano`, `htop`, `tmux` | not run — they need WSL or a remote host |
| truecolor, alternate screen, mouse tracking | not run against a local shell |
| Resize during drag / maximize / font change / full screen | **not measured.** The path is the same atomics the SSH transport uses, consumed each loop iteration, and the initial size is correct in both captures — but no resize was exercised |
| Ctrl+C to the child, AltGr, dead keys, UTF-8 input | not exercised — input goes through the same encoder as every other protocol, but that is an argument, not a measurement |
| Handle/thread leaks over repeated open/close | **not measured** |
| Idle CPU, throughput, frame time | **not measured** |

The performance gate and the leak check both need a session opened and closed
many times with a monitor attached; neither has been done. Nothing in this
report should be read as claiming otherwise.

## 8. Acceptance criteria

| Criterion | Status |
|---|---|
| Local sessions behave as first-class tabs | yes — same Session, same grid, same renderer |
| PowerShell/cmd/WSL render through the same terminal model | cmd and PowerShell yes; WSL untestable here |
| Resize and input are correct | wired, not measured |
| Existing SSH behavior unchanged | no existing transport was touched; the full suite passes |
| No handle/thread leaks in repeated open/close | not measured |
| Profiles persist correctly | yes, round-trip tested |
| Unit/integration tests pass | yes — 158 cases, 1125 assertions |
| Implementation report | this document |
