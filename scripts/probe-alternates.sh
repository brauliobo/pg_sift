#!/usr/bin/env bash
# Which majors of PostgreSQL need an alternate output (test/expected/NAME_N.out), found by running the tests without any: a test that passes on a major does not need one
# there, and one that fails shows its real output there (the diff of regression.diffs applied to the main output). Removing or changing an alternate is judged on every major
# the CI runs (17, 18, 18-arm64, 19), never on some of them: an alternate that looks dead on 17 and 19 can be the one 18 reads.
#
#   scripts/probe-alternates.sh make                  a local branch probe-alternates: HEAD without its alternate outputs (nothing is pushed; push it as a CI branch)
#   scripts/probe-alternates.sh report RUN_ID [--out DIR] [--write]   the same for a run that was dispatched already (on the probe branch)
#   scripts/probe-alternates.sh run CI_BRANCH [--pg 17,18,19] [--out DIR] [--write]
#                                                     dispatches ci.yml on CI_BRANCH (the probe branch, on the repository GH_REPO or the one gh names) for the tests that had
#                                                     an alternate, waits, and prints for each test and major: pass | needs an alternate (= which existing alternate) | not run.
#                                                     --write makes the alternates the probe shows (the existing file name when the output equals one, else the next number) and
#                                                     prints the scripts/check-alternates.sh --accept / --removed lines that list them and the majors they were probed on
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
PROBE=probe-alternates
IDX=$(git rev-parse --git-dir)/probe-alternates.index
trap 'rm -f "$IDX"' EXIT
ALL_MAJORS=17,18,19

alternate_tests() { git ls-files 'test/expected/*_[0-9]*.out' | sed 's#.*/##; s#_[0-9]*\.out$##' | sort -u; }

make_probe() {
	local tests tree
	tests=$(alternate_tests | xargs)
	[[ -n $tests ]] || { echo "probe-alternates: the tree has no alternate output"; exit 1; }
	GIT_INDEX_FILE=$IDX git read-tree HEAD
	GIT_INDEX_FILE=$IDX git rm -q --cached $(git ls-files 'test/expected/*_[0-9]*.out')
	tree=$(GIT_INDEX_FILE=$IDX git write-tree)
	git update-ref "refs/heads/$PROBE" "$(git commit-tree "$tree" -p HEAD -m "probe: no alternate outputs" -m "tests: $tests")"
	echo "branch $PROBE: HEAD without its alternates, for the tests: $tests"
	echo "snapshot it as a CI branch, then: scripts/probe-alternates.sh run CI_BRANCH"
}

dispatch() {
	local branch=$1 pg=$2 tests regex t0 id
	tests=$(git log -1 --format=%b "$PROBE" | sed -n 's/^tests: //p' | tr ' ' '|')
	[[ -n $tests ]] || { echo "probe-alternates: no branch $PROBE with its tests: scripts/probe-alternates.sh make"; exit 1; }
	regex="^installcheck-(($tests)|batch[0-9]+)\$"; t0=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	gh workflow run ci.yml ${GH_REPO:+-R "$GH_REPO"} --ref "$branch" -f "versions=${pg//,/ }" -f "only=$regex" >&2
	for _ in $(seq 20); do
		sleep 15
		id=$(gh run list ${GH_REPO:+-R "$GH_REPO"} --workflow ci.yml --branch "$branch" --event workflow_dispatch --limit 5 --json databaseId,createdAt --jq "[.[] | select(.createdAt >= \"$t0\")] | last | .databaseId // empty")
		[[ -z $id ]] || break
	done
	[[ -n $id ]] || { echo "probe-alternates: the dispatch did not start"; exit 1; }
	echo "run $id" >&2
	until [[ $(gh run view "$id" ${GH_REPO:+-R "$GH_REPO"} --json status --jq .status) == completed ]]; do sleep 60; done
	echo "$id"
}

# collect RUN OUT: OUT/ran/LABEL (the tests that ran on a major, with pass or fail) and OUT/actual/LABEL/TEST.out (what a failing test printed)
collect() {
	local id=$1 out=$2 jobs j label name
	mkdir -p "$out/ran" "$out/actual" "$out/raw"
	jobs=$(gh run view "$id" ${GH_REPO:+-R "$GH_REPO"} --json jobs --jq '.jobs[] | select(.name | test("^PostgreSQL")) | "\(.databaseId)\t\(.name)"')
	while IFS=$'\t' read -r j name; do
		label=$(sed -E 's/^PostgreSQL ([0-9]+) arm64.*/\1-arm64/; s/^PostgreSQL ([0-9]+).*/\1/' <<<"$name")
		[[ -s $out/ran/$label ]] && continue
		gh api --allow-escape-sequences "repos/${GH_REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}/actions/jobs/$j/logs" | sed -nE 's/.*ci: +(installcheck-[a-z0-9_]+) +(PASS|FAIL).*/\1 \2/p' >>"$out/ran/$label"
	done <<<"$jobs"
	[[ -d $out/raw/. && -n $(ls "$out/raw") ]] || gh run download "$id" ${GH_REPO:+-R "$GH_REPO"} -D "$out/raw"
	python3 - "$out" <<'PY'
import glob, os, re, subprocess, sys
out = sys.argv[1]
for art in sorted(glob.glob(f'{out}/raw/*')):
    m = re.search(r'pg(\d+)(-arm64)?', os.path.basename(art))
    label = m.group(1) + ('-arm64' if m.group(2) else '')
    seen = {l.split()[0] for l in open(f'{out}/ran/{label}')}
    with open(f'{out}/ran/{label}', 'a') as ran:
        for log in glob.glob(f'{art}/**/logs/installcheck-batch*.log', recursive=True):
            for l in open(log):
                m = re.match(r'(not )?ok +\d+ +- (\S+)', l)
                if m and 'installcheck-' + m.group(2) not in seen: ran.write(f'installcheck-{m.group(2)} {"FAIL" if m.group(1) else "PASS"}\n')
    for d in glob.glob(f'{art}/**/regression.diffs', recursive=True):
        test = os.path.basename(os.path.dirname(d))
        main = subprocess.run(['git', 'show', f'{os.environ["PROBE"]}~1:test/expected/{test}.out'], capture_output=True, text=True, check=True).stdout
        os.makedirs(f'{out}/actual/{label}', exist_ok=True)
        open(f'{out}/main_{test}.out', 'w').write(main)
        subprocess.run(['patch', '-s', '-o', f'{out}/actual/{label}/{test}.out', f'{out}/main_{test}.out', d], check=True)
PY
}

