#!/usr/bin/env bash
# The refusal path of the table write path and the upgrade path from 3.0.1: a sift_table that has had rows with their own versions (unfrozen groups, column format 1.4,
# on-disk format 1.3) is refused with a format error, and returns no row, by a server that loads the older library (3.0.1, built from
# the repository by scripts/old-lib-check.sh and kept in ~/.cache/sift/old-lib). Skipped when that library cannot be built here:
# no v3.0.1 in the repository, or PostgreSQL 17 (no extension_control_path to hold two builds).
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
# A cold released build must own its install, even inside a lane sharing the current root.
# Reuse that build for the refusal and upgrade checks below.
export SIFT_OLD_DIR="$TAPDIR/old-lib-cache"
parent="$TAPDIR/old-lib-parent"
rm -rf "$SIFT_OLD_DIR" "$parent"
mkdir -p "$parent$(pg_config --pkglibdir)" "$parent$(pg_config --sharedir)/extension"
files=("$parent/.flavor" "$parent$(pg_config --pkglibdir)/pg_sift.so" "$parent$(pg_config --sharedir)/extension/pg_sift.control")
for file in "${files[@]}"; do echo current-testing-install >"$file"; done
before=$(sha256sum "${files[@]}")
root=$(SIFT_ROOT="$parent" "$REPO/scripts/old-lib-check.sh" build)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP older library unavailable'; exit 0; fi
((rc == 0)) || bail 'cold released build failed'
check 'released build preserves the inherited install' test "$(sha256sum "${files[@]}")" == "$before"
check 'released build returns its private root' test "$root" == "$SIFT_OLD_DIR/root"
check 'released root contains its flavor' test -f "$root/.flavor"
check 'released root contains its library' test -f "$root$(pg_config --pkglibdir)/pg_sift.so"
check 'released root contains its control file' test -f "$root$(pg_config --sharedir)/extension/pg_sift.control"
((NFAIL == 0)) || bail 'released install escaped its root'
rm -rf "$parent"

# the cluster of the script on a port of its own, in the directory of the other nodes (removed when it ends)
out=$(SIFT_TMP="$TAPDIR/old-lib" SIFT_PORT=$((SIFT_PORT + 15)) "$REPO/scripts/old-lib-check.sh" run 2>&1)
rc=$?
if ((rc != 77)); then # and the other way: the groups the older library tags under pressure are read, frozen or dropped
	out+=$'\n'$(SIFT_TMP="$TAPDIR/old-lib-up" SIFT_PORT=$((SIFT_PORT + 16)) "$REPO/scripts/old-lib-check.sh" upgrade 2>&1)
fi
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
