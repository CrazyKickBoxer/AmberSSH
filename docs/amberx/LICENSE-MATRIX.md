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
| xserver `os/` | 21.1.24 | MIT / X11 | preserve headers; attribute | no | **approved — subset**: `mitauth.c oscolor.c xprintf.c strlcpy.c strlcat.c strndup.c reallocarray.c timingsafe_memcmp.c` (pure C, compiled unmodified into AmberXServer). Everything else in `os/` is replaced by `src/amberx/server/` — see REJECTED-COMPONENTS.md |
| xserver `miext/damage`, `miext/sync` | 21.1.24 | MIT | preserve headers; attribute | no | **approved** |
| **xorgproto** `include/` | 2025.1 | MIT variants, per-proto `COPYING-*` | preserve; attribute | no | **approved — subset**: every proto used must have its `COPYING-*` in the notice bundle |
| xorgproto `glxproto` | 2025.1 | SGI Free Software License B 2.0 | preserve | no | **rejected — out of scope** (GLX is not a first-milestone feature; see REJECTED-COMPONENTS.md) |
| **pixman** | 0.46.4 | MIT | preserve `COPYING`; attribute | no | **approved — compiled**: the portable C paths (28 files) as `AmberXPixman`; the SIMD files are not compiled (see REJECTED-COMPONENTS.md) |
| **libXfont2** | 2.0.9 | MIT (Red Hat / Oracle form) | preserve `COPYING`; attribute | no | **approved — subset**: `fontfile/` (less `catalogue.c`, `bunzip2.c`), `bitmap/`, `builtins/`, `util/`, `stubs/` as `AmberXFont`; no FreeType, no font-server client |
| **libxkbfile** (headers) | 1.1.3 | MIT (Silicon Graphics form) | preserve `COPYING`; attribute | no | **approved — headers**; only `XKMformat.h` is referenced |
| **zlib** | 1.3.2 (vcpkg) | zlib | preserve licence text | no | **approved — required**: libXfont2's built-in `fixed` and `cursor` fonts are stored gzip-compressed inside the library, so `fontfile/gunzip.c` and zlib are in the runtime closure. Same zlib AmberSSH already ships for libssh2 |
| xserver `miext/sync/misyncshm.c` | 21.1.24 | MIT | — | no | **rejected — scope**: the MIT-SHM fence path; MIT-SHM is off by policy |
| xserver `xkb/ddxLoad.c` | 21.1.24 | MIT | — | no | **rejected — policy**: spawns `xkbcomp`; the host may not spawn. Replaced by `src/amberx/server/ddx_keymap.c` |
| **MSVC / Windows SDK** | 14.44 | proprietary, redistributable runtime | none in source | — | approved by existing AmberSSH policy |

### `Xext/` subset

`Xext/` mixes core extensions with ones explicitly out of scope. Approved
for compilation: `bigreq.c`, `shape.c`, `sync.c`, `xcmisc.c`, `xtest.c`,
`security.c`, `hashtable.c`, `geext.c`, and `xace.c` — the last added after
the link probe: seven `Xace*` symbols were unresolved, and XACE is the hook
mechanism the SECURITY extension enforces restricted mode through, so it is
not optional for Phase 5. **Not approved**: `xvmain.c`,
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

## Build coverage

Every row marked **approved** above is compiled by a CMake target under
MSVC — `AmberXCore` (228 files), `AmberXServer` (the `os/` subset),
`AmberXPixman` and `AmberXFont` — 0 errors at `/W3`, Release, with no
upstream file modified (PHASE-1-GATE.md, PHASE-2-GATE.md). Approval here
therefore means two things at once: the licence is on the allowlist, *and*
the file has been shown to compile with the approved toolchain and to link
and run in AmberXHost. Rows marked "headers" are not compiled.

## How the gate is enforced

`third_party/amberx/CMakeLists.txt` re-states this allowlist as data —
approved directories, the `Xext/` subset, the excluded files — and refuses to
*configure* if any source in `AmberXCore-sources.cmake` falls outside it. It
also refuses a pinned tree whose HEAD is not the audited hash or whose
working tree is dirty. Both refusals have been exercised (PHASE-1-GATE §4).
The list here and the list in CMake must agree; when this document changes,
the CMake list is the one that actually stops the build, so change it too.
The SBOM (`amberx.spdx.json`) and notice bundle (`THIRD-PARTY-NOTICES.md`)
are generated by `make-notices.sh` from the same pins.
