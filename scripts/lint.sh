#!/usr/bin/env bash
# Every check that needs no PostgreSQL, in one list: the lint job of the workflow, `make lint` and a local run before a push all read it. The checks run side by
# side (at most LINT_JOBS at a time, default half the cores and at least 8); the output of each is shown in the order below with its seconds, a failed one with its log, then the
# five slowest checks; the exit status is 1 when any failed. LINT_TIMES=FILE appends the wall and cpu seconds of every check to FILE.
#
#   scripts/lint.sh            all of them
#   scripts/lint.sh NAME...    only those (the names are the first column below)
#   scripts/lint.sh --macos    only the checks that run the BSD tools and the clang of macOS (MACOS_CHECKS): the others read the same files as the Linux job
#   scripts/lint.sh --selftest the runner itself
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 2

# the checks go through scripts/lint-cache.sh (the ones below with their own inputs, the others with the whole tree: TREE_UNCACHED): a pass is kept per box by the content of the
# check's inputs, and the same check on the same inputs in another run waits for the one that is running and takes its result; CI always runs them.

# bench-cases compares with the base branch: a CI checkout is shallow. Only a shallow one is given the base: in a full clone (fetch-depth 0, the macOS job) a depth-1 fetch makes
# origin/main a shallow commit, its history is cut, and the neutral check then judges every commit of HEAD that main reached before (3 600 instead of 271 on the merge of main)
[[ -z ${CI:-} || $(git rev-parse --is-shallow-repository) != true ]] || git fetch -q --depth=1 origin "${GITHUB_BASE_REF:-main}:refs/remotes/origin/${GITHUB_BASE_REF:-main}" 2>/dev/null || true

