#!/usr/bin/env bash
# Every driver that starts a server runs the leak check (scripts/leakcheck.sh: leak_before and leak_after around a unit of work, leak_end around a cluster) without the
# test opting in: a driver that does not source scripts/leakcheck.sh, test/t/lib.bash or run scripts/pgtest.sh, which carry the hooks, fails here. A driver that
# starts a server and does not test sift (a benchmark cluster, an older release's library, a stub) is a row in scripts/leak-hooks.allow: FILE<TAB>reason. The
# fix for a failing driver is to bracket its work with the hooks, not to add a row.
#   scripts/check-leak-hooks.sh [--selftest]
cd "$(git rev-parse --show-toplevel)" || exit 1
allow=scripts/leak-hooks.allow
DRIVER='^[^#]*(pg_ctl[^#|]* start|(^|[^_a-z-])initdb[ "]|pg_regress |pg_isolation_regress|Cluster->new|get_new_node)'
HOOKED='^[^#]*(leak_(before|after|end)|leakcheck\.sh|lib\.bash|SiftTest|pgtest\.sh (start|stop|regress|tap|fresh))'
check() { # ROOT: prints a line for each driver without the hooks and each row that is wrong
	local root=$1 f
	for f in $(git -C "$root" grep -lE "$DRIVER" -- scripts test bench tools ':!*.sql' ':!*.out' ':!*.md' ':!*.tsv' ':!*.allow' ':!scripts/check-leak-hooks.sh'); do
		grep -qE "$HOOKED" "$root/$f" && continue
		awk -F'\t' -v f="$f" '$1 == f && $2 != "" { found = 1 } END { exit !found }' "$root/$allow" ||
			echo "$f: starts a server and runs no leak check: bracket its work with leak_before/leak_after (scripts/leakcheck.sh) or end its cluster with leak_end; a driver that does not test sift goes in $allow with the reason"
	done
	awk -F'\t' '!/^#/ && NF { print $1 "\t" $2 }' "$root/$allow" | while IFS=$'\t' read -r f why; do
		[[ -n $why ]] || echo "$allow: $f has no reason"
		git -C "$root" grep -qE "$DRIVER" -- "$f" 2>/dev/null || echo "$allow: $f starts no server (or is gone): delete the row"
		! grep -qE "$HOOKED" "$root/$f" 2>/dev/null || echo "$allow: $f runs the leak check already: delete the row"
	done
}
if [[ ${1:-} == --selftest ]]; then
	t=$(mktemp -d "$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/check-leak-hooks.XXXXXX") || exit 1
	trap 'rm -rf "$t"' EXIT
	git -C "$t" init -q && mkdir -p "$t/scripts" "$t/test/t" "$t/bench"
	printf 'pg_ctl -D "$D" -w start\n' >"$t/test/t/bare.sh"
	printf '. scripts/leakcheck.sh\npg_ctl -D "$D" -w start\nleak_end "$D" "$T" pg_ctl stop\n' >"$t/test/t/hooked.sh"
	printf 'scripts/pgtest.sh start\ninitdb -D x\n' >"$t/test/t/viapgtest.sh"
	printf 'initdb -D x\n' >"$t/bench/measure.sh"
	printf 'initdb -D x\n' >"$t/bench/noreason.sh"
	printf 'pg_ctl -D x -w start\n' >"$t/bench/stale.sh"
	printf '# initdb in a comment\n' >"$t/bench/comment.sh"
	printf 'bench/measure.sh\tbenchmark cluster\nbench/noreason.sh\t\nbench/stale.sh\tbenchmark cluster\ntest/t/hooked.sh\twrong row\nbench/gone.sh\tgone\n' >"$t/$allow"
	git -C "$t" add -A
	out=$(check "$t")
	for want in 'test/t/bare.sh: starts a server' 'bench/noreason.sh: starts a server' 'noreason.sh has no reason' 'test/t/hooked.sh runs the leak check' 'bench/gone.sh starts no server'; do
		grep -q "$want" <<<"$out" || { echo "selftest: not refused: $want: $out"; exit 1; }
	done
	if grep -qE 'viapgtest|measure.sh: starts|comment.sh|stale.sh' <<<"$out"; then echo "selftest: a hooked, allowed or comment line was refused: $out"; exit 1; fi
	echo "check-leak-hooks selftest ok"
	exit 0
fi
out=$(check .)
[[ -z $out ]] || { echo "$out"; exit 1; }
