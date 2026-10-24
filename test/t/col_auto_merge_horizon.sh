#!/usr/bin/env bash
# The automatic rewrite waits for the horizon. A snapshot older than a rewrite keeps the blocks the rewrite freed from being reused and the rows
# the churn wrote out of the visibility map, so another rewrite answers no more units and leaves as much garbage again: with the snapshot held the index
# is not due for it (sift.col_maintenance_due(index, true)) and sift.col_maintenance_status says why, and when the snapshot ends it is due again.
. "$(dirname "$0")/lib.bash"

plan 7
node_init hz 23 "autovacuum = off
sift.maintenance_naptime = 200"
node_start hz || bail "start failed"
sql hz "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"

# the churn of a round: every twentieth row, then the VACUUM that tells the index which rows died
churn() { sql hz "UPDATE facts SET amount = amount + 1 WHERE id % 20 = $1" "VACUUM facts" >/dev/null; }
due() { sql hz "SELECT coalesce(sift.col_maintenance_due('facts_idx', true), '-')"; }
rewrite() { sql hz "SELECT sift.col_maintain('facts_idx')" >/dev/null; }
blocked() { sql hz "SELECT array_to_string(blocked_by, ',') FROM sift.col_maintenance_status('facts_idx')"; }
snapshot_blocks() { [[ $(blocked) == *snapshot* ]]; }
no_snapshot_blocks() { ! snapshot_blocks; }
# the statistics lag the statements: until the table has an entry vacuum_at is NULL
vacuum_at() { for _ in $(seq 50); do v=$(sql hz "SELECT vacuum_at FROM sift.col_maintenance_status('facts_idx')"); [[ -n $v ]] && break; sleep 0.2; done; echo "$v"; }
holders() { sql hz "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'holder' AND backend_xmin IS NOT NULL"; }

churn 1
check "no old snapshot: the index is due for a rewrite" test "$(due)" != "-"
rewrite
churn 2
check "after a rewrite and no old snapshot: due again" test "$(due)" != "-"

PGAPPNAME=holder psql -X -q -h "$(dir hz)/sock" -p "$(port hz)" -U postgres -d postgres -c "BEGIN ISOLATION LEVEL REPEATABLE READ" \
	-c "SELECT count(*) FROM facts" -c "SELECT pg_sleep(300)" >/dev/null 2>&1 &
for _ in $(seq 50); do [[ $(holders) == 1 ]] && break; sleep 0.1; done
rewrite
churn 3
check "a snapshot older than the rewrite: not due" test "$(due)" == "-"
check "col_maintenance_status names the snapshot" snapshot_blocks
at=$(vacuum_at)
check "the VACUUM starts at a thousandth of the rows (the live rows are an estimate)" test "$at" -ge 90 -a "$at" -le 110

sql hz "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'holder'" >/dev/null
for _ in $(seq 50); do [[ $(holders) == 0 ]] && break; sleep 0.1; done
churn 4
check "the snapshot is gone: due again" test "$(due)" != "-"
check "col_maintenance_status no longer names it" no_snapshot_blocks

finish
