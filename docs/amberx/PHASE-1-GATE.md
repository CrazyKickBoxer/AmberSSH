# AmberX — Phase 1 gate

The prompt's Phase 1 gate has four parts. This report says which passed and
exactly what each pass proves — because the numbers are easy to over-read.

```
Reproducible clean build ........... PASS  cmake -DAMBERX_CORE=ON builds AmberXCore.lib
                                           229 files, 0 errors, 499 warnings at /W3
SBOM generated ..................... PASS  third_party/amberx/amberx.spdx.json
Notice bundle generated ............ PASS  third_party/amberx/THIRD-PARTY-NOTICES.md
Licence gate passes ................ PASS  configure-time; proven in both directions
```

**Phase 1 gate: pass — as an engineering gate.** The legal review the
prompt calls for on LICENSE-MATRIX.md has not happened and is not something
this pass can supply. Everything below is what the build proves and what it
does not.

## 1. The build

```bash
bash third_party/amberx/fetch-upstream.sh              # 5 pinned trees; refuses a wrong hash
cmake -S . -B build-amberx -DAMBERX_CORE=ON             # gates run here and FAIL the configure
cmake --build build-amberx --config Release --target AmberXCore
```

Result on 2026-09-04, MSVC 14.44, `/std:c11 /TC /W3 /utf-8`, Release (`/O2`):

```
AmberXCore.lib   229 sources   0 errors   499 warnings   4,170,664 bytes
  dix 34   mi 29   fb 27   Xi 53   xkb 25   randr 16   render 11   present 10
  Xext (subset) 9   xfixes 6   composite 5   miext/sync 2   miext/damage 1   damageext 1
```

No upstream file is modified. `git status --porcelain` in each pinned tree
is empty, and the configure step refuses to proceed if it is not.

## 2. How it got there — the layers, in order

Each run peeled one layer. The first seven were the bare-`cl` probe
(`run-probe.sh`); the last three were what the *build* found that the probe
had not.

| run | result | what stopped it | what fixed it |
|---|---|---|---|
| 1 | 1 / 90 | `include/misc.h` includes `<pthread.h>` and `<sys/param.h>` unconditionally; `dix/colormap.c` wants `<strings.h>`; `servermd.h` demands the real config's guard | 4 declaration-only shim headers; `_DIX_CONFIG_H_` |
| 2 | 1 / 90 | `os.h` uses `sigset_t` and `pid_t` in prototypes; `dixfontstr.h` needs libXfont2 | two typedefs; fetch + pin libXfont2 |
| 3 | 88 / 90 | `M_PI`; `dix/main.c` wants `<unistd.h>` | `_USE_MATH_DEFINES`; unistd shim |
| 4 | 217 / 229 | `<sys/time.h>`; `<sys/mman.h>` (SHM fences); `XKMformat.h` | time shim; **exclude** `misyncshm.c` (MIT-SHM is off by policy); fetch + pin libxkbfile |
| 5 | 220 / 228 | my `sys/time.h` shim included `<winsock2.h>` → `<windows.h>` → `BOOL` collides with X11's `BOOL` | shim defines `struct timeval` itself |
| 6 | 227 / 228 | `W_OK` in `xkb/ddxLoad.c` | four `access()` mode constants |
| 7 | **228 / 228** probe | (+ `Xext/xace.c` after the link probe → 229) | — |
| 8 | 18,175 errors | CMake's VS generator adds `/DWIN32 /D_WINDOWS`; `os.h:721` then takes the MinGW path `typedef _sigset_t sigset_t;` and every declaration after it is inside a broken prototype | strip both defines from the target: X.Org reads `WIN32` as "the Cygwin/MinGW port", and AmberX is not that |
| 9 | 1 error | `/O2` makes `cbrt` an intrinsic; `mi/miarc.c`'s fallback definition collides with `math.h`'s `dllimport` declaration | `HAVE_CBRT 1` — a real `dix-config.h.in` slot. **The probe missed this because it ran at `/Od`.** |
| 10 | 0 errors, 502 → 499 warnings | my `strings.h` shim redefined `strcasecmp` under `os.h`'s own macro (C4005 ×2); `ffs` implicitly declared (C4013) | shim macros removed (upstream's `xstrcasecmp` is the one definition); `int ffs(int)` declared in `dix-config.h`, where `mibitblt.c` can actually see it |

Runs 5 and 8 are the same lesson from two directions: **the X.Org core never
sees `<windows.h>` and never sees `WIN32`; the Windows os layer never sees an
X header.** They meet through a private interface with no shared types. That
rule is now enforced in `third_party/amberx/CMakeLists.txt` with a comment
that says why.

Run 9 is the reason the probe was never going to be the gate on its own: a
measurement at one optimisation level is not a build.

## 3. The 499 warnings

```
C4244  228   double/int/short narrowing on assignment
C4267  148   size_t → int/unsigned                   } 479: LP64 code on LLP64, style class
C4018  103   signed/unsigned comparison
C4311   11   pointer → unsigned long truncation      }
C4312    4   long → pointer widening                 }  20: the ones worth reading
C4715    3   not all control paths return a value    }
C4146    1   unary minus on unsigned
C4101    1   unreferenced local
```

The 20 are listed here because they are the ones that can be bugs on a
64-bit Windows build, where `long` is 32 bits and X.Org's `_XSERVER64`
assumptions were written for LP64:

