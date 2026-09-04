#!/usr/bin/env bash
# Re-creates third_party/amberx/upstream/ from the pinned commits recorded in
# docs/amberx/SOURCE-PROVENANCE.md, and refuses to proceed if a fetched tree
# does not match its recorded hash. The hash is the identity; the tag is
# only how to ask for it.
set -euo pipefail
cd "$(dirname "$0")/upstream" 2>/dev/null || { mkdir -p "$(dirname "$0")/upstream"; cd "$(dirname "$0")/upstream"; }

fetch() {
  local repo="$1" tag="$2" dir="$3" want="$4"
  if [ ! -d "$dir/.git" ]; then
    git clone -q --depth 1 --branch "$tag" "https://gitlab.freedesktop.org/$repo.git" "$dir"
  fi
  local got
  got="$(git -C "$dir" rev-parse HEAD)"
  if [ "$got" != "$want" ]; then
    echo "!! $dir is at $got, expected $want — not the audited tree. Remove it and re-run." >&2
    exit 1
  fi
  echo "$dir  $got  ok"
}

fetch xorg/xserver          xorg-server-21.1.24 xserver   65d790bd208ec380b196eb98f144abb0b32e334d
fetch xorg/proto/xorgproto  xorgproto-2025.1    xorgproto c18d2bc22813793bba7f0e4e603c0104d7724802
fetch pixman/pixman         pixman-0.46.4       pixman    9cc163c9da0fb4da430641715313d95a6ec466d9
fetch xorg/lib/libxfont     libXfont2-2.0.9     libxfont  975cc6526e892c6fad8fa49d0fef58565fbaa003
fetch xorg/lib/libxkbfile   libxkbfile-1.1.3    libxkbfile 39a5f8e67615f443e76146769d5f5f9abc5ebd2f
