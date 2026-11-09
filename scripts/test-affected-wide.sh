#!/usr/bin/env bash
# `scripts/ci.sh affected` on a wide diff: more than SIFT_CI_WIDE_DIFF changed files (a pull request of a whole release line, 1,841 files) picks every job at once, as `all` does, and
# the regex of its files is not built (146 s against 1 s on a quiet machine; the `changes` job of a runner stops at 5 minutes). A narrow diff keeps its own selection.
# Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
SRC=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_ALLOW_LOCAL_CI=1
bad=0
fail() { echo "FAIL: $*"; bad=1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/wide-scratch.XXXXXX")
trap 'git -C "$SRC" worktree remove --force "$T/wt-wide" >/dev/null 2>&1; rm -rf "$T"' EXIT
git -C "$SRC" worktree add -q --detach "$T/wt-wide" HEAD || { echo "FAIL: no work tree of HEAD"; exit 1; }
REPO=$T/wt-wide
for i in 1 2 3; do echo 'SELECT 1;' >"$REPO/test/sql/zz_wide_probe$i.sql"; done   # three changed files that a row of scripts/areas.tsv covers
pick() { "$REPO/scripts/ci.sh" affected --list HEAD 2>&1; }
all=$("$REPO/scripts/ci.sh" affected --list all 2>&1)
narrow=$(pick)
[[ $narrow != "$all" ]] || fail "a diff of three files picked every job"
wide=$(SIFT_CI_WIDE_DIFF=2 pick)
[[ $wide == "$all" ]] || fail "a diff over the threshold did not pick what 'all' picks: $(head -c 200 <<<"$wide")"
[[ $(SIFT_CI_WIDE_DIFF=3 pick) == "$narrow" ]] || fail "a diff at the threshold lost its own selection"
exit "$bad"