| where | what | assessment |
|---|---|---|
| `fb/fbbits.h:596,716`, `fb/fbseg.c:356`, `fb/fbgetsp.c:50`, `fb/fbsetsp.c:48`, `mi/mizerline.c:120` | pointer cast to `unsigned long`, then masked for alignment | low bits survive truncation; the alignment test is still correct. Known-benign pattern in every LLP64 X build |
| `dix/resource.c:818` | pointer hashed through `unsigned long` | a hash; truncation only changes the distribution |
| `dix/events.c:4958`, `dix/pixmap.c:139`, `fb/fboverlay.c:223`, `mi/miscrinit.c:306` | integer → pointer via `long` | sentinel values / private-key round-trips. **Not verified individually.** |
| `Xext/sync.c:1269`, `Xi/exevents.c:2871`, `dix/events.c:4603` | `switch` with a return in every case and no default | benign as written; MSVC cannot prove it |
| `mi/mizerclip.c:593` | `-(unsigned)` | deliberate two's-complement arithmetic |

**These have been read, not proven.** Phase 2 runs the server and the
integer→pointer round-trips either work or fail on the first window. They
are on the Phase 2 list under "LLP64 review", and none of the 479 style
warnings is silenced: the count is the honest number and stays visible.

## 4. The tooling

- **Provenance gate** (`third_party/amberx/CMakeLists.txt`): five pins,
  duplicated from SOURCE-PROVENANCE.md on purpose — a document can be edited
  quietly; a configure failure cannot. Refuses a wrong HEAD and a dirty tree.
  Proven: an edited upstream file → `FATAL_ERROR`; reverted → passes.
- **Allowlist gate**: every source must be in an approved directory, not on
  the excluded list, and in the `Xext/` subset if it is in `Xext/`. Proven:
  adding `glx/glxext.c` → `FATAL_ERROR`; removed → 229 pass.
- **SBOM** `amberx.spdx.json`: SPDX 2.3, one package per pinned tree with
  commit hash and licence, generated by `make-notices.sh`.
- **Notice bundle** `THIRD-PARTY-NOTICES.md` (2,841 lines): each tree's
  `COPYING` plus every xorgproto `COPYING-*` the build actually reads.
- **Link probe** (`run-link-probe.sh`): archives the objects and links a stub
  `main` with `/WHOLEARCHIVE`. **194 unresolved symbols**: 71 pixman, 13
  libXfont2, **110 port surface**. That list, bucketed, *is* PHASE-2-SPEC.md.

## 5. What the shims are

`config-msvc/compat/` — 104 non-comment lines across six headers, plus three
declarations in `dix-config.h` (`sigset_t`, `pid_t`, `ffs`):

```
pthread.h      types + prototypes, NO definitions      → replaced by SRWLOCK/CONDITION_VARIABLE
poll.h         struct pollfd + prototype, NO definition → removed: AmberX has no listener
unistd.h       _-prefixed CRT mappings; getuid & co declared, NO definitions
sys/time.h     struct timeval; gettimeofday declared, NO definition
sys/param.h    MIN/MAX/MAXPATHLEN (stays: these are just macros)
strings.h      <string.h> only (stays; strcasecmp is upstream's, see run 10)
```

Declaration-only is deliberate. Anything that calls `pthread_mutex_lock`,
`poll`, `gettimeofday` or `ffs` fails **at link time** — the link probe lists
every one — so the build can never accidentally succeed by running POSIX on
Windows.

## 6. What this proves

- The approved subtrees are **compilable by MSVC** as C11, optimised, with
  warnings on, from a CMake target, with no upstream file modified.
- The POSIX surface of the core is **small and enumerated**: six shim
  headers, three declarations, and the linker's list of 110 names.
- The Cygwin/X exclusion (prompt rule) **cost nothing**: none of `hw/xwin`
  was needed, and `WIN32` — the switch that would have led there — is
  actively kept off.
- `os/` is exactly what Phase 0 said it was: the layer the port replaces.
  Nothing in the core needed it to compile; 35 of its symbols are pure C
  worth retaining (PHASE-2-SPEC §A), the rest is the port (§B).

## 7. What this does not prove

- **Running.** Nothing has executed. Not one X request has been dispatched.
- **Linking to completion.** 194 unresolved by design; zero is Phase 2's
  first milestone.
- **Warnings fixed.** 499 counted, 20 read, 0 fixed. Fixing narrowing
  warnings in upstream files would mean patching upstream, which is a
  patch-series decision, not a gate decision.
- **`dix/main.c`** compiles and is replaced: AmberXHost has its own entry
  point. **`xkb/ddxLoad.c`** compiles and must not ship as-is: it spawns
  `xkbcomp`, and the host cannot spawn processes (Job Object limit) — see
  REJECTED-COMPONENTS.md.
- **Toolchain breadth.** MSVC 14.44 only; clang-cl not tried. Release only;
  Debug not built.
- **A fresh machine.** Reproducibility was checked by `--clean-first` here,
  not on a second Windows 11 install.
- **Legal review.** LICENSE-MATRIX.md is an engineering reading of licence
  headers. It is the input to a review, not the review.

## Gate verdict

**Phase 1: pass.** All four checklist items are done and each is enforced
by something that fails rather than warns. Proceed to Phase 2 with the
linker's list as the specification.
