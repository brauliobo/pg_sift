#!/usr/bin/env bash
# Every check that needs no PostgreSQL, in one list: the lint job of the workflow, `make lint` and a local run before a push all read it. The checks run side by
# side; the output of each is shown in the order below, a failed one with its log, the exit status is 1 when any failed.
#
#   scripts/lint.sh            all of them
#   scripts/lint.sh NAME...    only those (the names are the first column below)
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 2

# bench-fresh, docs, plan, portable, testing-regress and the selftests of the tools go through scripts/lint-cache.sh: a pass is kept per box by the content of the check's inputs, and the same check on the same inputs in another run
# waits for the one that is running and takes its result; CI always runs them.

# bench-cases compares with the base branch: a CI checkout is shallow. Only a shallow one is given the base: in a full clone (fetch-depth 0, the macOS job) a depth-1 fetch makes
# origin/main a shallow commit, its history is cut, and the neutral check then judges every commit of HEAD that main reached before (3 600 instead of 271 on the merge of main)
[[ -z ${CI:-} || $(git rev-parse --is-shallow-repository) != true ]] || git fetch -q --depth=1 origin "${GITHUB_BASE_REF:-main}:refs/remotes/origin/${GITHUB_BASE_REF:-main}" 2>/dev/null || true

CHECKS=(
	'syntax		for f in scripts/*.sh scripts/ci/*.sh test/t/*.sh bench/bench bench/*.sh; do bash -n "$f" || exit 1; done'
	'layout		scripts/restructure-src.sh --check'
	'tracked	! git ls-files | grep -E "(^|/)__pycache__/|[.]pyc$"'
	'portable	scripts/lint-cache.sh portable src/portable.list src/portable-col.list $(grep -hvE '\''^[[:space:]]*(#|$)'\'' src/portable.list src/portable-col.list | sed '\''s#^#src/#'\'') src/core/host.h sqlite scripts/check-portable.sh -- scripts/check-portable.sh'
	'testing-regress	scripts/lint-cache.sh testing-regress Makefile test/sql scripts/check-testing-regress.sh -- scripts/check-testing-regress.sh'
	'host-stubs	a=$(mktemp -d) b=$(mktemp -d) && trap "rm -rf $a $b" EXIT && make -s -C sqlite B=$a $a/host_stubs.o && make -s -C mariadb OUT=$b $b/host_stubs.o && echo "host_stubs.c compiles for the SQLite and MariaDB hosts"'
	'neutral	scripts/check-neutral.sh'
	'limits		scripts/gen-limits.sh --check'
	'errors		scripts/check-errors.sh'
	'approx		scripts/check-approx.sh'
	'kernels	python3 scripts/check-kernels.py'
	'memory		scripts/check-memory.sh && scripts/check-memory.sh --selftest'
	'bench-tests	for f in bench/test/test_*.py; do python3 -I "$f" || exit 1; done'
	'slugs		scripts/check-slugs.sh'
	'leakcheck	scripts/leakcheck.sh selftest'
	'plan		scripts/lint-cache.sh plan --names scripts test Makefile sql pg_sift.control src/agg/par.c '\'':(exclude)test/sql'\'' '\'':(exclude)test/expected'\'' '\''*.py'\'' -- scripts/ci.sh --selftest'
	'selftest-tools	scripts/lint-cache.sh selftest-tools scripts packaging bench/steps/cases.py bench/steps/cases -- bash -c '\''scripts/check-neutral.sh --selftest && scripts/package.sh --selftest && scripts/apt-install.sh --selftest'\'
	'ci-claim	scripts/test-ci-claim.sh'
	'land		scripts/lint-cache.sh land scripts -- scripts/land.sh --selftest'
	'diff-fuzz	test/fuzz/diff_fuzz.py --selftest'
)

want=" $* "
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
pids=() names=()
for c in "${CHECKS[@]}"; do
	name=${c%%$'\t'*} cmd=${c##*$'\t'}
	[[ $# == 0 || $want == *" $name "* ]] || continue
	bash -c "$cmd" >"$tmp/$name" 2>&1 &
	pids+=($!) names+=("$name")
done
bad=0
for i in "${!pids[@]}"; do
	if wait "${pids[i]}"; then echo "ok    ${names[i]}: $(tail -n 1 "$tmp/${names[i]}" | cut -c1-150)"
	else bad=1; echo "FAIL  ${names[i]}"; tail -n 25 "$tmp/${names[i]}"; fi
done
(( ! bad )) || exit 1