report() {
	local out=$1 write=$2
	python3 - "$out" "$write" <<'PY'
import collections, glob, os, re, subprocess, sys
out, write = sys.argv[1], sys.argv[2] == '1'
probe = os.environ['PROBE']
def show(path):
    return subprocess.run(['git', 'show', f'{probe}~1:{path}'], capture_output=True, text=True).stdout
ran = {os.path.basename(p): dict(l.split() for l in open(p) if l.strip()) for p in glob.glob(f'{out}/ran/*')}
labels = sorted(ran, key=lambda s: (int(s.split('-')[0]), s))
tests = sorted(subprocess.run(['git', 'log', '-1', '--format=%b', probe], capture_output=True, text=True).stdout.split('tests: ')[1].split())
existing = collections.defaultdict(dict)
for f in subprocess.run(['git', 'ls-tree', '--name-only', f'{probe}~1', 'test/expected/'], capture_output=True, text=True).stdout.split():
    m = re.match(r'test/expected/(.+)_(\d+)\.out$', f)
    if m: existing[m.group(1)][int(m.group(2))] = show(f)
for t in tests:
    groups = collections.defaultdict(list)
    cells = []
    for l in labels:
        st = ran[l].get('installcheck-' + t)
        if st is None: cells.append(f'{l}: not run'); continue
        p = f'{out}/actual/{l}/{t}.out'
        if st == 'PASS': cells.append(f'{l}: pass'); continue
        if not os.path.exists(p): cells.append(f'{l}: FAILS and shows no diff (read its log)'); continue
        txt = open(p).read(); groups[txt].append(l)
        eq = [n for n, c in existing[t].items() if c == txt]
        cells.append(f'{l}: needs an alternate' + (f' (= {t}_{eq[0]}.out)' if eq else ' (not equal to any alternate of the tree)'))
    print(f'{t}\n    ' + '\n    '.join(cells))
    if not write: continue
    used = {}
    for txt, ls in groups.items():
        eq = [n for n, c in existing[t].items() if c == txt]
        n = eq[0] if eq else max([*existing[t], *used.values(), 0]) + 1
        used[txt] = n
        open(f'test/expected/{t}_{n}.out', 'w').write(txt)
        print(f'scripts/check-alternates.sh --accept --on {",".join(ls)} test/expected/{t}_{n}.out')
    for n in existing[t]:
        if n not in used.values() and any(ran[l].get('installcheck-' + t) is None for l in labels):
            print(f'{t}_{n}.out stays: the test did not run on every major')
        elif n not in used.values():
            subprocess.run(['git', 'rm', '-q', '--ignore-unmatch', f'test/expected/{t}_{n}.out'])
            print(f'scripts/check-alternates.sh --removed --on {",".join(labels)} test/expected/{t}_{n}.out')
PY
}

case ${1:-} in
	make) make_probe ;;
	report)
		id=${2:?"usage: scripts/probe-alternates.sh report RUN_ID [--out DIR] [--write]"}; shift 2
		out=${TMPDIR:-/tmp}/probe-alternates-$id write=0
		while [[ $# -gt 0 ]]; do case $1 in --out) out=$2; shift ;; --write) write=1 ;; *) echo "unknown argument $1" >&2; exit 2 ;; esac; shift; done
		export PROBE
		collect "$id" "$out"; report "$out" "$write"; echo "outputs of the probe: $out" ;;
	run)
		branch=${2:?"usage: scripts/probe-alternates.sh run CI_BRANCH [--pg 17,18,19] [--out DIR] [--write]"}; shift 2
		pg=$ALL_MAJORS out=${TMPDIR:-/tmp}/probe-alternates-$$ write=0
		while [[ $# -gt 0 ]]; do case $1 in --pg) pg=$2; shift ;; --out) out=$2; shift ;; --write) write=1 ;; *) echo "unknown argument $1" >&2; exit 2 ;; esac; shift; done
		export PROBE
		id=$(dispatch "$branch" "$pg" | tail -1)
		collect "$id" "$out"
		report "$out" "$write"
		echo "outputs of the probe: $out" ;;
	*) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
