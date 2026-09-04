# AmberX — Phase 8 gate

The last phase: a compatibility matrix, automated protocol tests, fuzzing,
performance measurement and packaging. This is the report, and it is the one
with the most NOT RUN in it — deliberately, because the two things Phase 8
most wants are a fresh Windows machine and a Linux host with applications on
it, and this session has neither.

Measured by `AmberSSH.exe --preview-amberx` (135 checks, no failures, twice;
transcript in `docs/amberx/phase8-preview-run.txt`) and by the regression
suite (384 cases, 80,802 assertions).

## The prompt's six gate criteria

```
1. clean install on a fresh Windows 11 x64 machine ......... NOT RUN
     There is no fresh machine here. tools\package-amberx.ps1 builds the
     bundle that would be installed — 54 files, 28 MiB, licence gate
     passing — and the bundle is self-contained by construction: two
     executables, four approved DLLs, fonts, shaders, and the documents.
     Whether it starts on a machine that has never had a compiler on it is
     exactly the thing that has to be tried rather than argued.
2. no external X server is installed ....................... PASS
     Nothing in the bundle installs, registers or requires one, and there
     is no code path that reaches an external display when Remote GUI is
     on: AmberSSH launches AmberXHost from beside its own executable and
     talks to it over a private pipe.
3. licence gate and SBOM verification from the artifacts ... PASS
     The packaging script walks what the two binaries actually import and
     refuses to build a bundle carrying anything that is neither a Windows
     system library nor a component the matrix approves. It refused twice
     while being written, which is the only evidence that a gate works.
4. compatibility suite and fuzz regressions pass ........... SPLIT
     Fuzz: PASS. Five parsers, 800,000 mutation iterations across two
     seeds, no failures; the corpus replays on every test run.
     Compatibility: NOT RUN. No application has been run against AmberX.
     docs/amberx/COMPATIBILITY.md is the form, published empty.
5. performance and resource measurements documented ........ PASS
     docs/amberx/PERFORMANCE.md, from numbers the preview prints on every
     run: 31 ms startup, 31 us per round trip, no memory growth across
     1,000 resources.
6. uninstall removes AmberX, preserving intended user data . BY DESIGN
     AmberX writes nothing outside the bundle. Its state is the host
     process, which dies with the session; its only file is
     %TEMP%\amberx-host.log. Deleting the bundle removes AmberX entirely
     and leaves %LOCALAPPDATA%\AmberSSH — profiles, settings, the journal —
     which is the data AmberSSH intends to keep. Not run as an installer
     step because there is no installer yet.
```

## Automated protocol tests

The prompt lists fourteen. Where each one is:

| test | where | status |
|---|---|---|
| byte-order variants | preview: a big-endian client completes setup and gets a big-endian reply | **PASS** — exercises the whole swapped request vector |
| setup / authentication success and failure | preview: good cookie accepted, wrong cookie refused, expired authorization refused | PASS (Phases 2 and 5) |
| truncated packets | preview: a request split across two arrivals still completes | PASS |
| request length boundaries | preview: a zero-length request is refused with BadLength; an over-long one does not wedge the server | PASS |
| BIG-REQUESTS boundaries | preview: enabled, and a 2 MiB property refused by the limit | PASS (Phase 5) |
| invalid resource ids | preview: an id outside the client's range gets BadIDChoice | PASS |
| disconnect during every lifecycle stage | preview: mid-setup, mid-request, and after a flood | PASS |
| window/pixmap/property destruction order | preview: a parent with a child, a property and a pixmap, destroyed parent first | PASS |
| selection ownership changes | preview: the clipboard bridge, three modes | PASS (Phase 6) |
| multiple clients, concurrent channels | preview: two clients, three windows, one closed underneath | PASS (Phase 3) |
| cross-session isolation | preview: two hosts at once; each report lists only its own windows; an id from one is BadDrawable in the other | **PASS** |
| SSH EAGAIN / fragmentation | preview: a client whose bytes arrive one per frame | PASS (Phase 4) |
| host restart / crash | preview: the crash test | PASS (Phase 5) |
| GPU device loss | — | **not applicable**: no GPU path, frames are system-memory DIBs |
| DPI and monitor changes | RANDR rebuilds on WM_DISPLAYCHANGE | **implemented, not tested** — needs a second monitor to plug in |

No external conformance suite was imported. The licence and dependency
closure of the obvious candidates was not examined, and the prompt's rule is
that it must pass the gate first; these tests are written from the protocol
specification instead.

## Fuzzing

`tests/AmberXFuzzTests.cpp`, five persistent targets:

| target | what it reads |
|---|---|
| framing | AmberXControl frames — what a subverted host could send |
| report | the host's periodic report, the one message with a counted list |
| window-action | a shelf action |
| handshake | every fixed-length control payload |
| setup | the X11 setup packet, which arrives straight off an SSH channel from a remote machine and is the most exposed parser in the feature |

