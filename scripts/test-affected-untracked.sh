#!/usr/bin/env bash
# `scripts/ci.sh affected` in a shared tree: an untracked directory or file that no row of scripts/areas.tsv covers (another lane's worktree or log) leaves the selection as it was,
# and an untracked file a row covers still counts. Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
SRC=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_ALLOW_LOCAL_CI=1
bad=0
fail() { echo "FAIL: $*"; bad=1; }
# its own work tree of HEAD: the probe file must not be seen by the other checks of scripts/lint.sh that read the tree while this one runs (plan counts test/sql)
T=$(mktemp -d "${TMPDIR:-/tmp}/untracked-scratch.XXXXXX")
trap 'git -C "$SRC" worktree remove --force "$T/wt" >/dev/null 2>&1; rm -rf "$T"' EXIT
git -C "$SRC" worktree add -q --detach "$T/wt" HEAD || { echo "FAIL: no work tree of HEAD"; exit 1; }
REPO=$T/wt
pick() { "$REPO/scripts/ci.sh" affected --list HEAD 2>&1; }
scratch=$(mktemp -d "$REPO/untracked-scratch.XXXXXX")
want=$(pick)
echo x >"$scratch/note.txt"; git init -q "$scratch/nested"
[[ $(pick) == "$want" ]] || fail "an untracked directory changed the selection"
echo 'SELECT 1;' >"$REPO/test/sql/zz_untracked_probe.sql"
[[ $(pick) != "$want" ]] || fail "an untracked test file was left out of the selection"
exit "$bad"
