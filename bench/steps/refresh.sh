#!/usr/bin/env bash
# Usage: bench/bench refresh [--stages LIST] [--calm [SECONDS]] [--pr BASE] [--dry-run] [--prepare] [OPTION...]   (bench/steps/refresh.sh, called by bench/bench)
# Measures the numbers of the README again at this checkout and rewrites their generated blocks: `bench final --docs` for the stages whose blocks are in
# README.md (default `updates search store`: the update, delete and VACUUM section, articles and places, the online store). Each block says the version, date,
# host and load it was measured at. Resumable: a stage that finished with the same code, settings and host is skipped (`bench final`), an interrupted run
# continues with `bench resume`; the other stages of the release pass (compression stats gap sets extras) are `bench final --docs --stages ...`.
#   --stages LIST  the stages to measure (space or comma separated; default "updates search store")
#   --calm [S]     for a timer: measure only on a calm host. Exit 75 at once when the host lock is held or the pressure stays above the quiet gate
#                  (scripts/quiet.sh, 10%) for S seconds (default 1800); without it the run waits for the lock and starts anyway
#   --pr BASE      commit bench/results, docs/img, README.md and docs/BENCHMARKS.md on the branch bench/refresh-DATE and open a pull request against BASE
#                  (a new branch from the checkout, never pushed to BASE, never merged here); needs a clean tree and `gh`
#   --dry-run      print the plan of `bench final` and start nothing
#   --prepare      build the image of this checkout and generate the data of the update, delete and VACUUM section (nice, no host lock, nothing timed), so that the
#                  measuring run starts at once; the image is rebuilt when the sources change, the base layers and the data stay
# Every other option goes to `bench final` (--size standard is the published size, --budget MIN: default 90, --remeasure-all, --engines ...).
# Environment: SIFT_BENCH_HOST_LOCK=FILE  a lock shared with the other benchmarks of the host, held for the whole run.
set -euo pipefail
. "$(dirname "$0")/lib.sh"
DIE_TAG=refresh

STAGES='updates search store' CALM='' PR='' DRY='' PREP='' FWD=() BUDGET=90
while (( $# )); do
  case $1 in
    --stages) STAGES=${2//,/ }; shift ;;
    --calm) CALM=1800; [[ ${2:-} != [0-9]* ]] || { CALM=$2; shift; } ;;
    --pr) PR=${2:?--pr BASE}; shift ;;
    --dry-run) DRY=1; FWD+=("$1") ;;
    --prepare) PREP=1 ;;
    --budget) BUDGET=$2; shift ;;
    *) FWD+=("$1") ;;
  esac
  shift
done
cd "$REPO"
[[ -z $PREP ]] || exec nice -n 10 "$BENCH/bench" store prepare --sizes 200000,1000000
[[ -z $PR || -z $(git status --porcelain) ]] || die "--pr needs a clean tree: the pull request holds the measured files and nothing else"

if [[ -n ${SIFT_BENCH_HOST_LOCK:-} && -z $DRY ]]; then
  exec 7>>"$SIFT_BENCH_HOST_LOCK"
  if [[ -n $CALM ]]; then flock -n 7 || { log "the host lock is held: not measuring"; exit 75; }
  else flock -n 7 || { log "waiting for the host lock (another benchmark is timing)"; flock 7; }; fi
fi
if [[ -n $CALM && -z $DRY ]]; then
  quiet_wait --timeout "$CALM" || { log "the host is not calm ($(quiet_report)): not measuring"; exit 75; }
fi

version=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" pg_sift.control) sha=$(git rev-parse --short=12 HEAD)
log "refresh: stages ${STAGES} at pg_sift $version ($sha), $(quiet_report || true)"
"$BENCH/bench" final --size standard --docs --stages "${STAGES// /,}" --budget "$BUDGET" ${FWD[@]+"${FWD[@]}"}
[[ -z $DRY ]] || exit 0

[[ -n $PR ]] || { log "refresh: git diff shows README.md, docs/img and bench/results"; exit 0; }
branch=bench/refresh-$(date -u +%F)
git switch -q -c "$branch"
git add -- bench/results docs/img README.md docs/BENCHMARKS.md docs/BENCH-GAP.md
git commit -q -m "bench: refresh: the README and docs numbers measured at pg_sift $version ($sha), $(date -u +%F)"
git push -q -u origin "$branch"
gh pr create --base "$PR" --head "$branch" --title "bench: refresh: the README and docs numbers at pg_sift $version" \
  --body "Measured by \`bench/bench refresh\` (stages: ${STAGES}) at pg_sift $version, $(date -u +%F). Each regenerated block says its version, date, host and load. Not merged automatically."
