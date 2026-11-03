#!/usr/bin/env bash
# The instruction budgets (scripts/instr-budget*.sh) run a single-user backend on a data directory of their own: scripts/instr-budget-lib.sh made it $SIFT_TMP/data and
# removed it before and after, which deleted the cluster a developer kept there (templates of 1,000,000-row tables). They now make a directory with mktemp under SIFT_TMP,
# share the install of SIFT_ROOT, and remove only that directory.
. "$(dirname "$0")/lib.bash"

plan 5
REPO=$(cd "$(dirname "$0")/../.." && pwd)
KEEP=$TAPDIR/ib_keep
STUB=$TAPDIR/ib_stub
mkdir -p "$KEEP/data" "$KEEP/sock" "$STUB"
echo kept >"$KEEP/data/PG_VERSION"
printf '#!/bin/sh\nexit 0\n' >"$STUB/valgrind"
chmod +x "$STUB/valgrind"
trap 'rm -rf "$KEEP" "$STUB"; cleanup' EXIT

out=$(cd "$REPO" && PATH=$STUB:$PATH SIFT_TMP=$KEEP SIFT_ROOT=$ROOT SIFT_PORT=$((SIFT_PORT > 56200 ? SIFT_PORT - 60 : SIFT_PORT + 60)) bash -c '
	. scripts/instr-budget-lib.sh
	ib_begin instr-budget-test 56199 <<<"" >/dev/null || exit 1
	echo "$SIFT_TMP"
	test -d "$SIFT_TMP/data" || exit 2
	ib_end') || bail "the budget harness failed ($out)"
own=$(tail -n 1 <<<"$out")

check "the harness runs in a directory of its own under SIFT_TMP" test "$(dirname "$own")" == "$KEEP" -a "$own" != "$KEEP"
check "the data directory that was in SIFT_TMP is not touched" test "$(cat "$KEEP/data/PG_VERSION")" == kept
check "the socket directory that was in SIFT_TMP is not touched" test -d "$KEEP/sock"
check "its own directory is removed at the end" test ! -e "$own"
check "nothing else is left in SIFT_TMP" test "$(ls "$KEEP" | sort | tr '\n' ' ')" == "data sock "
finish
