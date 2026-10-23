#!/usr/bin/env bash
# Are the generated benchmark blocks of README.md and docs/BENCHMARKS.md what the generators make of docs/results, and how old are the results?
# No benchmark runs: the generators rewrite a scratch copy and the copy is compared (python only, about a second).
#
#   scripts/check-bench-fresh.sh            exit 1 when a block (or a chart) differs from what `bench/bench readme` / `bench store` generate;
#                                           "warning:" lines on stdout, exit 0, when the results are from another MAJOR.MINOR than pg_sift.control
#                                           or, for rows that carry no version, older than the newest release tag (MAX_AGE tags newer: default 0)
#   scripts/check-bench-fresh.sh --strict   the warnings fail too
# Fix: `bench/bench readme` and `bench/bench store render` when only the generator or the text changed; `bench/bench refresh` for stale results (docs/RELEASE.md).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
# the generators use f-strings that quote inside themselves (Python 3.12); the python3 of macOS is 3.9
python3 -c 'import sys; sys.exit(sys.version_info < (3, 12))' || { echo "check-bench-fresh: skipped, the generators need Python 3.12 or newer ($(python3 -V 2>&1))"; exit 0; }
STRICT='' ; [[ ${1:-} != --strict ]] || STRICT=1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
res=$R/docs/results bad=0 warn=0

cp "$R/README.md" "$R/docs/BENCHMARKS.md" "$T/"
python3 -B -I "$R/bench/steps/readme.py" --results "$res" --readme "$T/README.md" --benchmarks "$T/BENCHMARKS.md" --tier slim >/dev/null
python3 -B -I "$R/bench/steps/store.py" render --results "$res/store.json" --readme "$T/README.md" --benchmarks "$T/BENCHMARKS.md" --compression "$res/store-compression.json" --img "$T/img" >/dev/null
python3 -B -I "$R/bench/steps/charts.py" --results "$res" --out "$T/img" --tier slim >/dev/null
for f in README.md:README.md BENCHMARKS.md:docs/BENCHMARKS.md; do
	diff -q "$T/${f%%:*}" "$R/${f#*:}" >/dev/null || { echo "${f#*:}: a generated block differs from what docs/results generates:"; diff -u "$R/${f#*:}" "$T/${f%%:*}" | sed -n '3,14p' | cut -c1-200; bad=1; }
done
for f in "$T"/img/*; do cmp -s "$f" "$R/docs/img/${f##*/}" || { echo "docs/img/${f##*/} differs from what bench/steps/charts.py draws"; bad=1; }; done

# age of the results: the version each pass was measured at (the `version` word of the session row; store.json: the server's), else its date against the release tags
now=$(sed -n "s/^default_version *= *'\([0-9]*\.[0-9]*\).*/\1/p" "$R/pg_sift.control")
while IFS='|' read -r what ver date; do
	if [[ -n $ver ]]; then
		[[ $ver == "$now" ]] || { echo "warning: the $what numbers were measured at pg_sift $ver ($date), this tree is $now: bench/bench refresh"; warn=1; }
	else
		newer=$(git -C "$R" tag --list 'v*' --format='%(creatordate:short)' 2>/dev/null | awk -v d="$date" '$1 > d' | wc -l)
		(( newer <= ${MAX_AGE:-0} )) || { echo "warning: the $what numbers (measured $date, no version recorded) are older than $newer release tags: bench/bench refresh"; warn=1; }
	fi
done < <(python3 -B -I - "$res" <<'EOF'
import json, os, sys
res = sys.argv[1]
words = {}
for line in open(os.path.join(res, 'articles.tsv')):
    f = line.rstrip('\n').split('\t')
    if len(f) > 5 and f[1] == 'slim' and f[5] == 'session':
        words = dict(w.split('=', 1) for w in f[4].split(';'))
print('search|%s|%s' % (words.get('version', ''), words.get('date', '')))
meta = json.load(open(os.path.join(res, 'store.json')))['meta']['tiers']
t = next(reversed(meta.values()))
print('store|%s|%s' % (t['servers'].get('sift_sift', ''), t['parts']['warm']['date'][:10]))
EOF
)
(( bad == 0 )) || exit 1
[[ -z $STRICT || $warn == 0 ]] || exit 1
echo "bench-fresh: the generated blocks equal what docs/results generates"
