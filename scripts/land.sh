#!/usr/bin/env bash
# Land one pull request: merge the tip of its base into the branch, run the gates, push when needed, wait for the required checks, merge.
#
#   scripts/land.sh PR            land PR; landings of different PRs run side by side, a second run of the same PR exits 7 at once
#   scripts/land.sh --dry-run PR  the same without a push, a merge or a write on GitHub, and without a lock: prints the plan
#   scripts/land.sh --selftest    the locks, the exit codes, the cleanup and the privacy gate with fake work (no GitHub, no network)
#
# Exit codes: 0 merged (or already merged); 1 the required checks are red; 2 SOURCE CONFLICT (a conflict beyond the routine ones below: the owner merges the tip);
# 3 the merge commit failed; 4 a gate is red (lint, generated files, bench cases, privacy, focus table); 5 the PR deletes files (ALLOW_DELETE=1 after checking they are its
# own); 6 the PR is closed; 7 another run is landing this same PR; 75 try again (checks still pending or missing, the tip or the branch moved, GitHub unreachable): run it again.
#
# Steps: (1) when the PR head does not hold the tip, merge the tip into the branch. A conflict only in what both sides add to (CHANGELOG, bench/cases.tsv, bench/results/*.tsv,
# the list lines of Makefile, scripts/focus.tsv) or in a generated file is resolved; any other is exit 2. The CHANGELOG is
# checked after every merge (no marker left, no line of Unreleased twice, no entry moved out of Unreleased or lost): a broken one is exit 2 and nothing is pushed. If the tip
# moved while the gates ran, it is merged again (three rounds; the gates run once, the checks of the pushed head are the rest). (2) After a merge: scripts/lint.sh. (3) The privacy gate on
# the added lines, the commit messages and the PR title and body, and the focus-table row of every new test/sql file. (4) A push, always after a merge of the tip: GitHub
# squashes the PR diff onto the tip, and a branch that is behind it conflicts there even when the local merge was clean. (5) The required checks of the new head (the branch
# protection list, else neutral and lint) are waited for: up to CHECK_WAIT seconds (600) while pending, MISSING_WAIT (300) while they have not appeared. (6) The tip is fetched
# again; if it moved while the checks ran, GitHub must still call the PR CLEAN or UNSTABLE, else exit 75. (7) The merge, `gh pr merge --match-head-commit` of the commit that was
# checked: rebase, or squash (PR title as subject, PR body) when the branch holds a merge commit, which GitHub refuses to rebase. Never a force-push. The line in MERGED.md says
# `merge-up` and `changelog-conflict` when the landing needed them, so they can be counted.
#
# Two kernel locks, in this order. pr-N.lock, taken first and never waited for: a second run for the same PR exits 7 and touches nothing of the first (its work tree is its
# own mktemp directory, removed by that path alone). merge.lock, taken around the merge call alone: landings of different PRs prepare side by side and merge one at a time. The
# kernel drops a lock when its holder dies, however it ends, and the worker runs without the pr-N.lock descriptor, so nothing that outlives a landing keeps it. pr-N.info
# beside the lock (pid, start) is for people. TERM and INT stop the worker and remove the work tree. A KILL cannot clean up: the worker runs to its end. During a switch from a
# legacy driver, LAND_LEGACY_LOCK names its merge lock, taken after merge.lock around the same call while the file exists.
#
# State and settings, all outside git: LAND_DIR (default $XDG_STATE_HOME/pg_sift/land: the locks, the merged log MERGED.md, privacy.deny), LAND_DENY (the private deny list of the
# privacy gate: one extended regular expression per line, # comments; the gate fails closed without it), LAND_CLONE (the clone to fetch into, default this repository),
# LAND_REPO (OWNER/NAME, default the remote of the clone). The script is read whole before it runs, so a checkout of another version under a landing does not change it.
set -u
SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
HERE=${SELF%/*}
DIR=${LAND_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/pg_sift/land}
DENY=${LAND_DENY:-$DIR/privacy.deny}
CLONE=${LAND_CLONE:-$(git -C "$HERE" rev-parse --show-toplevel)}
dry=${LAND_DRY:-} child='' holding='' WORK='' PR='' CL=''

# ---- the privacy gate: private names do not enter the public repository (the list is private data and never committed)

# deny_patterns FILE: the usable lines of the deny list into FILE; 2 when the list is missing, empty or not a regular expression: the gate fails closed
deny_patterns() {
	[[ -r $DENY ]] && grep -v -e '^#' -e '^[[:space:]]*$' "$DENY" >"$1"
	[[ -s $1 ]] && { grep -q -i -E -f "$1" /dev/null; (( $? <= 1 )); } && return 0
	echo "land: no usable deny list at $DENY (LAND_DENY): refusing to merge unchecked" >&2
	return 2
}
# privacy_range BASE HEAD: 0 clean, 1 a hit (printed), 2 no deny list; the lines HEAD adds over BASE and the messages of its commits
privacy_range() {
	local pat rc=0
	pat=$(mktemp); deny_patterns "$pat" || { rm -f "$pat"; return 2; }
	{ git -C "$CLONE" diff -U0 "$1...$2" | grep -E '^(\+\+\+ |\+[^+])' | awk '/^\+\+\+ /{f=$2;next}{print f": "$0}'
	  git -C "$CLONE" log --format='%h %s%n%b' "$1..$2"; } | grep -i -E -f "$pat" | cut -c1-200 | grep . && rc=1
	rm -f "$pat"; return $rc
}
# privacy_text: the text on stdin (a PR title and body), same results
privacy_text() {
	local pat rc=0
	pat=$(mktemp); deny_patterns "$pat" || { rm -f "$pat"; return 2; }
	grep -i -E -f "$pat" | cut -c1-160 | grep . && rc=1
	rm -f "$pat"; return $rc
}

# ---- the worker

# merge_locked CMD...: CMD under merge.lock, then the lock of a legacy driver while its file exists (so an old and a new driver never merge at once): the one global lock,
# held for the merge call alone. `flock -o` closes the descriptor for CMD, which therefore cannot leak it; no lock is held while waiting for checks or sleeping.
merge_locked() {
	local w=${LAND_LOCK_WAIT:-3600}
	[[ -z ${LAND_LEGACY_LOCK:-} || ! -e $LAND_LEGACY_LOCK ]] || set -- flock -w "$w" -E 75 -o "$LAND_LEGACY_LOCK" "$@"
	flock -w "$w" -E 75 -o "$DIR/merge.lock" "$@"
}

# resolve_conflicts BASE MB HEAD: the routine conflicts of the merge in progress, by the rules above; 2 on any other
resolve_conflicts() {
	local f l
	while IFS= read -r f; do
		case $f in
			CHANGELOG.md|bench/cases.tsv|bench/results/*.tsv)
				sed -i '/^<<<<<<< /d;/^=======$/d;/^>>>>>>> /d' "$f"; git add "$f"; [[ $f != CHANGELOG.md ]] || CL=1 ;;
			Makefile|scripts/focus.tsv)
				# both sides only add list lines (TESTING_REGRESS += ..., focus rows): keep both once; anything else is the owner's
				if python3 "$HERE/land-union.py" "$f"; then git add "$f"; else echo "SOURCE CONFLICT in $f: owner must merge the tip"; git merge --abort; return 2; fi ;;
			README.md|bench/results/*|docs/OPTIONS.md|docs/LIMITS.md|docs/REFERENCE.md|docs/COL-TAM.md|docs/img/*|bench/*)
				git checkout -q --theirs -- "$f" 2>/dev/null; git add "$f" ;;
			*) echo "SOURCE CONFLICT in $f: owner must merge the tip"; git merge --abort; return 2 ;;
		esac
	done < <(git diff --name-only --diff-filter=U)
	return 0
}

# checks_verdict HEAD REQUIRED: ok | pending <names> | red <names> | missing <names>, for the checks of the commit HEAD, which must still be the PR head; of several runs of
# one check (a re-run) the latest decides
checks_verdict() {
	gh pr view "$pr" --repo "$repo" --json headRefOid,statusCheckRollup | jq -r --arg req "$2" --arg head "$1" '
		if .headRefOid != $head then "pending the new head of the PR" else
		.statusCheckRollup as $c | ($req | split(" ")) as $names
		| [ $names[] as $n | ([$c[] | select(.name == $n)]) as $r
			| ($r | sort_by(if .status == "COMPLETED" then .completedAt else "9999" end) | last) as $l
			| if ($r | length) == 0 then "missing \($n)"
			  elif $l.status != "COMPLETED" then "pending \($n)"
			  elif ($l.conclusion | IN("FAILURE","CANCELLED","TIMED_OUT","STARTUP_FAILURE","ACTION_REQUIRED","STALE")) then "red \($n)"
			  elif $l.conclusion != "SUCCESS" and $n == "neutral" then "red \($n) (no success)"
			  else empty end ]
		| if length == 0 then "ok" else (map(split(" ")[0]) | if index("red") then "red" elif index("pending") then "pending" else "missing" end) + " " + (map(split(" ")[1:] | join(" ")) | join(", ")) end end' || echo "pending GitHub unreachable"
}

# wait_checks HEAD REQUIRED: sets verdict; a pending check is waited for up to CHECK_WAIT seconds (600), one that has not appeared yet (GitHub starts the checks of a pushed
# head late) up to MISSING_WAIT (300), every LAND_POLL seconds (15); no lock is held
wait_checks() {
	local waited=0 limit poll=${LAND_POLL:-15}
	while :; do
		verdict=$(checks_verdict "$1" "$2")
		case $verdict in pending*) limit=${CHECK_WAIT:-600} ;; missing*) limit=${MISSING_WAIT:-300} ;; *) return 0 ;; esac
		(( waited < limit )) || return 0
		sleep "$poll" # wait: the required checks of this PR, and GitHub to show a pushed head
		waited=$((waited + poll))
	done
}

# fetch_tip: origin/BASE of the clone (bounded; runs side by side with other landings, so a held ref lock is waited for)
fetch_tip() { timeout 120 git -C "$CLONE" -c core.filesRefLockTimeout=10000 fetch -q origin "+refs/heads/$base:refs/remotes/origin/$base"; }

# changelog_check: in the work tree, after a merge of the tip: the CHANGELOG.md must not be broken (see land-union.py); needs mb and before (the merge base, the commit merged into)
changelog_check() {
	git show "$mb:CHANGELOG.md" >"$WORK/cl.mb" && git show "$before:CHANGELOG.md" >"$WORK/cl.head" && git show "origin/$base:CHANGELOG.md" >"$WORK/cl.tip" || return 0
	python3 "$HERE/land-union.py" --changelog CHANGELOG.md "$WORK/cl.mb" "$WORK/cl.head" "$WORK/cl.tip" || { echo "SOURCE CONFLICT in CHANGELOG.md: the merge of the tip left it broken (above); nothing is pushed, owner must merge the tip"; return 2; }
}

# red MESSAGE [LOG]: a gate is red
red() { echo "$1"; [[ -z ${2:-} ]] || tail -n 3 "$2"; exit 4; }

worker() {
	pr=$1 repo=${LAND_REPO:-$(cd "$CLONE" && gh repo view --json nameWithOwner -q .nameWithOwner)}
	local meta_hits cand state base br head mb before round push=0 W=$WORK/wt dels prot required verdict meta prc tests rows missing merges how opts st sha ms f note=''
	[[ -n $repo ]] || { echo "cannot tell the GitHub repository (LAND_REPO)"; exit 75; }
	cd "$CLONE" || exit 1
	IFS=$'\t' read -r state base br < <(gh pr view "$pr" --repo "$repo" --json state,baseRefName,headRefName -q '[.state,.baseRefName,.headRefName]|@tsv') && [[ -n $br ]] || { echo "cannot read #$pr from GitHub"; exit 75; }
	case $state in MERGED) echo "#$pr is already merged"; exit 0 ;; CLOSED) echo "REFUSED #$pr: the PR is closed"; exit 6 ;; esac
	dels=$(gh pr view "$pr" --repo "$repo" --json files -q '[.files[]|select(.changeType=="DELETED")|.path]|join(" ")') || exit 75
	[[ -z $dels || -n ${ALLOW_DELETE:-} ]] || { echo "REFUSED #$pr: the PR deletes files ($dels); check they are its own, then ALLOW_DELETE=1"; exit 5; }
	fetch_tip && timeout 120 git fetch -q origin "+refs/pull/$pr/head:refs/land/pr$pr" || { echo "cannot fetch #$pr or $base"; exit 75; }
	head=$(git rev-parse "refs/land/pr$pr"); git update-ref -d "refs/land/pr$pr"

	cand=$head   # the commit the gates look at; the one that is checked and merged is the PR head, or what this run pushes
	for round in 1 2 3; do
		git merge-base --is-ancestor "origin/$base" "$cand" && break
		# the PR does not hold the tip, or the tip moved while the gates ran: merge it in, in a work tree of this run, and push the result. GitHub squashes the PR diff onto
		# the tip, and a branch that is behind it conflicts there even when the local merge was clean (every PR adds its CHANGELOG entry at the same spot)
		push=1
		[[ -d $W ]] || git worktree add -q --detach "$W" "$cand" || exit 1
		cd "$W" || exit 1
		before=$(git rev-parse HEAD); mb=$(git merge-base "origin/$base" "$before")
		if ! git merge --no-ff -q -m "merge $base into $br" "origin/$base" >/dev/null 2>&1; then
			resolve_conflicts "$base" "$mb" "$before" || exit $?
			git add -A
			git commit -q --no-edit || { echo "commit failed"; exit 3; }
		fi
		changelog_check || exit $?
		if ! git diff --quiet; then git add -A; git commit -q -m "docs: regenerate after the merge of the tip"; fi
		echo "round $round, src patch-id: PR $(git diff "$mb" "$before" -- src | git patch-id --stable | cut -c1-12), merged-up $(git diff "origin/$base" HEAD -- src | git patch-id --stable | cut -c1-12)"
		(( round == 1 )) || { cand=$(git rev-parse HEAD); cd "$CLONE" || exit 1; fetch_tip || { echo "cannot fetch $base"; exit 75; }; continue; }   # again: the checks of the pushed head are the gates
		scripts/lint.sh >"$WORK/lint.log" 2>&1 || { cp "$WORK/lint.log" "$DIR/lint-$pr.log"; echo "LINT RED (log: $DIR/lint-$pr.log)"; grep -E '^(FAIL|[A-Za-z-]+: .*(not|missing|differ))' "$WORK/lint.log" | head -n 5 | cut -c1-200; exit 4; }
		cand=$(git rev-parse HEAD); cd "$CLONE" || exit 1
		fetch_tip || { echo "cannot fetch $base"; exit 75; }
	done
	(( push )) || echo "up to date"

	# privacy and focus table on the commit to merge: before any push
	meta=$(gh pr view "$pr" --repo "$repo" --json title,body -q '.title + "\n" + .body') || { echo "cannot read the text of #$pr"; exit 75; }
	meta_hits=$(privacy_range "origin/$base" "$cand" 2>&1); prc=$?
	[[ $prc == 0 ]] || { head -n 5 <<<"$meta_hits"; red "privacy red (rc $prc)"; }
	privacy_text <<<"$meta" >/dev/null 2>&1 || red "privacy red in the title or body of #$pr"
	if git cat-file -e "$cand:scripts/focus.tsv" 2>/dev/null; then
		tests=$(git diff --name-only --diff-filter=A "$(git merge-base "origin/$base" "$cand")" "$cand" -- test/sql | sed 's#.*/##; s/\.sql$//' | sort)
		rows=$(git show "$cand:scripts/focus.tsv" | cut -f1 | grep -v '^[#=]' | sort)
		missing=$(comm -23 <(echo "$tests" | grep .) <(echo "$rows") | tr '\n' ' ')
		[[ -z $missing ]] || red "no row in scripts/focus.tsv for: $missing(run scripts/focus-gen.sh NAME... and commit scripts/focus.tsv)"
	fi

	required=
	if prot=$(gh api "repos/$repo/branches/$base/protection/required_status_checks" --jq '.contexts[]' 2>/dev/null); then required=$(sort -u <<<"$prot" | tr '\n' ' '); required=${required% }; fi
	required=${required:-neutral lint}
	if [[ -n $dry ]]; then
		if (( push )); then echo "dry run: would push the merge-up to $br, then wait for the required checks ($required)"; else echo "dry run: required checks ($required): $(checks_verdict "$head" "$required")"; fi
		merges=$(gh api "repos/$repo/pulls/$pr/commits" --paginate --jq '[.[] | select(.parents | length > 1)] | length' | awk '{ n += $1 } END { print n + 0 }')
		echo "dry run: would merge by $( ((merges > 0)) && echo squash || echo rebase) ($merges merge commit(s) in the PR range)"; exit 0
	fi
	if (( push )); then
		git push -q origin "$cand:refs/heads/$br" || { echo "push of $br rejected: the branch moved"; exit 75; }
		head=$cand
	fi
	ms=${base#integration/}; ms=${ms%%-*}; [[ $ms =~ ^3\.[0-9]+(\.[0-9]+)?$ ]] && gh pr edit "$pr" --repo "$repo" --milestone "$ms" >/dev/null 2>&1  # every PR carries the milestone of its base

	wait_checks "$head" "$required"
	echo "required checks ($required): $verdict"
	if [[ $verdict != ok ]]; then
		echo "REFUSED #$pr: required checks not green ($verdict)"
		case $verdict in red*) exit 1 ;; *) exit 75 ;; esac
	fi

	# the tip may have moved while the checks ran: merge only when GitHub still calls the PR mergeable, else the owner runs this again (a new merge-up)
	fetch_tip || { echo "cannot fetch $base"; exit 75; }
	if ! git merge-base --is-ancestor "origin/$base" "$head"; then
		st=$(gh pr view "$pr" --repo "$repo" --json mergeStateStatus -q .mergeStateStatus)
		[[ $st == CLEAN || $st == UNSTABLE ]] || { echo "the tip moved while the checks ran and GitHub calls #$pr $st: run land.sh again"; exit 75; }
	fi

	merges=$(gh api "repos/$repo/pulls/$pr/commits" --paginate --jq '[.[] | select(.parents | length > 1)] | length' | awk '{ n += $1 } END { print n + 0 }')
	if (( merges > 0 )); then
		how=squash; opts=(--squash --subject "$(gh pr view "$pr" --repo "$repo" --json title -q .title)" --body "$(gh pr view "$pr" --repo "$repo" --json body -q .body)")
	else
		how=rebase opts=(--rebase)
	fi
	echo "merge mode: $how ($merges merge commit(s) in the PR range)"
	for f in 1 2; do
		merge_locked gh pr merge "$pr" --repo "$repo" --match-head-commit "$head" "${opts[@]}" --delete-branch >/dev/null 2>"$WORK/merge.err"
		st=$(gh pr view "$pr" --repo "$repo" --json state -q .state)
		[[ $st == MERGED ]] && break
		echo "merge attempt $f state=$st: $(head -c 200 "$WORK/merge.err")"
		sleep 20 # wait: GitHub to settle after a refused merge
	done
	[[ $st == MERGED ]] || { echo "NOT MERGED #$pr (state $st)"; exit 75; }
	sha=$(gh pr view "$pr" --repo "$repo" --json mergeCommit -q .mergeCommit.oid)
	(( push )) && note+=" merge-up"; [[ -z ${CL:-} ]] || note+=" changelog-conflict"   # counted from the log: how many landings needed a new merge of the tip
	echo "#$pr $sha $(date -u +%H:%MZ) -> $base$note" >> "${LAND_LOG:-$DIR/MERGED.md}"
	echo "merged #$pr $sha -> $base ($how)"
}

