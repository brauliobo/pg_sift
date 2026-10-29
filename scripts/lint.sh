#!/usr/bin/env bash
# Every check that needs no PostgreSQL, in one list: the lint job of the workflow, `make lint` and a local run before a push all read it. The checks run side by
# side; the output of each is shown in the order below, a failed one with its log, the exit status is 1 when any failed.
#
#   scripts/lint.sh            all of them
#   scripts/lint.sh NAME...    only those (the names are the first column below)
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 2

# bench-cases compares with the base branch: a CI checkout is shallow. Only a shallow one is given the base: in a full clone (fetch-depth 0, the macOS job) a depth-1 fetch makes
# origin/main a shallow commit, its history is cut, and the neutral check then judges every commit of HEAD that main reached before (3 600 instead of 271 on the merge of main)
[[ -z ${CI:-} || $(git rev-parse --is-shallow-repository) != true ]] || git fetch -q --depth=1 origin "${GITHUB_BASE_REF:-main}:refs/remotes/origin/${GITHUB_BASE_REF:-main}" 2>/dev/null || true

CHECKS=(
	'syntax		for f in scripts/*.sh test/t/*.sh bench/bench bench/*.sh; do bash -n "$f" || exit 1; done'
	'layout		scripts/restructure-src.sh --check'
	'portable	scripts/check-portable.sh'
	'testing-regress	scripts/check-testing-regress.sh'
	'host-stubs	a=$(mktemp -d) b=$(mktemp -d) && trap "rm -rf $a $b" EXIT && make -s -C sqlite B=$a $a/host_stubs.o && make -s -C mariadb OUT=$b $b/host_stubs.o && echo "host_stubs.c compiles for the SQLite and MariaDB hosts"'
	'neutral	scripts/check-neutral.sh'
	'limits		scripts/gen-limits.sh --check'
	'errors		scripts/check-errors.sh'
	'approx		scripts/check-approx.sh'
	'kernels	python3 scripts/check-kernels.py'
	'memory		scripts/check-memory.sh && scripts/check-memory.sh --selftest'
	'slugs		scripts/check-slugs.sh'
	'leakcheck	scripts/leakcheck.sh selftest'
	'plan		scripts/ci.sh --selftest'
	'ci-claim	scripts/test-ci-claim.sh'
	'land		scripts/land.sh --selftest'
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
