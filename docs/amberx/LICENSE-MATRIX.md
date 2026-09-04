# AmberX — licence matrix

The allowlist gate for everything AmberXCore compiles or ships. One row per
component or retained subtree, recorded against the pinned commits in
`SOURCE-PROVENANCE.md`. A subtree not listed here is **not approved** — the
list is an allowlist, never a denylist.

**Reviewer: Claude (engineering audit), 2026-09-04. This is an engineering
determination of what the licence headers say, not legal advice. Every row
marked "approved" below is approved for the *build gate*; shipping requires
the legal review the prompt calls for, and that review has not happened.**

## Method

Every `.c` and `.h` in each candidate subtree of the pinned trees was
searched for licence language. Files carrying an MIT/X11-style grant were
counted; files mentioning the GPL, LGPL, AGPL, MPL or any other copyleft
were searched for across the *entire* xserver tree, not just the candidates;
files with no licence language at all were listed individually and read.

Results: **no copyleft mention exists anywhere in the pinned xserver tree
outside `hw/`**, and `hw/` is not a candidate.

## Components

| Component | Pinned | SPDX (tree-level) | Notice requirement | Modified | Status |
|---|---|---|---|---|---|
| xserver `dix/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved** |
| xserver `mi/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved** |
| xserver `fb/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved** |
| xserver `include/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved** |
| xserver `render/` | 21.1.24 | MIT (Keith Packard form) | preserve headers; attribute | no | **approved** |
| xserver `randr/` | 21.1.24 | MIT (Keith Packard form) | preserve headers; attribute | no | **approved** |
| xserver `xfixes/` | 21.1.24 | MIT | preserve headers; attribute | no | **approved** |
| xserver `damageext/` | 21.1.24 | MIT (Keith Packard form) | preserve headers; attribute | no | **approved** |
| xserver `composite/` | 21.1.24 | MIT | preserve headers; attribute | no | **approved** |
| xserver `xkb/` | 21.1.24 | MIT / X11 (Silicon Graphics form) | preserve headers; attribute | no | **approved** |
| xserver `Xi/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved** |
| xserver `Xext/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved — subset** (see below) |
| xserver `present/` | 21.1.24 | MIT (Keith Packard form) | preserve headers; attribute | no | **approved** |
| xserver `os/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **needs review** — platform layer; most files are replaced by AmberWinDDX, the retained subset is not yet chosen |
| xserver `miext/damage`, `miext/sync` | 21.1.24 | MIT | preserve headers; attribute | no | **approved** |
| **xorgproto** `include/` | 2025.1 | MIT variants, per-proto `COPYING-*` | preserve; attribute | no | **approved — subset**: every proto used must have its `COPYING-*` in the notice bundle |
| xorgproto `glxproto` | 2025.1 | SGI Free Software License B 2.0 | preserve | no | **rejected — out of scope** (GLX is not a first-milestone feature; see REJECTED-COMPONENTS.md) |
| **pixman** | 0.46.4 | MIT | preserve `COPYING`; attribute | no | **approved** |
| **libXfont2** (headers) | 2.0.9 | MIT (Red Hat / Oracle form) | preserve `COPYING`; attribute | no | **approved — headers**; compiling its sources is a Phase 2 decision |
| **libxkbfile** (headers) | 1.1.3 | MIT (Silicon Graphics form) | preserve `COPYING`; attribute | no | **approved — headers**; only `XKMformat.h` is referenced |
| xserver `miext/sync/misyncshm.c` | 21.1.24 | MIT | — | no | **rejected — scope**: the MIT-SHM fence path; MIT-SHM is off by policy |
| **MSVC / Windows SDK** | 14.44 | proprietary, redistributable runtime | none in source | — | approved by existing AmberSSH policy |

### `Xext/` subset

`Xext/` mixes core extensions with ones explicitly out of scope. Approved
for compilation: `bigreq.c`, `shape.c`, `sync.c`, `xcmisc.c`, `xtest.c`,
`security.c`, `hashtable.c`, `geext.c`. **Not approved**: `xvmain.c`,
`xvdisp.c`, `xvmc.c` (Xv), `panoramiX*.c` (Xinerama), `dpms.c`, `saver.c`,
`xres.c`, `shm.c` (MIT-SHM, deliberately not advertised), `xf86bigfont.c`.
Nothing in the not-approved set is a licence problem; they are excluded for
scope, and excluding them shrinks the surface the gate has to hold.

### Files with no per-file notice

Twenty-six files across the candidate subtrees carry no licence language
of their own. Each was opened. They fall into two kinds:

- **Trivial mechanical files**: `fb/wfbrename.h` (a list of `#define`
  renames), `xkb/ddxPrivate.c` (14 lines), `os/xstrans.c` (17 lines),
  `include/globals.h`, `Xext/hashtable.{c,h}`, `xkb/xkb.h`, `Xi/xibarriers.h`
  and similar private headers.
- **Files whose notice my search missed**: `include/privates.h` and
  `include/registry.h` open with the X11 "AS IS" disclaimer block and carry a
  copyright line further down.

**Determination:** files in the xserver tree without a per-file grant are
distributed by X.Org under the tree-level `COPYING` (MIT), which is how the
project itself treats them and how every distribution packages them. They are
recorded here so that determination is visible rather than implicit.
**This is precisely the kind of finding the legal review should confirm.**

## Probe coverage

Every row marked **approved** above was compiled by `run-probe.sh` under
MSVC on 2026-09-04 — 228 files, 0 failures — with no upstream file modified
(PHASE-1-GATE.md). Approval here therefore means two things at once: the
licence is on the allowlist, *and* the file has been shown to compile with
the approved toolchain. Rows marked "headers" or "needs review" were not
compiled.

## What the gate does not yet do

The prompt requires the *build* to fail on an unapproved file. That
enforcement (`cmake/AmberXLicenseGate.cmake`, the SPDX SBOM) is **not
written**: there is no AmberXCore build target yet for it to gate. This
document is the allowlist it will read from. Creating the CMake gate before
there is a build to attach it to would be another empty gate.
