#!/usr/bin/env bash
# Generates the one header meson would have produced for pixman, runs the
# MSVC compile probe over dix/mi/fb, and summarises: counts, then the FIRST
# error in every failing file grouped by message, so the porting work is
# visible as a ranked list rather than a wall of logs.
set -uo pipefail
cd "$(dirname "$0")"
up=upstream
mkdir -p config-msvc/pixman probe-out

# pixman-version.h from its template, with the pinned 0.46.4 numbers.
sed -e 's/@PIXMAN_VERSION_MAJOR@/0/' -e 's/@PIXMAN_VERSION_MINOR@/46/' \
    -e 's/@PIXMAN_VERSION_MICRO@/4/' "$up/pixman/pixman/pixman-version.h.in" \
    > config-msvc/pixman/pixman-version.h

rm -f probe-out/*.log
cmd //c "$(cygpath -w "$PWD/probe-core.cmd")" > probe-out/results.txt 2>&1
pass=$(grep -c '^PASS' probe-out/results.txt); fail=$(grep -c '^FAIL' probe-out/results.txt)
echo "=== allowlisted core + extensions under MSVC: $pass compile, $fail do not (of $((pass+fail))) ==="
# Per-directory counts, derived from the result lines themselves so a new
# directory in the probe shows up here without editing this script.
grep -E '^(PASS|FAIL) ' probe-out/results.txt | awk '{
  split($2, p, "\\\\"); d = p[1]; for (i = 2; i < length(p); i++) d = d "\\" p[i];
  if ($1 == "PASS") ok[d]++; else bad[d]++; seen[d] = 1
} END { for (d in seen) printf "  %-14s pass=%-3d fail=%d\n", d, ok[d]+0, bad[d]+0 }' | sort
echo
echo "=== first error per failing file, grouped (count  message) ==="
for f in probe-out/*.log; do
  grep -m1 -E 'error C[0-9]+|fatal error' "$f" | sed -E 's/^.*(error C[0-9]+|fatal error C[0-9]+): //'
done | sed -E "s/'[^']*'/'…'/g" | sort | uniq -c | sort -rn | head -14
echo
echo "=== first error per failing file, verbatim (first 12) ==="
for f in probe-out/*.log; do
  m=$(grep -m1 -E 'error C[0-9]+|fatal error' "$f") && printf '%s: %s\n' "$(basename "$f" .log)" "${m##*): }"
done | head -12
