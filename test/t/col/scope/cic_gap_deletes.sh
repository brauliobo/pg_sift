#!/usr/bin/env bash
# Rows that a writer deletes or updates while CREATE INDEX CONCURRENTLY of a second scope index runs are not in the answers the index gives a snapshot that sees the change. The writer is out of
# the index's way twice: before its triggers are visible (the first pass indexed the rows its snapshot saw, and nobody tells the index which of them the heap ended since), and until it is valid
# (a trigger of an index that is not valid did nothing, and the validation pass only added the rows the first pass missed). The summary of the scope and the newest rows of a key count the stale
# entries, and a version and the one that replaced it are both read. The build is held by sift.test_build_gate (a testing build) at two points, each by an advisory lock the test holds (0x51F7, 8:
# after the first pass; 0x51F7, 20: after the validation pass has read the index), so no timing is involved.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
node_init cg 95 "autovacuum = off
sift.test_create_format1 = off"
node_start cg || bail "start failed"
D=$(dir cg)
SCOPE="scope = 'tenant_id: dim_a_id -> pairs(dim_a_id, tenant_id) max_multiplicity(8)'"
sql cg "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO pairs SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 300) m, generate_series(0, 3) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX pairs_journal ON pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT i, 1 + i % 320, timestamptz '2026-01-01' + i * interval '1 minute', i % 1000 FROM generate_series(1, 20000) i" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH ($SCOPE)" \
	"VACUUM facts" >/dev/null || bail "setup failed"

ARM_SCOPE="SET sift.enable_agg = on"
ARM_PLAIN="SET sift.enable_agg = off; SET sift.enable_col_scan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off"
newest() { # the five newest facts of each key, one statement per key
	for k in $(seq 1 12); do
		echo "SELECT f.id, f.created_at FROM facts f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs WHERE tenant_id IN ($k)) ORDER BY f.created_at DESC, f.id DESC LIMIT 5;"
	done
}
reads() { # ARM: the answers of the scope index for the keys, in one snapshot
	sql cg "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT 1" "$1" "SET sift.agg_force = on" \
		"SELECT count(*), sum(f.amount), sum(f.id) FROM facts f JOIN pairs p ON p.dim_a_id = f.dim_a_id WHERE p.tenant_id = 3" \
		"SELECT p.tenant_id, count(*), min(f.id), max(f.id) FROM facts f JOIN pairs p ON p.dim_a_id = f.dim_a_id WHERE p.tenant_id IN (2, 5, 8) GROUP BY 1 ORDER BY 1" \
		"$(newest)" "COMMIT"
}
held() { [[ $(sql cg "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; }

# build NAME KEY EDIT: CREATE INDEX CONCURRENTLY NAME, held at gate KEY while EDIT runs; the reads of the new index (the only one left) are the heap's
build() {
	rm -f "$D/locked" "$D/release"
	{
		echo "SELECT pg_advisory_lock(20983, $2);"
		echo "\\! touch '$D/locked'"
		echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	} | sql cg >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql cg "SET sift.test_build_gate = on" "CREATE INDEX CONCURRENTLY $1 ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH ($SCOPE)" >/dev/null 2>"$D/cic.err" &
	cp=$!
	wait_for held 20 || bail "the build did not reach its gate"
	sql cg "$3" >/dev/null
	touch "$D/release"
	wait "$hp"
	wait "$cp"
}
same() { test "$(reads "$ARM_SCOPE")" == "$(reads "$ARM_PLAIN")" -a "$(reads "$ARM_PLAIN" | wc -l)" -gt 40; }

build facts_col2 8 "DELETE FROM facts WHERE id % 7 = 0; UPDATE facts SET amount = amount + 1 WHERE id > 19000; DELETE FROM facts WHERE id BETWEEN 18000 AND 18400"
check "the build finished and the index is valid" test "$(sql cg "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid = 'facts_col2'::regclass")" == t
sql cg "DROP INDEX facts_col" >/dev/null
check "rows ended during the first pass: the reads of the scope index are the heap's" same
sql cg "VACUUM facts" >/dev/null
check "... and after VACUUM" same

build facts_col3 20 "UPDATE facts SET dim_a_id = 1 + dim_a_id % 320 WHERE id > 19500; DELETE FROM facts WHERE id BETWEEN 17000 AND 17300; UPDATE facts SET amount = amount + 1 WHERE id > 18500"
sql cg "DROP INDEX facts_col2" >/dev/null
check "rows ended after the validation pass read the index: the reads of the scope index are the heap's" same
check "the index verifies against the heap" test -z "$(sql cg "SELECT * FROM sift.check('facts_col3', true, 50)" 2>&1)"
finish
