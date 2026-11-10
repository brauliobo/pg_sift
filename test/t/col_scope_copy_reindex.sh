#!/usr/bin/env bash
# REINDEX and REINDEX CONCURRENTLY of the indexes of a scope copy (the sift_col index of a plain copy and of a partitioned one, the unique index the triggers look rows up by, the
# table): every key answers by the sift plan as before and as the join does, sift.scope_verify finds nothing, the triggers keep the copy exact afterwards, and a loop of
# REINDEX CONCURRENTLY under four writers that change facts and pairs (transactions, data-modifying CTEs) leaves the copies exact.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_scope.bash"

SECS=$(knob SIFT_SCOPE_REINDEX_SECS 8 4)
plan 11
node_init n 52 "autovacuum = off
sift.col_auto_merge = off"
node_start n || bail "start failed"
D=$(dir n)
scope_setup n || bail "setup failed"
BASE_FLAT=$(scope_answers n cflat)
BASE_PART=$(scope_answers n cpart)
check "the copies are exact" scope_exact n

same_answers() { [[ $(scope_answers n cflat) == "$BASE_FLAT" && $(scope_answers n cpart) == "$BASE_PART" ]] && scope_exact n; }
reindex() { sql n "$@" >/dev/null && same_answers; }
check "REINDEX INDEX of the sift_col index keeps the answers" reindex "REINDEX INDEX cflat_col"
check "REINDEX INDEX CONCURRENTLY of the sift_col index keeps the answers" reindex "REINDEX INDEX CONCURRENTLY cflat_col"
check "REINDEX INDEX of the partitioned copy's index keeps the answers" reindex "REINDEX INDEX cpart_col"
check "REINDEX INDEX CONCURRENTLY of the partitioned copy's index keeps the answers" reindex "REINDEX INDEX CONCURRENTLY cpart_col"
check "REINDEX of the unique index the triggers use, and of the tables, keeps the answers" reindex "REINDEX INDEX CONCURRENTLY cflat_maint" "REINDEX TABLE cflat" "REINDEX TABLE CONCURRENTLY cpart"

edits() { sql n "INSERT INTO facts VALUES (800001, 45, now(), 1, 5, 'r')" "INSERT INTO pairs VALUES (45, 8), (44, 8) ON CONFLICT DO NOTHING" "UPDATE facts SET amount = amount + 1 WHERE id % 7 = 0" \
	"DELETE FROM pairs WHERE scope_id = 3 AND dim_a_id % 2 = 0" "DELETE FROM facts WHERE id % 13 = 0" >/dev/null && scope_exact n; }
check "changes after the reindexes keep the copies exact" edits
check "the sift index of the copy verifies" test -z "$(sql n "SELECT sift.col_verify('cflat_col', true)" 2>&1)"

# a loop of REINDEX CONCURRENTLY under writers: the triggers of the sift index move to the new index, the triggers of the copy go on
loop() {
	local end=$((SECONDS + SECS)) rounds=0
	while ((SECONDS < end)); do sql n "REINDEX INDEX CONCURRENTLY cflat_col" "REINDEX INDEX CONCURRENTLY cpart_col" >/dev/null 2>&1 && rounds=$((rounds + 1)); done
	echo "$rounds" >"$D/rounds"
}
FILES=($(scope_files n) --max-tries=20)
pgbench_bg n 4 "$((SECS + 2))" "${FILES[@]}"
loop
wait "$PGBENCH_PID"
check "REINDEX CONCURRENTLY of both sift indexes under 4 writers: $(cat "$D/rounds") rounds in ${SECS} s" test "$(cat "$D/rounds")" -ge 1
check "... the copies are exact" scope_exact n
check "... and no invalid index is left" test "$(sql n "SELECT count(*) FROM pg_index WHERE NOT indisvalid")" == 0
finish
