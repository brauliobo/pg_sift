#!/usr/bin/env bash
# A shell test that sleeps a whole second or more for a state to arrive runs that second on every run, on a fast host too, and still fails on a slow one. The wait is
# `wait_for CONDITION SECONDS` of test/t/lib.bash (10 ms polls, then 50 ms, the cap is only for a host that is far too slow): a test that waits for a worker, a standby,
# a vacuum or a connection names what it waits for. A sleep of 0.5 s or more in test/t/*.sh and test/t/*.pl counts against scripts/tap-sleeps.allow, which holds the number each file has
# today (a file with a window that has no event: nothing may happen in it, a session is held open for the test). The number only goes down: a new sleep fails.
#   scripts/check-tap-sleeps.sh [--selftest]
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

sleeps() { # DIR: "FILE COUNT" of every shell or perl test with sleeps of 0.5 s or more outside comments
	local f n
	for f in $(find "$1/test/t" \( -name '*.sh' -o -name '*.pl' \) | LC_ALL=C sort); do
		n=$(grep -vE '^[[:space:]]*#' "$f" | grep -cE '\bsleep[[:space:]]*\(?[[:space:]]*([1-9][0-9]*(\.[0-9]+)?|0\.[5-9][0-9]*)([[:space:]]|$|\)|;|&)')
		(( n == 0 )) || echo "${f#"$1/test/t/"} $n"
	done
}
check() { # DIR ALLOW: the lines of the files that sleep more than their allowance
	local f n have
	while read -r f n; do
		have=$(awk -F'\t' -v f="$f" '$1 == f { print $2 }' "$2")
		(( n <= ${have:-0} )) || echo "$f: $n sleeps of 0.5 s or more, $((${have:-0})) allowed: wait on the state with wait_for (test/t/lib.bash) or poll_query_until (perl) instead of sleeping; if the test has a window in which nothing may happen, say so in scripts/tap-sleeps.allow"
	done < <(sleeps "$1")
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d "${TMPDIR:-/tmp}/check-tap-sleeps.XXXXXX") && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/test/t"
	# shellcheck disable=SC2016
	printf 'sleep 2\nsleep 0.1\n# sleep 5\nx=$(sleep 1; echo)\n' >"$d/test/t/a.sh"
	printf 'sleep 0.05\nwait_for true 5\n' >"$d/test/t/b.sh"
	printf 'sleep 1\n' >"$d/test/t/c.sh"
	printf 'sleep(3);\nsleep 2;\nselect(undef, undef, undef, 0.1);\n# sleep 9;\nusleep(2000000);\n' >"$d/test/t/d.pl"
	printf 'a.sh\t2\n' >"$d/allow"
	got=$(check "$d" "$d/allow")
	grep -q '^c.sh:' <<<"$got" || { echo "selftest: a sleep with no allowance was not refused: $got"; exit 1; }
	grep -q '^d.pl: 2 sleeps' <<<"$got" || { echo "selftest: the sleeps of a perl test were not counted: $got"; exit 1; }
	! grep -qE '^(a|b).sh:' <<<"$got" || { echo "selftest: an allowed, short or commented sleep was refused: $got"; exit 1; }
	printf 'a.sh\t1\n' >"$d/allow"
	got=$(check "$d" "$d/allow")
	grep -q '^a.sh:' <<<"$got" || { echo "selftest: a sleep over its allowance was not refused"; exit 1; }
	echo "check-tap-sleeps selftest ok"
	exit 0
fi
out=$(check . scripts/tap-sleeps.allow)
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "tap sleeps: no shell test sleeps more than scripts/tap-sleeps.allow allows"