The driver is a seeded mutation loop rather than libFuzzer, so it runs
wherever the test suite runs with no extra toolchain, and it is
deterministic: a failure is reproducible from the seed it prints. The same
functions take a pointer and a length so libFuzzer can drive them where it is
available.

**Run:** 400,000 iterations on the default seed and 400,000 on a second seed,
plus 20,000 on every ordinary test run. No failures. The framing target also
checks a round-trip property — anything the decoder accepts must re-encode to
the same bytes, so a decoder that invents a message fails even without
crashing.

`tests/corpus/amberx/` holds the regression cases, one file each, replayed
against every target on every run. It is empty, and its README says what to
put in it and why: a fuzzer that finds a bug once and forgets it finds the
same bug again after the next refactor.

ASAN was not run. MSVC's `/fsanitize=address` would work on these targets and
is the obvious next step; it is named here rather than claimed.

## Performance

`docs/amberx/PERFORMANCE.md` has the numbers and the machine they came from.
The short version, three consecutive runs:

| | |
|---|---|
| host startup, launch to a display that answers | 31 ms |
| request round trip, 200 of them | 29-31 µs |
| drawing, 300 fills into a mapped window | 7,585-42,036 a second |
| memory across 500 windows and 500 pixmaps | no growth |

The preview asserts on these, so a change that makes a round trip take five
milliseconds or leaks 32 MiB fails the gate rather than being noticed later.

The targets that need an application — keystroke latency, 60 Hz
presentation, hidden-window savings — are argued from the shape of the code
in that document and marked as unmeasured. Low-bandwidth mode exists and
coalesces damage; it does not compress the X11 stream, and both the profile
page and the document say so.

## Packaging

`tools\package-amberx.ps1` builds the bundle:

```
licence gate: every import is a system library or an approved component
bundle: dist\AmberSSH (54 files)
archive: dist\AmberSSH-with-AmberX.zip
PACKAGE OK
```

Contents, against the prompt's list: **AmberXHost and its runtime files**
(two executables, libssh2, libcrypto, z, dxcompiler, fonts, shaders);
**third-party notices** (AmberSSH's and AmberX's, separately);
**SPDX SBOM** (`amberx.spdx.json`); **source provenance**
(`SOURCE-PROVENANCE.md`, plus the licence matrix and the rejected-components
list); **version and build information** (`VERSION.txt`, which says
explicitly when the tree was modified and the build is therefore not
reproducible); **the AmberX report** (`AMBERX-REPORT.txt`, from
`AmberSSH.exe --amberx-report`); **security and unsupported-feature
documentation** (`UNSUPPORTED.md`, `THREAT-MODEL.md`, `COMPATIBILITY.md`,
`PERFORMANCE.md`). Plus `MANIFEST-SHA256.txt`, a hash and size per file, so
a bundle can be checked after it has travelled.

The licence gate is the part that matters. It runs `dumpbin /dependents` over
both binaries and refuses to build a bundle whose imports are not either
Windows system libraries or components named in the matrix. It refused twice
during development — once for four Windows libraries the pattern did not yet
list — which is the only way to know a gate is wired to anything.

**Naming.** The bundle, the report and the documents state that AmberX is not
VcXsrv, Cygwin/X, Xming or X410, shares no code with them, and is not
endorsed by them. No file, string or icon carries their branding, and no
VcXsrv material was fetched, read or copied at any point in this work
(`REJECTED-COMPONENTS.md`).

## Limitations

- No fresh-machine install test, and no installer — the bundle is a folder
  and a zip.
- No application has been run against AmberX. This is the same open item as
  the Phase 4 live gate, and it is the largest one in the whole project.
- No ASAN or UBSAN run.
- The fuzz corpus is empty because nothing has been found yet.
- GPU device loss and monitor hot-plug are untested for lack of hardware.
- Uninstall is "delete the folder", which is true and is not the same as
  having been tried.

## How to re-run this gate

```
cmake --build build-amberx --config Release --target AmberSSH
build-amberx\Release\AmberSSH.exe --preview-amberx
build\tests\Release\AmberTests.exe
AMBERX_FUZZ_ITERS=400000 build\tests\Release\AmberTests.exe [fuzz]
powershell -ExecutionPolicy Bypass -File tools\package-amberx.ps1 -Zip
```

## Gate verdict

**Phase 8: pass on what can be measured here; two criteria open for want of
hardware.** The protocol tests, the fuzz targets, the performance numbers and
the packaging with its licence gate are done and they run. The clean-install
test needs a fresh Windows machine, and the compatibility suite needs a Linux
host with applications on it and a person to watch them. Both are written
down as forms to fill in rather than claimed, because a compatibility matrix
with no results is honest and a compatibility matrix with invented ones is
worse than nothing.
