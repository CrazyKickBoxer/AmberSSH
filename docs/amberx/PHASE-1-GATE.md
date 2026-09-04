# AmberX — Phase 1 gate

The prompt's Phase 1 gate has four parts. This report says which passed,
which did not, and exactly what the passing part proves — because the
number at the top is easy to over-read.

```
Reproducible clean build ........... compile gate PASSES; no link, no artifact
SBOM generated ..................... NOT DONE
Notice bundle generated ............ NOT DONE
Licence gate passes ................ allowlist exists (LICENSE-MATRIX.md);
                                     the CMake enforcement is NOT WRITTEN
```

**Phase 1 is not complete.** Its decisive question — Finding P0-3, "will
X.Org's device-independent core compile under an approved Windows toolchain
at all?" — is answered, and answered yes. Its tooling half is not built.

## The measurement

`third_party/amberx/run-probe.sh` compiles every `.c` on the allowlist, one
at a time, with `cl.exe` (MSVC 14.44, `/std:c11`, `/TC`), against the pinned
upstream headers and `third_party/amberx/config-msvc/`. Nothing is linked.

Final run:

```
allowlisted core + extensions under MSVC: 228 compile, 0 do not (of 228)
  dix 34   mi 29   fb 27   Xi 53   xkb 25   randr 16   render 11
  present 10   Xext (subset) 8   xfixes 6   composite 5
  miext/sync 2   miext/damage 1   damageext 1
```

Reproducible from a clean checkout:

```bash
bash third_party/amberx/fetch-upstream.sh    # pins 5 trees, refuses a wrong hash
bash third_party/amberx/run-probe.sh         # ~2 minutes; prints the table above
```

## How it got there — the layers, in order

Each run peeled one layer, and the layers are the honest shape of the
porting job:

| run | result | what stopped it | what fixed it |
|---|---|---|---|
| 1 | 1 / 90 | `include/misc.h` includes `<pthread.h>` and `<sys/param.h>` unconditionally; `dix/colormap.c` wants `<strings.h>`; `servermd.h` demands the real config's guard macro | 4 declaration-only shim headers; `_DIX_CONFIG_H_` |
| 2 | 1 / 90 | `os.h` uses `sigset_t` and `pid_t` in prototypes; `dixfontstr.h` needs libXfont2 | two typedefs; fetch + pin libXfont2 |
| 3 | 88 / 90 | `M_PI`; `dix/main.c` wants `<unistd.h>` | `_USE_MATH_DEFINES`; unistd shim |
| 4 | 217 / 229 | `<sys/time.h>`; `<sys/mman.h>` (SHM fences); `XKMformat.h` from libxkbfile | time shim; **exclude** `misyncshm.c` (MIT-SHM is off by policy); fetch + pin libxkbfile |
| 5 | 220 / 228 | my `sys/time.h` shim included `<winsock2.h>` → `<windows.h>` → `BOOL` collides with X11's `BOOL` | shim defines `struct timeval` itself |
| 6 | 227 / 228 | `W_OK` in `xkb/ddxLoad.c` | four `access()` mode constants |
| 7 | **228 / 228** | — | — |

Run 5 is worth remembering: it was the oldest problem in porting X to Windows
— X headers and Windows headers cannot be visible to the same translation
unit — and it showed up in a five-line shim. The port's rule follows from it:
**the X.Org core never sees `<windows.h>`; the Windows os layer never sees an
X header.** They meet through a private interface with no shared types.

## What the shims are

`config-msvc/compat/` — 106 non-comment lines across six headers:

```
pthread.h      types + prototypes, NO definitions     → replaced by SRWLOCK/CONDITION_VARIABLE
poll.h         struct pollfd + prototype, NO definition → removed: AmberX has no listener
unistd.h       _-prefixed CRT mappings; getuid & co declared, NO definitions
sys/time.h     struct timeval; gettimeofday declared, NO definition
sys/param.h    MIN/MAX/MAXPATHLEN (stays: these are just macros)
strings.h      strcasecmp → _stricmp (stays: this is the standard mapping)
```

Declaration-only is deliberate. Anything that calls `pthread_mutex_lock` or
`poll` fails **at link time**, so the probe can never accidentally succeed by
running POSIX on Windows. Two of the six (`strings.h`, `sys/param.h`) are real
and can stay; the other four are the Phase 2 work, and the fact that they are
four small headers rather than four subsystems is the finding.

`config-msvc/dix-config.h` is a hand-written replacement for the meson output:
every macro in it is one `include/dix-config.h.in` can define; extensions in
scope are on, everything else — GLX, DRI, Xv, Xinerama, XDMCP, listeners,
input thread — is off.

## What this proves

- The approved subtrees are **compilable by MSVC** as C11 with no GCC
  extensions in the way. (`__attribute__`, `typeof`: zero uses in the core;
  one `__builtin_ffs` behind a macro.)
- The POSIX surface of the core is **small and enumerated**: it is the six
  shim headers plus two typedefs, and nothing else was needed.
- The Cygwin/X exclusion (prompt rule) **cost nothing**: none of `hw/xwin`
  was needed to reach this result, so the "permissive port work is in the
  barred directory" worry from Phase 0 did not bite at this stage.
- `os/` is exactly what Phase 0 said it was: the layer that talks to the OS,
  and the layer the port replaces. It was excluded and nothing in the core
  needed it *to compile*.

## What this does not prove

- **Linking.** Every `os_*`, `pthread_*`, `poll`, `gettimeofday`, `getuid`
  symbol is unresolved. A link attempt would produce hundreds of errors, and
  that list is the specification for the Windows os layer. It has not been
  produced yet.
- **Running.** Nothing has executed. Not one X request has been dispatched.
- **Warnings.** `/w` suppressed every warning to keep the measurement about
  errors. 64-bit truncation, sign conversion and format mismatches are
  unmeasured. The next probe should run at `/W3` and count.
- **`dix/main.c`.** It compiles, and it is replaced: AmberXHost has its own
  entry point and initialisation order.
- **`xkb/ddxLoad.c`.** It compiles, and it must not ship as-is: it spawns
  `xkbcomp`, and the host cannot spawn processes (Job Object limit). Keymap
  delivery needs its own design — see REJECTED-COMPONENTS.md.
- **Toolchain breadth.** MSVC 14.44 only. clang-cl was not tried.
- **A fresh machine.** Reproducibility was checked by re-running here, not on
  a second Windows 11 install.

## What Phase 1 still needs

1. **`cmake/AmberXLicenseGate.cmake`** — read the allowlist from
   `LICENSE-MATRIX.md`, refuse to compile any file not on it, refuse a tree
   whose hash differs from `SOURCE-PROVENANCE.md`.
2. **SPDX SBOM** (`third_party/amberx/amberx.spdx.json`) generated at build
   time from the same two documents.
3. **Notice bundle** (`third_party/amberx/THIRD-PARTY-NOTICES.md`) assembled
   from each pinned tree's `COPYING` plus the per-proto `COPYING-*` files
   actually used.
4. A CMake target that builds the 228 files into a static library — which is
   where the link errors appear and Phase 2's work list is written by the
   linker.

None of those is hard, and none was done in this pass: the pass was spent
finding out whether there was anything to gate. There is.

## Gate verdict

**Compile gate: pass.** **Phase 1: incomplete** — three of four checklist
items remain. Proceeding to Phase 2 is justified on the technical finding;
the tooling debt is recorded here and must be paid before anything ships.
