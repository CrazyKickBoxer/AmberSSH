# AmberX — source provenance

Every third-party tree AmberX builds from, pinned to an exact upstream commit.
Nothing here is vendored into git: `third_party/amberx/upstream/` is
`.gitignore`d and is re-created from this record by the fetch script below.
A tree whose `HEAD` does not match the hash recorded here is not the tree
that was audited and must not be built.

Fetched 2026-09-04, on the user's instruction to proceed through all
phases, which is taken as the authorisation the Phase 0 audit asked for.

| Component | Canonical upstream | Tag | Commit | Size (shallow) |
|---|---|---|---|---|
| **xserver** | `https://gitlab.freedesktop.org/xorg/xserver.git` | `xorg-server-21.1.24` | `65d790bd208ec380b196eb98f144abb0b32e334d` | 31 MB |
| **xorgproto** | `https://gitlab.freedesktop.org/xorg/proto/xorgproto.git` | `xorgproto-2025.1` | `c18d2bc22813793bba7f0e4e603c0104d7724802` | 9 MB |
| **pixman** | `https://gitlab.freedesktop.org/pixman/pixman.git` | `pixman-0.46.4` | `9cc163c9da0fb4da430641715313d95a6ec466d9` | 5 MB |
| **libXfont2** | `https://gitlab.freedesktop.org/xorg/lib/libxfont.git` | `libXfont2-2.0.9` | `975cc6526e892c6fad8fa49d0fef58565fbaa003` | 2 MB |

| **libxkbfile** | `https://gitlab.freedesktop.org/xorg/lib/libxkbfile.git` | `libxkbfile-1.1.3` | `39a5f8e67615f443e76146769d5f5f9abc5ebd2f` | 1 MB |

libXfont2 and libxkbfile were added after probe runs, not guessed at in
advance: `dix/dixfonts.c` includes `X11/fonts/libxfont2.h`, and three
`xkb/` files include `X11/extensions/XKMformat.h`, which ships with
libxkbfile rather than xorgproto. Both are MIT (libXfont2 in the Red Hat /
Oracle form, libxkbfile in the Silicon Graphics form). Only their **headers**
are used by the probe; whether their sources are compiled into AmberXCore is
a Phase 2 decision recorded in LICENSE-MATRIX.md.

`xorg-server-21.1.24` is the newest tag on the 21.1 stable series at fetch
time (the `ls-remote` that chose it is reproducible: it listed 21.1.20 through
21.1.24). The 21.1 series is the maintained release line; the `master` branch
was not used because it has no release discipline to pin to.

## Reproducing the import

```bash
cd third_party/amberx/upstream
git clone --depth 1 --branch xorg-server-21.1.24 https://gitlab.freedesktop.org/xorg/xserver.git xserver
git clone --depth 1 --branch xorgproto-2025.1   https://gitlab.freedesktop.org/xorg/proto/xorgproto.git xorgproto
git clone --depth 1 --branch pixman-0.46.4      https://gitlab.freedesktop.org/pixman/pixman.git pixman
for t in xserver xorgproto pixman; do echo "$t $(git -C $t rev-parse HEAD)"; done
```

The printed hashes must equal the table above. A tag can be moved; a commit
hash cannot, which is why both are recorded and the hash is the one that
counts.

## What is and is not used

Fetched ≠ compiled. The subtrees actually intended for AmberXCore, and their
audit status, are in `LICENSE-MATRIX.md`. Everything else in the clones —
`hw/` in its entirety, `glx/`, `glamor/`, `dri3/`, `record/`, the
platform-specific parts of `os/` — is present because a shallow clone brings
the whole tree, and is **not built and not audited**. `REJECTED-COMPONENTS.md`
lists what was looked at and turned away.

## Local modifications

**None.** No file under `third_party/amberx/upstream/` has been edited. When
the build gate requires patches, they go in `third_party/amberx/patches/` as
an explicit series applied at build time, so the upstream tree stays byte-
identical to the hash above and the diff from upstream is always visible.

## Provenance of what is NOT here

No VcXsrv source, patch, build file, resource or binary has been fetched,
inspected, or copied, at any point in this branch's history. The clones above
are the only third-party material added, and each came from the project's own
canonical repository.
