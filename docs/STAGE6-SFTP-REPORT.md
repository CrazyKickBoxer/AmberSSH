# Stage 6 — SFTP Pro: Directory Sync, Resume and Verification

Implementation report. What the investigation found, what was built, what is
proven by test, what was seen on screen, and — at length, because it is most
of the risky surface — what was never executed.

## 1. Investigation

| Item | State before Stage 6 |
|---|---|
| SFTP channel ownership | `SftpClient` owns its own socket, libssh2 session and SFTP subsystem, and authenticates independently with the same profile and retained secrets. It never touches a terminal session's shell channel |
| Transfer queue | `Transfer` with `state` (queued/running/done/failed/cancelled), `done`/`total` atomics and a `cancel` flag; a `std::vector<std::shared_ptr<Transfer>>` per tab under a mutex |
| Threading | **one worker thread per browser tab**, running a `std::deque` of `std::function<void(SftpClient&)>` jobs serially. The UI polls snapshots |
| Concurrency | already bounded — at one. Nothing could open unlimited operations because nothing ran in parallel |
| Edit/upload watcher | `EditWatch { remote, local, mtime }`, polled for a newer mtime |
| Listing | `SftpEntry` already carried name, dir, link, size, mtime, perms, uid, gid — everything a comparison needs |
| Taskbar progress | wired, driven from the aggregate queue progress |
| Cancellation | the `Progress` callback returns false; the transfer aborts with the error string `"cancelled"` |
| Path encoding | remote paths are UTF-8 `std::string`, local are `std::wstring`, converted at the boundary with `CP_UTF8` both ways |
| Resume | **absent.** `Download` opened the local file `"wb"` and `Upload` used `FXF_TRUNC` — both always started at zero |
| Verification | absent |
| Comparison / sync | absent |

## 2. What was built

**`src/ssh/SyncPlan.{h,cpp}` — the decision layer, as a pure module.** No
sockets, no libssh2, no Windows handles, no UI. That split is the point of the
stage: the dangerous parts of a sync tool are not the byte pushing, they are
*"are these two files the same"*, *"is it safe to append to this partial"*,
*"which of these steps deletes something"* and *"is this failure worth
retrying"*. Every one is a function of metadata, so every one is tested
exhaustively without a server.

It holds comparison and its exclusion globs, the planner and its three
directions, the resume decision, the retry classification, and the rate/ETA
arithmetic.

**Resume is safe rather than optimistic**, and that is one specific rule:
appending is allowed *only* when the source's size and mtime were **recorded
when the partial was written** and still match. Without that record, N bytes
on disk might be the first N bytes of a different version of the file, and
appending would produce a file that is corrupt in a way no size check can
detect. So no record means restart. So does a source that changed size, a
source modified since, a partial larger than the source, a server that cannot
seek, and an unknown source size. Every decision carries a reason string,
which the queue shows — a restart that looks like unexplained lost progress is
a support call.

**`src/utility/Hash.{h,cpp}` — SHA-256** via the OS provider rather than a
bundled implementation, because it has to agree byte for byte with whatever
`sha256sum` on the far end produces. Streaming, so verifying a 4 GB download
does not need 4 GB of memory. `ParseRemoteSha256` finds a digest in whatever
the host printed — coreutils, `shasum`, BSD `sha256`, or a login banner in
front of any of them — and reports *no digest* when there is none rather than
guessing.

**Verification gates success.** `TransferSucceeded` returns false when
verification was required and the result was `Failed` **or `Unavailable`**.
That second one matters: if "we could not obtain a hash" counted as success,
the guarantee the user asked for would silently become "we copied some bytes
and hoped".

**Comparison** classifies every path into the eight states the spec lists, and
refuses to guess: no timestamp on one side is `Uncertain`, not `Identical`; a
symlink is `Uncertain` and never followed; matching mtime with differing size
is `SizeDiffers`, which is the one combination that means something is
actually wrong. Tolerance defaults to 2 seconds, because SFTP reports whole
seconds and FAT stores two-second granularity.

**The planner** produces an ordered plan — directories created before their
contents, deletions after theirs, asserted as an invariant — and never
overwrites a destination that is *newer* than its source in a one-way sync.
Two-way reports a conflict rather than merging; an explicit preference can
resolve a size clash, but `PreferNewer` cannot (there is no newer copy when
the times agree), and the conflict correctly stands.

**Queue upgrades.** Every field the spec lists: operation, source,
destination, bytes, speed, ETA, state, verification state, retry count, error.
Speed and ETA are *derived* from a start timestamp rather than stored, so a
stalled transfer's numbers decay instead of freezing at their last good
reading, and both return "-" rather than a guess when there is not enough
information. New controls: Pause Queue / Resume Queue, Retry Failed, Clear
Done, Cancel Selected (existing), Verify toggle, Compare Folders, Sync
direction, Exclusions, Delete-extraneous.

**Retry Failed only re-queues failures worth retrying.** A permission error or
a full disk fails identically every time; re-queueing it would look like the
button did nothing. The skipped count is reported.

**Metadata.** Timestamps are applied on both sides. Unix mode bits are applied
on upload and **explicitly not** on download, with the queue detail saying
"remote mode bits not applied: Windows has no equivalent" — the spec asked for
limitations to be exposed, and silently dropping them is the opposite.

## 3. Deliberate decisions worth stating

**Deletions are planned, counted and shown — but not executed.** The planner
produces `DeleteRemote`/`DeleteLocal` steps, the plan dialog counts them on
their own line, and the button that enables them asks first. The executor then
queues only transfers, and says "No deletions were performed."

