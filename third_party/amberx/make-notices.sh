#!/usr/bin/env bash
# Regenerates THIRD-PARTY-NOTICES.md from the pinned upstream trees' own
# licence files. The output is the notice bundle the prompt requires shipped
# with AmberX; it is generated, never hand-edited, so it cannot drift from
# the trees it describes.
set -euo pipefail
cd "$(dirname "$0")"
u=upstream; out=THIRD-PARTY-NOTICES.md
for t in xserver pixman libxfont libxkbfile xorgproto; do
  [ -d "$u/$t" ] || { echo "missing $u/$t — run fetch-upstream.sh" >&2; exit 1; }
done
{
  echo "# AmberX — third-party notices"
  echo
  echo "Licence texts for every upstream tree AmberX is built from, reproduced in"
  echo "full as each licence requires. Pinned versions are in"
  echo '`docs/amberx/SOURCE-PROVENANCE.md`; which subtrees are compiled is in'
  echo '`docs/amberx/LICENSE-MATRIX.md`. Generated from the trees themselves by'
  echo '`third_party/amberx/make-notices.sh` — do not edit by hand.'
  echo
  for spec in "X.Org Server 21.1.24|$u/xserver/COPYING" \
              "pixman 0.46.4|$u/pixman/COPYING" \
              "libXfont2 2.0.9|$u/libxfont/COPYING" \
              "libxkbfile 1.1.3|$u/libxkbfile/COPYING"; do
    IFS='|' read -r title file <<<"$spec"
    echo "---"; echo; echo "## $title"; echo; echo '```'; cat "$file"; echo '```'; echo
  done
  echo "---"; echo; echo "## xorgproto 2025.1 — per-protocol licences"; echo
  echo "xorgproto carries one licence file per protocol. The ones for protocols in"
  echo "AmberX's scope are reproduced; GLX (SGI Free Software License B) is excluded"
  echo "with its protocol."; echo
  for p in xproto xextproto bigreqsproto xcmiscproto renderproto randrproto fixesproto \
           damageproto compositeproto presentproto inputproto kbproto xf86bigfontproto; do
    f="$u/xorgproto/COPYING-$p"; [ -f "$f" ] || continue
    echo "### $p"; echo; echo '```'; cat "$f"; echo '```'; echo
  done
} > "$out"
echo "$out: $(wc -l < "$out") lines, $(grep -c '^## \|^### ' "$out") sections"
