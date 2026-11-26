#!/usr/bin/env bash
# Are the generated benchmark blocks and charts of README.md and docs/ what the renderers make of docs/results, how old are the results, and do the cases set only live settings?
# No benchmark runs: scripts/gen-docs.py --check renders into a scratch copy and compares it (python only, about a second).
#
#   scripts/check-bench-fresh.sh            exit 1 when a block or a chart differs from what scripts/gen-docs.py generates, or when bench/cases.tsv, bench/cases/*.sql or
#                                           bench/steps/cases/*.py set a sift.* setting that the release library does not define (a retired one, or a sift.test_ one: the server refuses it,
#                                           and the lane would measure the sift plan again);
#                                           "warning:" lines on stdout, exit 0, when docs/results/v6-core.tsv or v6-scaling.tsv were measured at another MAJOR.MINOR than pg_sift.control
#   scripts/check-bench-fresh.sh --strict   the warnings fail too
#   scripts/check-bench-fresh.sh --selftest
# Fix: python3 scripts/gen-docs.py when only the generators or the text changed; bench/bench v6-release --docs for stale results; a setting of a case that is gone: delete the variant, or
# set the live setting that has its effect.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
STRICT='' warn=0

# the sift.* settings that FILE... set (`sift.x = v`, `sift.x TO v`, set_config('sift.x', ...)) and the sources do not define outside the sift.test_ ones, as FILE: NAME
dead_settings() {
	local live
	live=$(cd "$R" && git grep -ohE 'Define[A-Za-z]*Variable\("sift\.[a-z0-9_]+"' -- 'src/*.c' | sed 's/.*("//; s/"$//' | grep -v '^sift\.test_' | sort -u)
	grep -oHE "sift\.[a-z0-9_]+[[:space:]]*(=|[Tt][Oo][[:space:]])|set_config\('sift\.[a-z0-9_]+'" "$@" | sed -E "s/^([^:]*):.*(sift\.[a-z0-9_]+).*/\1: \2/" | sort -u |
		while read -r f name; do grep -qxF "$name" <<<"$live" || echo "$f $name"; done
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d)
	trap 'rm -rf "$d"' EXIT
	printf 'x\ty\tscan:sift.agg_preagg = off\n' >"$d/cases.tsv"
	printf -- '-- set: sift.enable_agg = on; work_mem = 1\nSELECT set_config('"'"'sift.test_create_format1'"'"', '"'"'on'"'"', false), sift.explain_why($$q$$)\n' >"$d/case.sql"
	[[ $(dead_settings "$d/cases.tsv" "$d/case.sql") == "$d/case.sql: sift.test_create_format1"$'\n'"$d/cases.tsv: sift.agg_preagg" ]] ||
		{ echo "bench-fresh selftest: a retired or a testing setting was not reported, or a live one or a function was"; exit 1; }
	echo "bench-fresh selftest: ok"
	exit 0
fi
[[ ${1:-} != --strict ]] || STRICT=1

dead=$(dead_settings "$R/bench/cases.tsv" "$R"/bench/cases/*.sql "$R"/bench/steps/cases/*.py)
[[ -z $dead ]] || { echo "${dead//$R\//}"; echo "bench-fresh: the cases set a setting the release library does not define: the server refuses it and the lane measures nothing new (delete the variant, or set the live setting that has its effect)"; exit 1; }

python3 -B -I "$R/scripts/gen-docs.py" --check

now=$(sed -n "s/^default_version *= *'\([0-9]*\.[0-9]*\).*/\1/p" "$R/pg_sift.control")
for f in v6-core v6-scaling; do
	[[ -f $R/docs/results/$f.tsv ]] || continue
	ver=$(sed -n 's/^# version=\([0-9]*\.[0-9]*\).*/\1/p' "$R/docs/results/$f.tsv" | head -1)
	[[ $ver == "$now" ]] || { echo "warning: docs/results/$f.tsv was measured at pg_sift ${ver:-an unknown version}, this tree is $now: bench/bench v6-release --docs"; warn=1; }
done
[[ -z $STRICT || $warn == 0 ]] || exit 1
echo "bench-fresh: the generated blocks equal what docs/results generates; the cases set only live settings"