# ---- the holder: the lock of this PR, a worker without it, the cleanup

# shellcheck disable=SC2329 # run by the trap
cleanup() {
	[[ -z $WORK ]] || { git -C "$CLONE" worktree remove --force "$WORK/wt" 2>/dev/null; rm -rf "$WORK"; }
	[[ -z $holding ]] || rm -f "$DIR/pr-$PR.info"
}

# shellcheck disable=SC2329
stop() {
	trap '' TERM INT HUP
	[[ -z $child ]] || { kill -TERM -- "-$child" 2>/dev/null || kill -TERM "$child" 2>/dev/null; wait "$child" 2>/dev/null; }
	exit "$1"
}

holder() {
	local pr=$1 rc
	[[ $pr =~ ^[0-9]+$ ]] || { echo "land: '$pr' is not a pull request number" >&2; exit 2; }
	PR=$pr
	mkdir -p "$DIR" || exit 1
	if [[ -z $dry ]]; then
		exec 9>"$DIR/pr-$pr.lock" || exit 1
		flock -n 9 || { echo "land: #$pr is already running (informational, from pr-$pr.info: $(tr '\n' ' ' <"$DIR/pr-$pr.info" 2>/dev/null)); not touching it"; exit 7; }
		holding=1
		printf 'pid=%s\nstarted=%s\n' "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$DIR/pr-$pr.info.$$" && mv -f "$DIR/pr-$pr.info.$$" "$DIR/pr-$pr.info"
	fi
	WORK=$(mktemp -d "${TMPDIR:-/tmp}/land.XXXXXX") || exit 1
	trap cleanup EXIT; trap 'stop 143' TERM; trap 'stop 130' INT; trap 'stop 129' HUP
	# own process group, no lock descriptor: `wait` returns at once for a trapped signal, and a process that outlives the landing cannot keep the lock
	LAND_RUN=1 LAND_WORK=$WORK LAND_DRY=$dry LAND_DIR=$DIR LAND_CLONE=$CLONE setsid "${LAND_WORKER:-$SELF}" "$pr" 9>&- & child=$!
	wait "$child"; rc=$?
	child=''
	exit $rc
}

main() {
	case ${1:-} in
		--selftest) exec "$HERE/land-selftest.sh" ;;
		--dry-run) dry=1; shift ;;
	esac
	(( $# == 1 )) || { sed -n '3,5p' "$SELF" | cut -c3-; exit 2; }
	if [[ -n ${LAND_RUN:-} ]]; then WORK=$LAND_WORK; worker "$1"; else holder "$1"; fi
}
# the selftest sources this file for the privacy functions and runs nothing
[[ ${BASH_SOURCE[0]} != "$0" ]] || { main "$@"; exit $?; }
