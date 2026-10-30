#!/usr/bin/env bash
# `scripts/land.sh --selftest`: the landing lock, exit codes, cleanup and privacy gate with a fake worker in a temporary state directory (no GitHub, no network, no merge,
# a few seconds):
#   1. a second run for the SAME PR exits 7 at once and touches nothing of the first; the lock is free after
#   2. runs for different PRs go side by side, each in its own work tree: the end of one removes nothing of the other
#   3. the global merge lock is held around the merge alone: preparations overlap, merges do not, a held merge or legacy lock blocks the merge
#   4. the exit code of the worker (2 = SOURCE CONFLICT, 4 = a gate is red) reaches the caller
#   5. TERM and KILL of the holder release its lock at once and the next run of that PR goes on; TERM stops the worker and removes the work tree
#   6. a background process of the worker or of the merge call that outlives the landing holds no lock; the normal end removes what the worker left
#   7. the privacy gate fails on a hit and closed without its deny list
#   7b. overlapping runs of one PR fetch its head without disturbing each other
#   8. land-union.py keeps both sides of added lines only
#   9. a CHANGELOG a merge of the tip left broken (markers, a duplicated line, an entry moved out of Unreleased or lost) is refused
#  10. the verdict on the required checks (ok, missing, pending, red, a head that is not the PR head, a re-run) and the bounded wait for pending and missing ones
#  11. the script has no process-table matching and no sleep that does not say what it waits for
#  12. a lint waits for a free slot, and a tree that passed once is not linted again
set -u
[[ $(uname) == Linux ]] && command -v flock >/dev/null && command -v setsid >/dev/null || { echo "land selftest: skipped (needs Linux with flock and setsid)"; exit 0; }
HERE=$(cd "$(dirname "$0")" && pwd)
LAND=$HERE/land.sh
T=$(mktemp -d) bad=0
finish() {
	local f
	for f in "$T"/*.pid; do [[ -e $f ]] && kill -KILL -- "-$(<"$f")" 2>/dev/null; done
	rm -rf "$T"
}
trap finish EXIT
fail() { echo "FAIL: $*"; bad=1; }
wait_for() { local i; for ((i = 0; i < 200; i++)); do "$@" 2>/dev/null && return 0; sleep 0.05; done; fail "timed out waiting for: $*"; return 1; }
has() { grep -q -- "$1" "$2"; }
gone() { ! kill -0 "$1"; }
held() { ! flock -n "$1" true; }
first_line() { grep -n -x -m1 -- "$1" "$T/events" | cut -d: -f1; }
events() { tr '\n' ' ' <"$T/events"; }

mkdir "$T/tmp"
export LAND_DIR=$T/state LAND_REPO=test/repo LAND_CLONE=$T/clone LAND_WORKER=$T/worker.sh TMPDIR=$T/tmp T_DIR=$T
unset LAND_DRY LAND_RUN LAND_DENY LAND_LEGACY_LOCK
git init -q -b main "$T/clone"
g() { git -C "$T/clone" -c user.name=t -c user.email=t@example.com "$@"; }
echo base >"$T/clone/f"; g add f; g commit -q -m base
echo fine >>"$T/clone/f"; g commit -q -am fine
echo secret-word-abc >>"$T/clone/f"; g commit -q -am change
worktrees() { g worktree list | wc -l; }

# the fake worker: events around a preparation sleep (FAKE_SLEEP), then, with FAKE_MERGE, a merge under the real merge_locked of land.sh; FAKE_RC="N:code ...": the exit code
# of PR N; FAKE_WT: a work tree in its own work directory, checked at the end; FAKE_BG, FAKE_BGM: leave a background process (of the worker, of the merge call); it records its
# pid in N.pid so the test can end a worker that outlived its holder
cat >"$T/worker.sh" <<'WORKER'
#!/usr/bin/env bash
pr=$1
echo $$ >"$T_DIR/$pr.pid.tmp"; mv "$T_DIR/$pr.pid.tmp" "$T_DIR/$pr.pid"
echo "start $pr" >>"$T_DIR/events"
echo "$LAND_WORK" >"$T_DIR/work.$pr"
[[ -z ${FAKE_WT:-} ]] || git -C "$LAND_CLONE" worktree add -q --detach "$LAND_WORK/wt"
[[ -z ${FAKE_BG:-} ]] || { sleep 60 & echo $! >"$T_DIR/bg.pid"; }
sleep "${FAKE_SLEEP:-0.4}"
[[ -z ${FAKE_WT:-} || -d $LAND_WORK/wt ]] || echo "wt gone $pr" >>"$T_DIR/events"
echo "prep end $pr" >>"$T_DIR/events"
if [[ -n ${FAKE_MERGE:-} ]]; then
	( . "$LAND_SCRIPT"; merge_locked bash -c 'echo "merge start $1" >>"$T_DIR/events"; [[ -z ${FAKE_BGM:-} ]] || { sleep 60 & echo $! >"$T_DIR/bgm.pid"; }; sleep 0.4; echo "merge end $1" >>"$T_DIR/events"' _ "$pr" )
fi
echo "end $pr" >>"$T_DIR/events"
rc=$(sed -n "s/.*\b$pr:\([0-9]*\).*/\1/p" <<<"${FAKE_RC:-}")
[[ -z $rc ]] || { echo "worker says $rc"; exit "$rc"; }
WORKER
chmod +x "$T/worker.sh"
export LAND_SCRIPT=$LAND

# 1. the same PR twice: the second exits 7 at once, the first is not disturbed
: >"$T/events"
FAKE_SLEEP=0.8 "$LAND" 1 >"$T/a.out" 2>&1 & a=$!
wait_for test -e "$T/state/pr-1.info"
"$LAND" 1 >"$T/b.out" 2>&1; rb=$?
kill -0 "$a" 2>/dev/null || fail "the second run for the same PR did not return at once"
wait "$a"; ra=$?
(( rb == 7 && ra == 0 )) && has "already running" "$T/b.out" && has "pid=$a" "$T/b.out" || fail "same PR twice: second exit $rb (want 7) with the holder named, first exit $ra"
[[ $(events) == "start 1 prep end 1 end 1 " ]] || fail "the second run for the same PR started a worker: $(events)"
[[ ! -e $T/state/pr-1.info ]] && flock -n "$T/state/pr-1.lock" true || fail "the lock or the info file is left after a landing"

# 2. different PRs side by side, each in its own work tree (a fixed path shared by two runs lost one of them to the cleanup of the other)
: >"$T/events"
FAKE_WT=1 FAKE_SLEEP=0.3 "$LAND" 2 >"$T/a.out" 2>&1 & a=$!
FAKE_WT=1 FAKE_SLEEP=0.9 "$LAND" 3 >"$T/b.out" 2>&1 & b=$!
wait "$a"; ra=$?; wait "$b"; rb=$?
(( ra == 0 && rb == 0 )) || fail "landings of different PRs exit $ra, $rb"
(( $(first_line 'start 3') < $(first_line 'end 2') )) || fail "a landing of another PR waited for this one: $(events)"
has '^wt gone' "$T/events" && fail "the end of one landing removed the work tree of the other: $(events)"
[[ $(<"$T/work.2") != "$(<"$T/work.3")" && -z $(ls "$T/tmp") ]] && (( $(worktrees) == 1 )) || fail "the work directories are shared or left behind"

# 3. the merge lock is held around the merge alone: the preparations of 4 and 5 overlap, their merges do not, and 5 did not wait for the whole landing of 4
: >"$T/events"
FAKE_MERGE=1 FAKE_SLEEP=0.5 "$LAND" 4 >"$T/a.out" 2>&1 & a=$!
FAKE_MERGE=1 FAKE_SLEEP=0.5 "$LAND" 5 >"$T/b.out" 2>&1 & b=$!
wait "$a"; wait "$b"
awk '/^merge start/ { n++; if (open) bad = 1; open = 1 } /^merge end/ { open = 0 } END { exit bad || n != 2 }' "$T/events" || fail "two merges overlapped: $(events)"
(( $(first_line 'start 5') < $(first_line 'end 4') )) || fail "a landing waited for the whole landing of another PR: $(events)"
# shellcheck source=land.sh
chain() { ( . "$LAND"; LAND_LOCK_WAIT=1 merge_locked true; echo $? ); }
touch "$T/legacy.lock"; export LAND_LEGACY_LOCK=$T/legacy.lock
[[ $(chain) == 0 ]] || fail "merge_locked does not run its command with both locks free"
flock -o "$T/legacy.lock" sleep 2 & lh=$!
wait_for held "$T/legacy.lock"; [[ $(chain) == 75 ]] || fail "a held legacy lock does not block the merge"
kill "$lh" 2>/dev/null
flock -o "$T/state/merge.lock" sleep 2 & lh=$!
wait_for held "$T/state/merge.lock"; [[ $(chain) == 75 ]] || fail "a held merge.lock does not block the merge"
kill "$lh" 2>/dev/null
rm "$T/legacy.lock"; [[ $(chain) == 0 ]] || fail "a missing legacy lock file blocks the merge"
unset LAND_LEGACY_LOCK

# 4. the exit code of the worker reaches the caller; the lock is free again
FAKE_RC="6:4 7:2" "$LAND" 6 >"$T/c.out" 2>&1; r6=$?
FAKE_RC="6:4 7:2" "$LAND" 7 >"$T/c.out" 2>&1; r7=$?
(( r6 == 4 && r7 == 2 )) && flock -n "$T/state/pr-6.lock" true || fail "the exit codes do not reach the caller (got $r6, $r7) or the lock is left"
"$LAND" abc >/dev/null 2>&1; (( $? == 2 )) || fail "a PR that is not a number is exit 2"

# 5. TERM and KILL of the holder: its lock is free at once; TERM stops the worker and cleans the work tree
for sig in TERM KILL; do
	: >"$T/events"; rm -f "$T/8.pid"
	FAKE_SLEEP=30 FAKE_WT=1 setsid "$LAND" 8 >"$T/h.out" 2>&1 & h=$!
	wait_for test -s "$T/8.pid"
	"$LAND" 8 >/dev/null 2>&1; (( $? == 7 )) || fail "the live holder did not answer 7 to a second run"
	kill -$sig "$h"
	wait_for gone "$h"; wait "$h" 2>/dev/null   # the holder ends at once: it does not wait for its worker
	FAKE_SLEEP=0.1 "$LAND" 8 >"$T/w.out" 2>&1; rw=$?
	(( rw == 0 )) || fail "$sig of the holder: the next run of the PR exits $rw"
	if [[ $sig == TERM ]]; then
		kill -0 "$(<"$T/8.pid")" 2>/dev/null && fail "TERM of the holder left its worker running"
		[[ -z $(ls "$T/tmp") ]] && (( $(worktrees) == 1 )) && [[ ! -e $T/state/pr-8.info ]] || fail "TERM left the work tree, the temporary directory or the info file"
	else
		kill -KILL -- "-$(<"$T/8.pid")" 2>/dev/null   # a KILL cannot stop the worker: the test ends it
		rm -rf "$T/tmp"/*; g worktree prune
	fi
done

# 6. a background process of the worker or of the merge call outlives the landing and holds no lock; the normal end removes what the worker left
rm -f "$T/bg.pid" "$T/bgm.pid"; FAKE_BG=1 FAKE_BGM=1 FAKE_MERGE=1 FAKE_WT=1 FAKE_SLEEP=0.1 "$LAND" 9 >"$T/d.out" 2>&1
kill -0 "$(<"$T/bg.pid")" 2>/dev/null && kill -0 "$(<"$T/bgm.pid")" 2>/dev/null && flock -n "$T/state/pr-9.lock" true && flock -n "$T/state/merge.lock" true || fail "a process that outlives the landing keeps a lock (or was never started)"
kill "$(<"$T/bg.pid")" "$(<"$T/bgm.pid")" 2>/dev/null
[[ -z $(ls "$T/tmp") ]] && (( $(worktrees) == 1 )) || fail "a normal end left the work tree or the temporary directory"

# 7. the privacy gate (the functions of land.sh, sourced): a hit, a clean range, text, and no usable deny list
export LAND_DENY=$T/deny
# shellcheck source=land.sh
rc_of() { ( . "$LAND"; "$@" >/dev/null 2>&1; echo $? ); }
printf '# private\n\nsecret-word-[a-z]+\n' >"$LAND_DENY"
[[ $(rc_of privacy_range HEAD~2 HEAD~1) == 0 && $(rc_of privacy_range HEAD~2 HEAD) == 1 ]] || fail "privacy_range does not tell a clean range from one with a hit"
[[ $(echo 'a SECRET-WORD-x' | rc_of privacy_text) == 1 && $(echo fine | rc_of privacy_text) == 0 ]] || fail "privacy_text"
for what in missing comments regex; do
	case $what in missing) rm -f "$LAND_DENY" ;; comments) printf '# nothing\n\n' >"$LAND_DENY" ;; regex) printf '(\n' >"$LAND_DENY" ;; esac
	[[ $(rc_of privacy_range HEAD~2 HEAD~1) == 2 && $(rc_of privacy_text) == 2 ]] || fail "the privacy gate does not fail closed with a $what deny list"
done
unset LAND_DENY

# 7b. the head of a PR is fetched through a handle of the run: overlapping runs of one PR do not delete each other's
git init -q --bare "$T/origin.git"; g remote add origin "$T/origin.git"; g push -q origin HEAD:refs/pull/5/head
# shellcheck source=land.sh
fp() { ( . "$LAND"; for _ in $(seq 15); do [[ $(fetch_pr_head 5) == "$(g rev-parse HEAD)" ]] || exit 1; done ); }
fp & p1=$!; fp & p2=$!; fp & p3=$!
wait "$p1" && wait "$p2" && wait "$p3" || fail "overlapping fetches of one PR head disturb each other"
# and the tip of a branch that other landings fetch into the same clone while it moves
git --git-dir="$T/origin.git" update-ref refs/heads/b "$(g rev-parse HEAD)"
# shellcheck source=land.sh disable=SC2034
ft() { ( . "$LAND"; base=b; for _ in $(seq 12); do fetch_tip || exit 1; done ); }
mover() { local i p; for i in $(seq 30); do p=$(git --git-dir="$T/origin.git" rev-parse refs/heads/b); git --git-dir="$T/origin.git" -c user.name=t -c user.email=t@example.com update-ref refs/heads/b "$(git --git-dir="$T/origin.git" -c user.name=t -c user.email=t@example.com commit-tree -p "$p" -m "n$i" "$p^{tree}")"; done; }
mover & pm=$!; ft & p1=$!; ft & p2=$!; ft & p3=$!
wait "$pm"; wait "$p1" && wait "$p2" && wait "$p3" || fail "overlapping fetches of a moving tip fail"

# 8. the union resolver
while IFS='|' read -r want name ours theirs; do
	printf 'A\n<<<<<<< HEAD\n%b\n=======\n%b\n>>>>>>> tip\nZ\n' "$ours" "$theirs" >"$T/$name"
	python3 "$HERE/land-union.py" "$T/$name"; rc=$?
	(( rc == want )) || fail "land-union.py: $name exits $rc, want $want"
	(( want )) || { (( $(wc -l <"$T/$name") == 4 )) && ! has '^[<=>]\{7\}' "$T/$name"; } || fail "land-union.py did not keep both sides once, without markers"
done <<'EOF2'
0|Makefile|TESTING_REGRESS += a|TESTING_REGRESS += a\nTESTING_REGRESS += b
1|Makefile|CFLAGS = -O2|CFLAGS = -O3
0|focus.tsv|a|a\nb
EOF2

# 9. the CHANGELOG check: a fixed tip, merge base and PR head; each result is what a merge of the tip could leave
cl() { printf '# Changelog\n\n## Unreleased\n\n%b\n## 3.5\n\n%b\n' "$1" "$2"; }
cl '- Added: tip entry.\n' '- Released: old.\n' >"$T/cl.tip"; cp "$T/cl.tip" "$T/cl.mb"
cl '- Added: PR entry.\n- Added: tip entry.\n' '- Released: old.\n' >"$T/cl.head"
while IFS='|' read -r want name unreleased released; do
	cl "$unreleased" "$released" >"$T/cl.$name"
	python3 "$HERE/land-union.py" --changelog "$T/cl.$name" "$T/cl.mb" "$T/cl.head" "$T/cl.tip" >/dev/null; rc=$?
	(( rc == want )) || fail "the CHANGELOG check of '$name' exits $rc, want $want"
done <<'EOF3'
0|ok|- Added: PR entry.\n- Added: tip entry.\n|- Released: old.\n
1|duplicated|- Added: PR entry.\n- Added: tip entry.\n- Added: PR entry.\n|- Released: old.\n
1|markers|<<<<<<< HEAD\n- Added: PR entry.\n=======\n- Added: tip entry.\n>>>>>>> tip\n|- Released: old.\n
1|moved|- Added: tip entry.\n|- Released: old.\n- Added: PR entry.\n
1|lost|- Added: tip entry.\n|- Released: old.\n
1|released changed|- Added: PR entry.\n- Added: tip entry.\n|
EOF3
# a duplicate the tip already carries is not the merge's: the same result passes when the tip had the line twice
cl '- Added: tip entry.\n- Added: tip entry.\n' '- Released: old.\n' >"$T/cl.tipdup"; cl '- Added: PR entry.\n- Added: tip entry.\n- Added: tip entry.\n' '- Released: old.\n' >"$T/cl.dupok"
python3 "$HERE/land-union.py" --changelog "$T/cl.dupok" "$T/cl.mb" "$T/cl.head" "$T/cl.tipdup" >/dev/null || fail "a duplicate the tip already carries is blamed on the merge"

# 10. verdicts with a fake gh (one JSON line per call, the last one repeats), and the bounded wait
mkdir "$T/bin"
cat >"$T/bin/gh" <<'GH'
#!/usr/bin/env bash
n=$(( $(cat "$T_DIR/gh.n" 2>/dev/null || echo 0) + 1 )); echo $n >"$T_DIR/gh.n"
line=$(sed -n "${n}p" "$T_DIR/gh.json"); [[ -n $line ]] || line=$(tail -n 1 "$T_DIR/gh.json")
echo "$line"
GH
chmod +x "$T/bin/gh"
ck() { printf '{"name":"%s","status":"%s","conclusion":"%s","completedAt":"%s"}' "$1" "$2" "$3" "$4"; }
J() { printf '{"headRefOid":"%s","statusCheckRollup":[%s]}\n' "$1" "$2"; }
N_OK=$(ck neutral COMPLETED SUCCESS 2026-01-01T00:00:01Z)
vd() { rm -f "$T/gh.n"; cat >"$T/gh.json"; timeout 10 bash -c '. "$1"; pr=1 repo=x/y; PATH=$2:$PATH; "${@:3}"' _ "$LAND" "$T/bin" "$@"; }
while IFS='|' read -r want head rollup; do
	got=$(J "$head" "$(eval "echo $rollup")" | vd checks_verdict H "neutral lint")
	[[ $got == "$want" ]] || fail "checks_verdict: want '$want', got '$got' for $rollup"
done <<'EOF4'
ok|H|$N_OK,$(ck lint COMPLETED SUCCESS 2026-01-01T00:00:02Z)
missing neutral, lint|H|
pending lint|H|$N_OK,$(ck lint IN_PROGRESS "" "")
red lint|H|$N_OK,$(ck lint COMPLETED FAILURE 2026-01-01T00:00:02Z)
red neutral (no success)|H|$(ck neutral COMPLETED SKIPPED 2026-01-01T00:00:01Z),$(ck lint COMPLETED SUCCESS 2026-01-01T00:00:02Z)
pending the new head of the PR|OLD|$N_OK,$(ck lint COMPLETED SUCCESS 2026-01-01T00:00:02Z)
ok|H|$N_OK,$(ck lint COMPLETED FAILURE 2026-01-01T00:00:02Z),$(ck lint COMPLETED SUCCESS 2026-01-01T00:00:03Z)
EOF4
waitv() { timeout 10 bash -c '. "$1"; pr=1 repo=x/y; PATH=$2:$PATH; wait_checks H "neutral lint"; echo "$verdict"' _ "$LAND" "$T/bin"; }
{ J H "$N_OK,$(ck lint IN_PROGRESS "" "")"; J H "$N_OK,$(ck lint COMPLETED SUCCESS 2026-01-01T00:00:02Z)"; } >"$T/gh.json"; rm -f "$T/gh.n"
[[ $(CHECK_WAIT=5 LAND_POLL=1 waitv) == ok ]] || fail "a pending check is not waited for"
J H "" >"$T/gh.json"; rm -f "$T/gh.n"
[[ $(MISSING_WAIT=1 LAND_POLL=1 waitv) == "missing neutral, lint" ]] && (( $(<"$T/gh.n") >= 2 )) || fail "a check that has not appeared is not waited for, then reported missing"
J H "$N_OK,$(ck lint IN_PROGRESS "" "")" >"$T/gh.json"; rm -f "$T/gh.n"
[[ $(CHECK_WAIT=1 LAND_POLL=1 waitv) == "pending lint" ]] || fail "the wait for a pending check is not bounded"

# 11. no process-table matching, every sleep says what it waits for, the worker runs without the lock descriptor
launch=("$LAND" "$HERE/land-union.py")
grep -nE '(^|[^a-z])(ps -|ps aux|pgrep|pkill|pidof|killall)|kill -0|/proc/' "${launch[@]}" && fail "land.sh matches the process table"
grep -nE '^[^#]*\bsleep\b' "${launch[@]}" | grep -v '# wait: ' && fail "a sleep in land.sh does not say what it waits for"
grep -q '9>&-' "$LAND" || fail "the worker is not started without the lock descriptor"
grep -qE '^\s*merge_locked gh pr merge' "$LAND" || fail "the merge call is not made under merge_locked"

# 12. lint_slot: with every slot held it waits, with one free it runs the command, and the tree that passed has a stamp that skips the next run
LR=$T/lintrepo; git init -q "$LR" && git -C "$LR" -c user.email=x@y -c user.name=x commit -q --allow-empty -m x
lslot() { (cd "$LR" && timeout "$1" bash -c '. "$1"; DIR=$2 LAND_LINT_SLOTS=1; shift 2; lint_slot "$@"' _ "$LAND" "$T/lint-state" "${@:2}"); }
mkdir -p "$T/lint-state"
( exec 8>"$T/lint-state/lint-slot.1"; flock 8; sleep 3 ) & hold=$!   # wait: holds the only slot while the check below runs
sleep 0.3 # wait: for the holder to take the slot
lslot 1 echo ran >/dev/null 2>&1 && fail "lint_slot ran with every slot held"
wait "$hold"
[[ $(lslot 10 echo ran) == ran ]] || fail "lint_slot did not run the command with a free slot"
[[ $(lslot 10 echo ran-again) == *cached* ]] || fail "lint_slot linted a tree that passed before"

# 13. lint-cache.sh: a pass recorded by the caller (LINT_CACHE_RECORD, after gen-docs wrote the tree) is taken by the next run without running the command, and a check never recorded runs it
LC=$T/lint-cache; mkdir -p "$LC"
(cd "$HERE/.." && SIFT_LINT_CACHE=$LC LINT_CACHE_RECORD=1 scripts/lint-cache.sh rec scripts/lint-cache.sh -- false >/dev/null) || fail "lint-cache.sh did not record a pass"
[[ $(cd "$HERE/.." && SIFT_LINT_CACHE=$LC scripts/lint-cache.sh rec scripts/lint-cache.sh -- false) == *cached* ]] || fail "lint-cache.sh ran a command whose pass the caller had recorded"
(cd "$HERE/.." && SIFT_LINT_CACHE=$LC scripts/lint-cache.sh other scripts/lint-cache.sh -- false >/dev/null 2>&1) && fail "lint-cache.sh passed a check nobody recorded or ran"

(( bad )) || echo "land selftest: a second run of one PR exits 7, different PRs go side by side in their own work trees, only the merge is under the global lock, the worker's exit code reaches the caller, TERM and KILL of the holder release its lock, nothing inherits a lock, a broken CHANGELOG is refused, the verdict on the required checks and its bounded wait, the privacy gate fails closed, land.sh has no process-table matching"
exit $bad