# scripts/ci.sh --selftest, cut in parts that run side by side (a part takes seconds to a minute; the whole was minutes of one process), first in the list because they start first. The key of
# a part is the content of the scripts, the tests other than the regression ones, the Makefile, the SQL and the worker that triggers the assertions family, and the names (not the content)
# of the files under test/: a new test is a change of the plan, an edit of a regression test is not. areas-files, which proves that every file of the tree is in a row of scripts/areas.tsv,
# reads every name; portability reads the content of the scripts, the shell tests and every Python file.
PLAN_IN="scripts Makefile test sql pg_sift.control src/agg/par ':(exclude)test/sql' ':(exclude)test/expected'"
plan_check() { CHECKS+=("plan-$1"$'\t'"scripts/lint-cache.sh plan-$1 --names $2 $3 -- scripts/ci.sh --selftest $1"); }   # PART NAMES-OF CONTENT
CHECKS=()
for part in shards-quick spread areas-select tooling budgets flavors formats; do plan_check $part test "$PLAN_IN"; done
plan_check areas-files . "$PLAN_IN"
plan_check portability test/t "scripts test/t '*.py'"
CHECKS+=(
	'syntax		for f in scripts/*.sh scripts/ci/*.sh $(git ls-files 'test/t/*.sh' 'test/t/lib/*.bash') bench/bench bench/*.sh; do bash -n "$f" || exit 1; done'
	'runner		scripts/lint.sh --selftest'
	'layout		scripts/restructure-src.sh --check'
	'path-redundancy	python3 -I scripts/check-path-redundancy.py --selftest && python3 -I scripts/check-path-redundancy.py --spec93 && python3 -I scripts/check-path-redundancy.py test/ scripts/focus/ tools/vk/ bench/test/ sqlite/tests/ mariadb/tests/'
	'tracked	! git ls-files | grep -E "(^|/)__pycache__/|[.]pyc$"'
	'portable	scripts/lint-cache.sh portable src/portable.list src/portable-col.list $(grep -hvE '\''^[[:space:]]*(#|$)'\'' src/portable.list src/portable-col.list | sed '\''s#^#src/#'\'') src/core/host.h sqlite scripts/check-portable.sh -- scripts/check-portable.sh'
	'classified	scripts/check-classified.sh --selftest && scripts/check-classified.sh'
	'fuzz-core	python3 -I scripts/check-fuzz-core.py --selftest && python3 -I scripts/check-fuzz-core.py'
	'alignment	python3 -I scripts/check-alignment.py --selftest && python3 -I scripts/check-alignment.py'
	'reference	scripts/gen-reference.sh --check'
	'update-drops	scripts/sql-parts.sh build && m=$(sed -n "s/^default_version *= *.\([0-9]*[.][0-9]*\).*/\1/p" pg_sift.control) && ! grep -nE "^(DROP|ALTER [A-Z ]* (DROP|RENAME))" sql/pg_sift--[0-9.]*--$m.sql'
	'changelog-summary	[ "$(sed -n "/^## Unreleased/,/^## [^U]/p" CHANGELOG.md | grep -c "things worth reading")" -le 1 ] || { echo "CHANGELOG.md: Unreleased has two summary entries (\"things worth reading\"); a merge left the old wording beside the new one"; exit 1; }'
	'neon-includes	for f in $(grep -l SIFT_HAVE_NEON $(git ls-files "src/*/neon.c" "src/*_neon.c") src/core/visibility/popcnt.c); do grep -q -E "^#include \"core/(cpu|neon|simd_isa)[.]h\"" $f || { echo "$f: SIFT_HAVE_NEON is defined by core/cpu.h; without it the object is empty on AArch64 and the library does not link there"; exit 1; }; done'
	'testing-regress	scripts/lint-cache.sh testing-regress Makefile test/sql scripts/check-testing-regress.sh -- scripts/check-testing-regress.sh'
	'worker-tests	scripts/lint-cache.sh worker-tests test/sql scripts/check-worker-tests.sh scripts/worker-tests.allow -- scripts/check-worker-tests.sh --selftest && scripts/check-worker-tests.sh'
	'host-stubs	a=$(mktemp -d) b=$(mktemp -d) && trap "rm -rf $a $b" EXIT && make -s -C sqlite B=$a $a/host/stubs.o && make -s -C mariadb OUT=$b $b/sqhost/stubs.o && echo "host/stubs.c compiles for the SQLite and MariaDB hosts"'
	'neutral	scripts/check-neutral.sh'
	'pgtest-bisect		scripts/selftest-pgtest-bisect.sh'
	'pgtest-auto-conf	d=$(mktemp -d) && trap "rm -rf $d" EXIT && echo "work_mem = 1GB" >$d/postgresql.auto.conf && scripts/pgtest.sh reset-auto-conf $d 2>/dev/null && [ ! -s $d/postgresql.auto.conf ]'
	'limits		scripts/gen-limits.sh --check'
	'code-id		scripts/code-id.sh --selftest'
	'options		scripts/gen-options.sh --check'
	'options-selftest	scripts/lint-cache.sh options-selftest scripts/gen-options scripts/gen-options.sh scripts/options-released.tsv docs/options.tsv src bench docs/results -- scripts/gen-options.sh --selftest'
	'docs		scripts/lint-cache.sh docs . -- python3 scripts/gen-docs.py --check'
	'docs-steps	scripts/check-docs-steps.sh --selftest && scripts/check-docs-steps.sh'
	'messages	scripts/gen-messages.sh --check && scripts/gen-messages.sh --selftest'
	'errors		scripts/check-errors.sh'
	'paging		scripts/check-paging.sh --selftest && scripts/check-paging.sh'
	'null-results	scripts/check-null.sh --selftest && scripts/check-null.sh'
	'locks		scripts/check-locks.sh --selftest && scripts/check-locks.sh'
	'dir-snapshot	scripts/check-dir-snapshot.sh'
	'focus		scripts/check-focus.sh --selftest && python3 -I scripts/focus-merge.py --selftest && scripts/check-focus.sh'
	'guards		scripts/check-guards.sh && scripts/check-guards.sh --selftest'
	'refusals	scripts/gen-refusals.sh --check'
	'approx		scripts/check-approx.sh'
	'kernels	python3 scripts/check-kernels.py'
	'memory		scripts/check-memory.sh --strict && scripts/check-memory.sh --selftest'
	'prod-viability-gate	python3 -I scripts/prod-viability-gate.py --selftest'
	'bpr-selftest	python3 -I scripts/bpr/run.py --selftest'
	'load-gate-selftest	python3 -I scripts/load/run.py --selftest'
	'mem-growth	scripts/check-mem-growth.sh --selftest && scripts/check-mem-growth.sh && python3 scripts/mem-slope.py selftest && python3 scripts/mem-slope-legacy.py selftest'
	'leak-gate	python3 -I scripts/mem-report.py selftest && python3 -I scripts/mem-report.py check'
	'pg-includes	scripts/check-pg-includes.sh && scripts/check-pg-includes.sh --selftest'
	'alternates	scripts/check-alternates.sh --selftest && scripts/check-alternates.sh'
	'quick-skip	scripts/check-quick-skip.sh --selftest && scripts/check-quick-skip.sh'
	'undefined-symbols	scripts/check-undefined.sh --selftest'
	'wt-tmpfs	scripts/wt-tmpfs.sh --selftest && python3 -I scripts/tmpfs-reap.py --selftest'
	'bench-tests	scripts/bench-tests.sh --selftest && scripts/bench-tests.sh'
	'bench-resume	scripts/lint-cache.sh bench-resume bench/steps bench/test/resume.sh -- bash bench/test/resume.sh'
	'bench-coverage	bench/bench cases --check'
	'slugs		scripts/check-slugs.sh'
	'file-size	scripts/check-file-size.sh'
	'sql-parts	scripts/sql-parts.sh check'
	'leakcheck	scripts/leakcheck.sh selftest'
	'leak-hooks	scripts/check-leak-hooks.sh --selftest && scripts/check-leak-hooks.sh'
	'raw-alloc	scripts/check-raw-alloc.sh'
	'arg-order	python3 -I scripts/check-arg-order.py --selftest && python3 -I scripts/check-arg-order.py'
	'upper-paths	scripts/check-upper-paths.sh --selftest && scripts/check-upper-paths.sh'
	'selftest-tools	scripts/lint-cache.sh selftest-tools scripts packaging bench/steps/cases.py bench/steps/cases -- bash -c '\''scripts/check-neutral.sh --selftest && scripts/package.sh --selftest && scripts/apt-install.sh --selftest && scripts/release.sh --selftest && scripts/plangate.sh --selftest && python3 -I bench/steps/cases.py cpus --selftest'\'
	'tap-versions	v=$(sed -n "s/^default_version *= *.\(.*\).$/\1/p" pg_sift.control) && f=$(git ls-files "test/t/compat/*.sh") && [ -n "$f" ] && ! grep -nE "(extversion|version\(\)).*== *${v//./[.]}( |\"|$)" $f'
	'budgets	scripts/budget.sh --selftest && scripts/check-budgets.sh --selftest && scripts/check-budgets.sh'
	'cache-kinds	[ "$(grep -rn "sift_cache_register(" src --include=*.c | grep -vc "^src/core/cachebudget")" -le "$(sed -n "s/^#define SIFT_CACHE_MAX_KINDS[[:space:]]*//p" src/core/cachebudget.h)" ] || { echo "more sift_cache_register() sites than SIFT_CACHE_MAX_KINDS (core/cachebudget.h): raise it with the new cache"; exit 1; }'
	'tap-timeouts	scripts/check-tap-timeouts.sh --selftest && scripts/check-tap-timeouts.sh'
	'tap-sleeps	scripts/check-tap-sleeps.sh --selftest && scripts/check-tap-sleeps.sh'
	'tap-coproc	scripts/check-tap-coproc.sh --selftest && scripts/check-tap-coproc.sh'
	'test-budget	scripts/check-test-budget.sh --selftest && scripts/check-test-budget.sh'
	'port-isa-check	sh scripts/port-isa-check.sh --selftest'
	'isa-scan	scripts/isa-scan.sh --selftest'
	'no-ifunc	scripts/check-no-ifunc.sh --selftest'
	'readme-tables	python3 -I scripts/check-readme-tables.py --selftest && python3 -I scripts/check-readme-tables.py'
	'ci-claim	scripts/test-ci-claim.sh'
	'ci-stop	scripts/test-ci-stop.sh'
	'build-id	scripts/test-build-id.sh'
	'ci-build-fail	scripts/test-ci-build-fail.sh'
	'ci-ready	scripts/test-ci-ready.sh'
	'scaling-lock	scripts/test-scaling-gate.sh'
	'relcache-slots	scripts/check-relcache-slots.sh --selftest && scripts/check-relcache-slots.sh'
	'polling	scripts/check-polling.sh --selftest && scripts/check-polling.sh'
	'threads	scripts/check-threads.sh --selftest && scripts/check-threads.sh'
	'pipefail-grep	scripts/check-pipefail-grep.sh --selftest && scripts/check-pipefail-grep.sh'
	'pg-bare	scripts/check-pg-bare.sh --selftest && scripts/check-pg-bare.sh'
	'scaling-triggers	scripts/check-scaling-triggers.sh'
	'perf-select	python3 -I scripts/test-perf-select.py'
	'perf-gate	python3 -I scripts/perf/selftest.py'
	'scratch	scripts/check-scratch.sh --selftest && scripts/check-scratch.sh'
	'no-raw-dsm-registry	! git grep -n "GetNamedDSMSegment" -- src ":!src/col/dsmreg.c" ":!src/col/dictshm/test.c"'
	'test-settings	scripts/check-test-settings.sh && scripts/check-test-settings.sh --selftest'
	'catalog-writes	scripts/check-catalog-writes.sh'
	'extlock	scripts/check-extlock.sh'
	'damage	scripts/check-damage.sh --selftest && scripts/check-damage.sh'
	'format-isolation	scripts/check-format-isolation.sh --selftest && scripts/check-format-isolation.sh'
	'ring-counter	scripts/check-ring-counter.sh --selftest && scripts/check-ring-counter.sh'
	'capacity	scripts/check-capacity.sh --selftest && scripts/check-capacity.sh'
	'complexity	scripts/check-complexity.sh --selftest && scripts/check-complexity.sh'
	'no-pattern-kill	! git grep -nE "\\b(pkil[l]|killal[l])\\b" -- scripts test bench packaging .github Makefile ":!scripts/land-selftest.sh"'
	'compat-calls	scripts/check-compat-calls.sh && scripts/check-compat-calls.sh --selftest'
	'package-text	scripts/check-package-text.sh --selftest && scripts/check-package-text.sh'
	'formula-sha	scripts/formula-sha.sh --selftest'
	'release-badge	[ "$(sed -n "s/^pkgver=//p" packaging/PKGBUILD)" = "$(grep -o "release-[0-9.]*-blue" README.md | head -n 1 | sed "s/release-\\(.*\\)-blue/\\1/")" ] || { echo "README.md release badge and packaging/PKGBUILD pkgver name different releases (scripts/release.sh bumps both)"; exit 1; }'
	'version-shape	! git grep -nE "version\(\).*cut -d[.] -f1-2|version\(\) ~ .\^\[0-9\]\+\[[.]\]\[0-9\]\+[$]." -- test'
	'format-literals	scripts/check-format-literals.sh && scripts/check-format-literals.sh --selftest'
	'expected-oracle	scripts/check-expected-oracle.py --selftest && scripts/check-expected-oracle.py'
	'codec-bytes	scripts/check-codec-bytes.py --selftest && scripts/check-codec-bytes.py'
	'affected-untracked	scripts/test-affected-untracked.sh'
	'affected-wide	scripts/test-affected-wide.sh'
	'gen-docs-skip	scripts/lint-cache.sh gen-docs-skip scripts bench/steps docs README.md -- scripts/test-gen-docs-skip.sh'
	'gen-docs-public	scripts/lint-cache.sh gen-docs-public scripts bench/steps docs README.md -- scripts/test-gen-docs-public.sh'
	'land		scripts/lint-cache.sh land scripts -- scripts/land.sh --selftest'
	'diff-fuzz	test/fuzz/diff/cli.py --selftest'
	'txn-fuzz	test/fuzz/txn/cli.py --selftest'
	'bm25-fuzz	test/fuzz/bm25.py --selftest'
	'fast-gate	scripts/fast-gate.sh --selftest'
	'fast-gate-select	python3 -I scripts/test-fast-gate-select.py'
	'test-hygiene	python3 -I scripts/check-test-hygiene.py --selftest && python3 -I scripts/check-test-hygiene.py'
	'pdtm		python3 -I scripts/pdtm.py --selftest && scripts/regress-times.sh --selftest'
)

