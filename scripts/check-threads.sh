#!/usr/bin/env bash
# Every parallel operator runs on the one pool of core/par/pool.h. A second pool (a thread of its own for a scan, a scorer, a merge) is a second set of cancel, terminate and memory rules to keep right.
# This fails on a thread created in src/ outside core/par/pool.c. The fix is an operator of core/par/morsel.h (SiftParOps) on that pool, not a thread of its own.
#   scripts/check-threads.sh [--selftest]
cd "$(git rev-parse --show-toplevel)" || exit 1
check() { # ROOT: prints a line for each thread created outside the pool
	git -C "$1" grep -nE '(^|[^[:alnum:]_])(pthread_create|thrd_create|std::thread)\(?' -- 'src/*.c' 'src/*.h' ':!src/core/par/pool.c' ':!src/*_test.c' ':!src/*/*_test.c' ':!src/*/test/*' |
		grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)' | while IFS=: read -r f n text; do
			echo "$f:$n: a thread outside the pool:$text  (make the work an operator of core/par/morsel.h on the pool of core/par/pool.h)"
		done
}
if [[ ${1:-} == --selftest ]]; then
	t=$(mktemp -d "$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/check-threads.XXXXXX") || exit 1
	trap 'rm -rf "$t"' EXIT
	git -C "$t" init -q && mkdir -p "$t/src/core/par" "$t/src/fixture"
	printf 'void f(void)\n{\n\tpthread_create(&t, NULL, g, NULL);\n}\n' >"$t/src/fixture/a.c"
	printf 'void f(void)\n{\n\tpthread_create(&t, NULL, g, NULL);\n}\n' >"$t/src/core/par/pool.c"
	printf 'void f(void)\n{\n\t/* pthread_create(x) is not taken */\n}\n' >"$t/src/fixture/b.c"
	printf 'void f(void)\n{\n\tpthread_create(&t, NULL, g, NULL);\n}\n' >"$t/src/fixture/c_test.c"
	git -C "$t" add -A
	out=$(check "$t")
	grep -q 'src/fixture/a.c:3' <<<"$out" || { echo "selftest: a thread outside the pool was not refused: $out"; exit 1; }
	if grep -qE 'pool.c|b.c|c_test' <<<"$out"; then echo "selftest: the pool, a comment or a test was refused: $out"; exit 1; fi
	echo "check-threads selftest ok"
	exit 0
fi
out=$(check .)
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "threads: every thread is created by core/par/pool.c"
