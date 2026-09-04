#!/usr/bin/env bash
# Runs the link probe and turns the linker's unresolved-external list into a
# ranked work list: how many distinct symbols, grouped by where they come
# from, then every name. This is the Phase 2 specification, written by the
# linker rather than guessed.
set -uo pipefail
cd "$(dirname "$0")"
objs=$(ls probe-out/*.obj 2>/dev/null | wc -l)
[ "$objs" -gt 0 ] || { echo "no .obj files — run run-probe.sh first"; exit 1; }
cmd //c "$(cygpath -w "$PWD/link-probe.cmd")" >/dev/null 2>&1
log=probe-out/link.log
echo "=== objects archived: $objs   $(grep -o 'LINK_EXIT [0-9]*' "$log")"
# One line per unresolved symbol; strip MSVC decoration and dedupe.
grep -oE 'unresolved external symbol [^ ]+' "$log" | awk '{print $4}' | sed -E 's/^_?(__imp_)?//' \
  | sort -u > probe-out/unresolved.txt
total=$(wc -l < probe-out/unresolved.txt)
echo "=== distinct unresolved symbols: $total ==="
echo
echo "=== grouped — what each group is ==="
g() { c=$(grep -cE "$2" probe-out/unresolved.txt); printf '  %-34s %4d\n' "$1" "$c"; }
# Libraries to build, not code to port:
g "pixman_*            (build pixman)"          '^pixman_'
g "libXfont2           (build libXfont2)"       '^(xfont2_|Xfont2|Font|font)'
# The port — the Windows os layer and the DDX, by responsibility. Patterns
# are by name, so a symbol can land in more than one bucket; the total is
# the honest number and the buckets are a map.
g "connections & auth  (Client*/Auth*/Host*)"   '(Client|Connection|Socket|Auth|Host|Listen|AccessControl|Audit|Creds)'
g "logging             (Log*/Error*/Fatal*)"    '^(Log|Error|Fatal|Verify|GiveUp|Abort|xorg_backtrace)'
g "time & timers       (GetTime*/Timer*)"       '(GetTime|Timer|Sleep|WaitFor|AdjustWait)'
g "memory & strings    (X*alloc/Xstrdup/strl*)" '^(X[a-z]*alloc|Xfree|Xstrdup|Xstrndup|strl|strndup|reallocarray|timingsafe|xstr|Xprintf|Xvprintf|XNFprintf|XNFvprintf|Xasprintf|XNFasprintf|Xvasprintf|XNFvasprintf)'
g "DDX hooks           (Init*/Close*/ddx*/DDX*)" '^(InitOutput|InitInput|CloseInput|ddx|DDX|OsVendor|ProcessInputEvents|InputThread|NotifyParent|xf86)'
g "extension flags     (no*Extension)"          '^no[A-Z].*Extension|^noTestExtensions'
g "POSIX shims         (pthread/poll/time)"     '^(pthread_|poll$|gettimeofday|getuid|geteuid|getgid|getegid|setuid|setgid|sleep|usleep|sysconf|ffs)'
nonlib=$(grep -vcE '^(pixman_|xfont2_|Xfont2|Font|font)' probe-out/unresolved.txt)
printf '  %-34s %4d   <- the actual porting surface\n' "TOTAL not-a-library" "$nonlib"
echo
echo "=== all unresolved, alphabetical ==="; cat probe-out/unresolved.txt | column -c 120 2>/dev/null || cat probe-out/unresolved.txt
