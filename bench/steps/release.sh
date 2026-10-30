#!/usr/bin/env bash
# Usage: bench/bench release [--dry-run] [--rehearsal] [--stages LIST] [--budget MIN] [--no-resume] [OPTION...]   (bench/steps/release.sh, called by bench/bench)
# The one authoritative run after a release: every table the docs print, one command, one log. It is `bench/bench final --size release --docs` (bench/presets.env, steps/final.sh)
# with the checks of a run that publishes numbers, and a summary of the minutes it took against the plan:
#   store updates compression stats gap search cases duckdb generic scaling analytics
# The plan is printed first (`--dry-run` stops there): the minutes of every stage, the total against the budget (ideally 60 minutes, never more than 120: the pass stops before it starts when the
# plan is over the budget), and the preconditions of the host. A stage that finished before with the same code, settings and host is skipped (`--no-resume` measures everything), and the
# cells of the engines that are not under test are read from the cache of cells with their date and key in the table.
# The cases stage keeps its clusters on tmpfs (CASES_TMP, default /dev/shm/sift-cases, fsync off; CASES_MEM caps the server and its tmpfs at 16G, 8G was killed by the kernel): the plan says 5 GB at 1M orders and the rehearsal peaked at 1.6x the plan, so 10 GB are reserved and about 18 GB of memory must be free; the run checks both.
# The budget table with the reason of every stage is docs/BENCHMARKS.md, "Time budget of the release pass".
#
#   --rehearsal  every stage at the smoke size in a throwaway worktree of this commit, with its own work and store directories, its tables rendered and checked there, then removed: a minute
#                count per stage and the proof that the pipeline runs end to end, without touching this checkout's docs or the caches. No other option is needed.
#   --dry-run    the plan and the preconditions, nothing started
#
# On the benchmark host (HOST is the ssh name, DIR a checkout of the commit under test with the engines built by `bench/bench setup`):
#   ssh HOST 'cd DIR && git checkout --detach SHA && nohup bench/bench release > ~/release-run.log 2>&1 &'      # the run survives the connection
#   ssh HOST 'tail -f ~/release-run.log'                                                                           # follow it; the stage lines are `[time final] stage ...`
# and afterwards, in the checkout: `git diff --stat` shows the regenerated README.md and bench/results.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=release

REHEARSAL='' ARGS=() DRY=''
while (( $# )); do
  case $1 in
    --rehearsal) REHEARSAL=1 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    *) ARGS+=("$1") ;;
  esac
  shift
done

if [[ -n $REHEARSAL ]]; then
  # a detached worktree of HEAD (plus this checkout's uncommitted bench/ and scripts/ changes), its own work and store directories: nothing of this checkout's docs or caches is touched
  tmp=$(mktemp -d "${REHEARSAL_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}}/release-rehearsal.XXXXXX")
  trap 'rc=$?; (( rc )) && { echo "release: the rehearsal failed; its tree is kept for inspection: $tmp (git worktree remove --force $tmp/wt, then rm -rf)" >&2; exit "$rc"; }; git -C "$REPO" worktree remove --force "$tmp/wt" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
  git -C "$REPO" worktree add -q --detach "$tmp/wt" HEAD
  git -C "$REPO" diff --binary HEAD -- bench scripts | git -C "$tmp/wt" apply --allow-empty
  rm -f "$tmp"/wt/bench/results/cases.tsv "$tmp"/wt/bench/results/cases-time.tsv   # the smoke preset runs a few cases at 50,000 orders; a cases.tsv of 1,000,000 orders refuses a partial run at another size
  # the host is shared while it is rehearsed: own cluster directory (on disk, under the rehearsal directory: the shared tmpfs is not scratch space) and port for the cases, and no waiting for a quiet host (a rehearsal measures nothing)
  export REHEARSAL=1 BENCH_WORK=$tmp/work BENCH_STORE=$tmp/store PERF_LOCK=$tmp/perf.lock CASES_TMP=$tmp/cases CASES_PORT=56270 SIFT_QUIET_MAX=100 D_LOAD_MAX=100000
  mkdir -p "$BENCH_WORK" "$BENCH_STORE"
  T0=$SECONDS
  "$tmp/wt/bench/bench" final --size smoke --docs --budget 0 "${ARGS[@]}"
  log "rehearsal: $(( (SECONDS - T0 + 30) / 60 )) min; checking the rendered tables"
  log "rehearsal: fuzzy cells in bench/results/articles.tsv: $(grep -c $'\tfuzzy\t' "$tmp/wt/bench/results/articles.tsv" || true); table heads: $(grep -h '^| engine .*| fuzzy' "$tmp/wt/README.md" "$tmp/wt/docs/BENCHMARKS.md" | head -2 | tr '\n' ' ')"
  log "rehearsal: every stage ran and the tables render; docs changed in the throwaway tree: $(git -C "$tmp/wt" diff --stat | tail -1)"
  exit 0
fi

[[ -n $DRY || -z $(git -C "$REPO" status --porcelain -- bench scripts src docs README.md) ]] || die 'the checkout has uncommitted changes: a published run is of a commit (git status)'
SHA=$(git -C "$REPO" rev-parse --short=12 HEAD)
log "release run of $SHA ($(git -C "$REPO" describe --tags --always)), size release, $(date -u +%FT%TZ)"
T0=$SECONDS
rc=0
"$STEPS/final.sh" --size release ${DRY:+--dry-run} --docs "${ARGS[@]}" || rc=$?
[[ -n $DRY ]] || log "release run of $SHA: $(( (SECONDS - T0 + 30) / 60 )) min wall, exit $rc"
exit "$rc"
