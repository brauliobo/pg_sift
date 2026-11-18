#!/usr/bin/env bash
# A wait that wakes on a timer burns a wakeup, a transaction or a lock probe each time and finds nothing: a worker that stays to look again, a backend that polls a latch every millisecond.
# This fails on a fixed sleep or a latch wait with a timeout in src/ (pg_usleep, sift_nap, sift_worker_nap, WaitLatch with WL_TIMEOUT) that has no row in scripts/polling.allow
# (FILE, a text of the line, why the wait cannot be an event). The fix is the event: a latch set by the party that changes the state, a condition variable, the lock itself.
#   scripts/check-polling.sh [--selftest]
cd "$(git rev-parse --show-toplevel)" || exit 1
allow=scripts/polling.allow
check() { # ROOT: prints a line for each wait without a row
	local root=$1
	git -C "$root" grep -nE '(^|[^[:alnum:]_])((pg_usleep|sift_nap|sift_worker_nap)\(|WaitLatch\([^;]*WL_TIMEOUT)' -- 'src/*.c' ':!src/*_test.c' ':!src/*/*_test.c' ':!src/*/test/*' |
		grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)' | grep -vE 'sift_(worker_)?nap\(long ms\)' | while IFS=: read -r f n text; do
			awk -F'\t' -v f="$f" -v t="$text" '$1 == f && index(t, $2) { found = 1 } END { exit !found }' "$root/$allow" ||
				echo "$f:$n: a wait on a timer:$text  (wake on an event instead; a wait that cannot be one goes in $allow with its reason)"
		done
}
if [[ ${1:-} == --selftest ]]; then
	t=$(mktemp -d "$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/check-polling.XXXXXX") || exit 1
	trap 'rm -rf "$t"' EXIT
	git -C "$t" init -q && mkdir -p "$t/scripts" "$t/src/fixture"
	printf 'void f(void)\n{\n\tpg_usleep(1000);\n}\n' >"$t/src/fixture/a.c"
	printf 'void f(void)\n{\n\tsift_nap(100);\n\tpg_usleep(10);\n}\n' >"$t/src/fixture/b.c"
	printf 'void f(void)\n{\n\tWaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT, 1, 0);\n\tWaitLatch(MyLatch, WL_LATCH_SET, 0, 0);\n}\n' >"$t/src/fixture/c.c"
	printf 'void f(void)\n{\n\t/* pg_usleep(5) is not taken */\n\tpg_usleep(7);\n}\n' >"$t/src/fixture/d_test.c"
	printf 'src/fixture/b.c\tsift_nap(100)\tpolls a thing\n' >"$t/$allow"
	git -C "$t" add -A
	out=$(check "$t")
	for want in 'src/fixture/a.c:3' 'src/fixture/b.c:4' 'src/fixture/c.c:3'; do grep -q "$want" <<<"$out" || { echo "selftest: $want was not refused: $out"; exit 1; }; done
	if grep -qE 'src/fixture/b.c:3|src/fixture/c.c:4|d_test' <<<"$out"; then echo "selftest: an allowed, untimed or test wait was refused: $out"; exit 1; fi
	echo "check-polling selftest ok"
	exit 0
fi
out=$(check .)
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "polling: every wait on a timer is listed in $allow"