This is a real gap against the spec, which lists "optional delete extraneous
files" as an executable action, and I am flagging it rather than presenting it
as a feature. My reasoning: an unattended recursive delete driven by a
comparison is the single most destructive thing in this stage, and I could not
test it against a real server. Shipping it unexercised seemed worse than
shipping the plan that shows what *would* go.

**Concurrency stays at one transfer per tab.** The pre-existing worker is
serial, which already satisfies "do not open unlimited SFTP operations".
Raising it would mean a second SFTP channel and a real scheduler; not done.

**The comparison walk is bounded** at 200,000 entries and 32 levels per side,
and says so when it truncates, so a comparison started at a filesystem root
cannot exhaust memory before the user sees anything.

## 4. Verified by automated test

`tests/SyncPlanTests.cpp` — 20 cases. Suite total: **12,498 assertions in 268
test cases, all passing.** Against the spec's own test list:

| Spec test | Where |
|---|---|
| planner output | "a one-way plan uploads what is missing and leaves the rest", plus ordering |
| conflict detection | "a two-way sync asks rather than inventing a merge"; "type differences and links are always conflicts" |
| exclusion rules | "glob exclusions match the way a user expects" — anchoring, trailing slash, `*` vs `**`, case, and that an excluded path can be neither transferred nor deleted |
| resume offset calculation | "resume is safe rather than optimistic" — eleven sections, including the no-record case |
| hash verification | published SHA-256 vectors, real files whole and by range, four remote output layouts, and that two failures to hash never read as a match |
| retry state machine | permanent vs transient classification, the attempt cap, the backoff ladder, and that a cancelled transfer is never retried |
| cancellation | the `"cancelled"` error is classified permanent, so it is never retried |
| schema persistence | **not applicable — sync profiles are not saved.** See §6 |

Plus: timestamp tolerance, refusal to guess without timestamps, that
comparison is non-destructive by construction, that a one-way sync never
overwrites a newer destination, that deletion is opt-in and counted, ordering
of mkdirs and deletes, and Unicode and long paths treated as opaque bytes.

## 5. Verified by running it

The browser opens with every new control, under two very different skins
(`scratchpad/sftp_e2e.ps1`): Pause Queue, Retry Failed, Verify, Compare
Folders, Sync direction, Exclusions, Delete-extraneous — all owner-drawn by
the existing skin abstraction with no bespoke code — and the queue's new
**Speed** and **ETA** columns beside Progress and Status.

One note on method: the first Letterpress capture showed only the "Transfer"
header. That is the capture-timing artefact this project has hit before (a
`PrintWindow` that races child-control painting), not a paint bug — the retest
at the same settings shows all seven headers. I mention it because the first
image would otherwise look like a regression I had introduced.

## 6. Not proven — which is most of the transfer path

**There is no SFTP server on this machine.** No `sshd`, no OpenSSH Server
service (established in Stage 2 and unchanged). The spec asks for "a
disposable SFTP test server for integration tests" and I did not have one.

**No transfer was executed at all.** Everything below is written, compiles,
and is reachable, and none of it has moved a byte:

| Item | Status |
|---|---|
| `DownloadFrom` / `UploadFrom` resume paths | **never run.** The seek-and-append logic, the `"ab"` open, the short-write-is-a-full-disk check |
| Post-transfer verification end to end | **never run.** The hashing is tested; `RemoteSha256` talking to a real host is not |
| `RemoteSha256` command selection | **never run** against any host. The three-way `command -v` fallback and the quoting are untested in situ |
| Metadata preservation | **never run.** `SetTimes` / `SetMode` against a real server |
| The comparison walk | **never run** against a real remote tree. `WalkRemote` is untested; `Compare` and the planner it feeds are tested exhaustively |
| The plan dialog and queueing | **never run** — it needs a comparison, which needs a server |
| Pause / Resume / Retry Failed | **never run** against live transfers |
| Speed and ETA in the queue | the arithmetic is tested; the columns have never shown a moving number |
| Every Part H failure case | **none run** — network interruption, reconnect during transfer, disk full, permission denied, remote file changing mid-transfer, local file changing mid-upload, hash mismatch, path too long, Unicode filenames on the wire, symlinks, partial temporary files |

**Not built at all:**

- **Atomic replace.** The spec asks not to destroy a valid destination until
  the replacement is safely complete "where atomic rename is available". An
  upload still truncates the destination in place. Doing it properly means
  writing to a `.part` and renaming, and the interaction with resume (which
  needs the partial to *persist* under a stable name) needs designing, not
  improvising.
- **Drag and drop, two of three directions.** Explorer → pane already existed
  (`WM_DROPFILES`) and still works. Pane → Explorer and remote → Explorer need
  an `IDataObject` with `CFSTR_FILEDESCRIPTOR` and delayed rendering — real
  COM work, not started.
- **Session Guardian integration.** A paused or failed transfer does not offer
  to resume after an SSH reconnect.
- **Saved sync profiles.** No schema, hence no persistence test.
- **Executed deletions.** §3.

## 7. Acceptance criteria

| Criterion | Status |
|---|---|
| Individual transfers remain as simple as before | yes — the existing buttons and the edit/upload watcher are untouched; the new controls are additions |
| Resume is safe rather than optimistic | **the decision is**, and it is tested hard. The code that acts on it has never run |
| Optional verification catches corruption | the hashing and the gating are tested; **never exercised against a remote digest** |
| Directory comparison is accurate and non-destructive | non-destructive by construction and tested; accuracy against a real remote tree is unverified |
| Sync always shows a plan before destructive actions | yes — and it goes further than asked: destructive actions are shown and then not executed |
| Queue recovery after network failure is predictable | the retry rules are tested; **recovery itself was never observed** |
| Existing SFTP edit/upload behaviour intact | believed so — untouched code paths, and the browser opens and lists correctly — but not re-tested against a server |