# every other check reads the tree and the tools of the box only: it goes through scripts/lint-cache.sh too, keyed by every tracked and untracked file, so a run with no change since a
# pass takes it at once (a warm run spent 20 to 55 s on checks that computed the same answer). The tree is read once: the tracked files, their unstaged changes, and the untracked ones
# with their content (a check that walks a directory sees them; a/ is a nested repository). bench-coverage (the commits since the last tag) and alternates (the commits after NEUTRAL_BASE)
# read the history too: their key adds HEAD, the tags and the commit NEUTRAL_BASE names. Not cached: neutral reads a private vocabulary and the text of the pull request, pg-includes
# remembers each compile with the compiler and the server itself.
TREE_UNCACHED=' neutral pg-includes ' HISTORY=' bench-coverage alternates '
tree_digest() {
	local u
	git ls-files -s; git diff --no-ext-diff
	u=$(git ls-files --others --exclude-standard | grep -v '/$')
	[[ -z $u ]] || paste <(git hash-object --stdin-paths <<<"$u") - <<<"$u"
}
tree_cache() {
	local i name cmd tree history digest
	tree=$(tree_digest | scripts/sha256.sh | cut -c1-64)
	history=$({ echo "$tree"; git rev-parse HEAD; git for-each-ref refs/tags; git rev-parse -q --verify "${NEUTRAL_BASE:-}^{commit}"; } | scripts/sha256.sh | cut -c1-64)
	for i in "${!CHECKS[@]}"; do
		name=${CHECKS[i]%%$'\t'*} cmd=${CHECKS[i]##*$'\t'}
		[[ $cmd == scripts/lint-cache.sh* || $TREE_UNCACHED == *" $name "* ]] && continue
		[[ $HISTORY == *" $name "* ]] && digest=$history || digest=$tree
		CHECKS[i]=$name$'\t'"scripts/lint-cache.sh $name --digest $digest -- bash -c $(printf %q "$cmd")"
	done
}
# the checks that take longest on a cold box start first (the longest processing time first: launched in the order of the list, gen-docs-skip, land and affected-* came 112th to 116th
# and were the tail of the run); the output keeps the order of the list
FIRST='plan-formats pg-includes plan-tooling plan-areas-files plan-shards-quick gen-docs-skip bench-resume land affected-wide affected-untracked options-selftest plan-areas-select plan-spread'

# the checks the macOS job runs: the shell syntax and the portability of the scripts (the BSD paste, stat, seq, wc and sed; bash 4), the portable core and the stubs of the SQLite and MariaDB hosts
# (the clang of macOS), the runner and the helpers of the harness that call those tools. The 100 others (python, documents, policy of the sources) give the same answer on every system
MACOS_CHECKS='syntax runner plan-portability portable host-stubs pgtest-auto-conf port-isa-check isa-scan no-ifunc ci-claim ci-stop ci-ready ci-build-fail build-id leakcheck formula-sha'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# now NAME: the clock of a check in whole seconds (the selftest sets it per check, so no result depends on the load of the machine)
now() { echo "$SECONDS"; }

# one check: its output in $tmp/NAME, then "exit-status wall-seconds cpu-seconds" in $tmp/NAME.t (the cpu is the user and system time of the processes the check waited for)
run_check() {
	local name=$1 start rc
	start=$(now "$name")
	bash -c "$2" >"$tmp/$name" 2>&1
	rc=$?
	times >"$tmp/$name.cpu"
	echo "$rc $(( $(now "$name") - start )) $(tail -n 1 "$tmp/$name.cpu" | tr , . | awk '{ for (i = 1; i <= 2; i++) { split($i, a, "m"); t += a[1] * 60 + a[2] } print t }')" >"$tmp/$name.t"
	exit "$rc"
}

# at most LINT_JOBS checks at a time (default: half the cores, at least 8: a hosted runner of 4 cores waits on its checks as much as it computes); LINT_TIMES=FILE appends "name wall-seconds cpu-seconds" of every check
run_checks() {
	local cores=$(nproc 2>/dev/null || sysctl -n hw.ncpu) jobs c name rc wall cpu bad=0 sel=() order=() walls=()
	local -A cmd=()
	jobs=${LINT_JOBS:-$(( cores / 2 > 8 ? cores / 2 : 8 ))}
	for c in "${CHECKS[@]}"; do
		name=${c%%$'\t'*}
		[[ $# == 0 || " $* " == *" $name "* ]] || continue
		sel+=("$name") cmd[$name]=${c##*$'\t'}
	done
	for name in $FIRST; do [[ -z ${cmd[$name]:-} ]] || order+=("$name"); done
	for name in "${sel[@]}"; do [[ " $FIRST " == *" $name "* ]] || order+=("$name"); done
	for name in "${order[@]}"; do
		while (( $(jobs -rp | wc -l) >= jobs )); do wait -n; done
		( run_check "$name" "${cmd[$name]}" ) &
	done
	wait
	for name in "${sel[@]}"; do
		read -r rc wall cpu <"$tmp/$name.t"
		walls+=("$wall $name")
		[[ -z ${LINT_TIMES:-} ]] || printf '%s\t%s\t%s\n' "$name" "$wall" "$cpu" >>"$LINT_TIMES"
		if (( rc == 0 )); then echo "ok    $name (${wall}s): $(tail -n 1 "$tmp/$name" | cut -c1-150)"
		else bad=1; echo "FAIL  $name (exit $rc, ${wall}s)"; [[ -s $tmp/$name ]] && tail -n 25 "$tmp/$name" || echo "(the check printed nothing: a defect of the check)"; fi
	done
	printf '%s\n' "${walls[@]}" | sort -rn | head -n 5 | awk 'BEGIN { printf "slowest" } { printf "%s %s (%ss)", (NR > 1 ? "," : ""), $2, $1 } END { print "" }'
	return "$bad"
}

if [[ ${1:-} == --selftest ]]; then
	unset LINT_TIMES
	for name in $MACOS_CHECKS; do
		[[ -n $(printf '%s\n' "${CHECKS[@]}" | grep -m1 "^$name"$'\t') ]] || { echo "FAIL: lint.sh --macos runs $name, which is no check"; exit 1; }
	done
	now() { cat "$tmp/$1.clock" 2>/dev/null || echo 0; }
	CHECKS=($'quick\ttrue' "slow"$'\t'"echo 1 >$tmp/slow.clock; echo slept" $'broken\techo boom; exit 3' $'silent\texit 4')
	out=$(LINT_JOBS=2 run_checks); rc=$?
	want=$'ok    quick (Ns): \nok    slow (Ns): slept\nFAIL  broken (exit 3, Ns)\nboom\nFAIL  silent (exit 4, Ns)\n(the check printed nothing: a defect of the check)'
	[[ $rc == 1 ]] || { echo "FAIL: lint.sh exits $rc with two failed checks, want 1"; exit 1; }
	[[ $(sed -E 's/[0-9]+s\)/Ns)/' <<<"$out" | head -n 6) == "$want" ]] || { echo "FAIL: lint.sh output differs: $out"; exit 1; }
	[[ $out == *'ok    quick (0s)'* && $out == *'ok    slow (1s)'* && $out == *$'\nslowest slow (1s), '* ]] || { echo "FAIL: the slow check does not show its second and lead the slowest line: $out"; exit 1; }
	CHECKS=($'work\tawk "BEGIN { for (i = 0; i < 5000000; i++) s += i }"')
	LINT_TIMES=$tmp/times run_checks >/dev/null
	[[ $(cut -f1,3 "$tmp/times" | awk '$2 > 0') == work* ]] || { echo "FAIL: LINT_TIMES does not record the cpu seconds of a check that computes: $(cat "$tmp/times")"; exit 1; }
	mkdir "$tmp/at-once"
	CHECKS=(); for i in 1 2 3 4 5 6; do CHECKS+=("w$i"$'\t'"mkdir $tmp/at-once/$i; ls $tmp/at-once | wc -l >>$tmp/seen; sleep 0.2; rmdir $tmp/at-once/$i"); done
	LINT_JOBS=2 run_checks >/dev/null
	[[ $(sort -n "$tmp/seen" | tail -n 1) -le 2 ]] || { echo "FAIL: LINT_JOBS=2 ran $(sort -n "$tmp/seen" | tail -n 1) checks at once"; exit 1; }
	CHECKS=($'early\techo early >>'"$tmp/order" $'late\techo late >>'"$tmp/order")
	out=$(FIRST=late LINT_JOBS=1 run_checks)
	[[ $(cat "$tmp/order") == $'late\nearly' && $out == $'ok    early'*$'\nok    late'* ]] || { echo "FAIL: a check of FIRST does not start first, or the output leaves the order of the list: $(cat "$tmp/order") / $out"; exit 1; }
	CHECKS=($'plain\ttrue' $'bench-coverage\ttrue' $'neutral\ttrue' $'own\tscripts/lint-cache.sh own x -- true')
	tree_cache
	digest() { local d=${CHECKS[$1]#*--digest }; echo "${d%% *}"; }
	[[ ${CHECKS[0]} == $'plain\tscripts/lint-cache.sh plain --digest '*' -- bash -c true' && ${CHECKS[1]} == *' --digest '* && $(digest 0) != "$(digest 1)" &&
		${CHECKS[2]} == $'neutral\ttrue' && ${CHECKS[3]} == $'own\tscripts/lint-cache.sh own x -- true' ]] ||
		{ echo "FAIL: tree_cache keys a check by the tree, a check of the history by the history, and leaves neutral and a check with its own inputs: ${CHECKS[*]}"; exit 1; }
	echo "lint.sh: seconds of each check, the slowest five, the log of a failure, the cap on parallel checks (LINT_JOBS), the slow checks first (FIRST) and the keys of the cache"
	exit 0
fi

[[ ${1:-} != --macos ]] || set -- $MACOS_CHECKS
[[ -n ${CI:-} || -n ${SIFT_LINT_NOCACHE:-} ]] || tree_cache
run_checks "$@"
