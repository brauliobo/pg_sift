#!/usr/bin/env bash
# `scripts/ci.sh affected` in a shared tree: an untracked directory or file that no row of scripts/areas.tsv covers (another lane's worktree or log) leaves the selection as it was,
# and an untracked file a row covers still counts. Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_ALLOW_LOCAL_CI=1
bad=0
fail() { echo "FAIL: $*"; bad=1; }
pick() { "$REPO/scripts/ci.sh" affected --list HEAD 2>&1; }
scratch=$(mktemp -d "$REPO/untracked-scratch.XXXXXX"); trap 'rm -rf "$scratch" "$REPO/test/sql/zz_untracked_probe.sql"' EXIT
want=$(pick)
echo x >"$scratch/note.txt"; git init -q "$scratch/nested"
[[ $(pick) == "$want" ]] || fail "an untracked directory changed the selection"
echo 'SELECT 1;' >"$REPO/test/sql/zz_untracked_probe.sql"
[[ $(pick) != "$want" ]] || fail "an untracked test file was left out of the selection"
exit "$bad"
