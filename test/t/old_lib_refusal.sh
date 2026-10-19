#!/usr/bin/env bash
# The refusal path of the table write path: a sift_table that has had rows with their own versions (unfrozen groups, column format 1.4,
# on-disk format 1.3) is refused with a format error, and returns no row, by a server that loads the older library (3.0.1, built from
# the repository by scripts/old-lib-check.sh and kept in ~/.cache/sift/old-lib). Skipped when that library cannot be built here:
# no release/3.0.1 in the repository, or PostgreSQL 17 (no extension_control_path to hold two builds).
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
# the cluster of the script on a port of its own, in the directory of the other nodes (removed when it ends)
out=$(SIFT_TMP="$TAPDIR/old-lib" SIFT_PORT=$((SIFT_PORT + 15)) "$REPO/scripts/old-lib-check.sh" run 2>&1)
rc=$?
if ((rc == 77)); then
	echo "1..0 # SKIP $(grep -h '^SKIP ' <<<"$out" | head -1 | sed 's/^SKIP //')"
	exit 0
fi
while IFS= read -r line; do
	case $line in
		"PASS "*) ok "${line#PASS }" ;;
		"FAIL "*) not_ok "${line#FAIL }" ;;
		*) [[ -z $line ]] || diag "$line" ;;
	esac
done <<<"$out"
((rc == 0 || NFAIL > 0)) || not_ok "scripts/old-lib-check.sh run failed (exit $rc)"
((NTEST > 0)) || not_ok "scripts/old-lib-check.sh run produced no result"
echo "1..$NTEST"
finish
