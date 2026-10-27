#!/usr/bin/env bash
# The peers a parallel build leaves (several segments over the same superblocks) are rewritten by the server's own worker on a table that nothing changes and
# nothing VACUUMs, whatever sift.col_auto_merge_wal_pct says: the scans of the index ask for it, once per sift.maintenance_naptime. A table that opted out of
# autovacuum is left alone, as the rewrite after a VACUUM is.
. "$(dirname "$0")/lib.bash"

plan 7
node_init pe 25 "autovacuum = off
sift.col_auto_merge = on
sift.col_auto_merge_wal_pct = 5
sift.maintenance_naptime = 200
max_parallel_maintenance_workers = 4"
node_start pe || bail "start failed"
sql pe "CREATE EXTENSION pg_sift" \
	"CREATE TABLE nk (id int, cust int, amt int, q int)" \
	"INSERT INTO nk SELECT i, ((i::bigint * 7919) % 20000)::int, ((i::bigint * 31) % 1000)::int, i % 100 FROM generate_series(1, 1000000) i" \
	"CREATE TABLE ko (LIKE nk) WITH (autovacuum_enabled = false)" "INSERT INTO ko SELECT * FROM nk" \
	"VACUUM (FREEZE, ANALYZE) nk" "VACUUM (FREEZE, ANALYZE) ko" \
	"CREATE INDEX nk_idx ON nk USING sift_col (id, cust, amt, q) WITH (key_cut = 'cust')" \
	"CREATE INDEX ko_idx ON ko USING sift_col (id, cust, amt, q) WITH (key_cut = 'cust')" >/dev/null || bail "setup failed"

segments() { sql pe "SELECT count(*) FROM sift.col_segments('$1') WHERE kind = 'immutable'"; }
# a scan of each index asks (the first of a backend, then once per naptime); the rewrite is done when the index has one segment
scan_until_one() {
	local i
	for i in $(seq 120); do
		sql pe "SELECT sum(amt) FROM nk" "SELECT sum(amt) FROM ko" >/dev/null
		[[ $(segments "$1") == 1 ]] && return 0
		sleep 0.5
	done
	return 1
}

check "the build left peers" test "$(segments nk_idx)" -gt 1 -a "$(segments ko_idx)" -gt 1
check "the server's own rule finds the rewrite due, with the budget on" test "$(sql pe "SELECT coalesce(sift.col_maintenance_due('nk_idx', true), '-')")" == "fragmented units"
check "scans alone, no VACUUM, no change, get the rewrite done" scan_until_one nk_idx
check "the index verifies" test "$(sql pe "SELECT sift.col_verify('nk_idx', true)" 2>&1 | grep -c ERROR)" == 0
check "and answers as the heap does" test "$(sql pe "SET sift.enable_agg = on" "SELECT sum(amt) FROM nk" | tail -1)" == "$(sql pe "SET sift.enable_agg = off" "SELECT sum(amt) FROM nk" | tail -1)"
check "it is not due again" test "$(sql pe "SELECT coalesce(sift.col_maintenance_due('nk_idx', true), '-')")" == "-"
check "the table that opted out of autovacuum keeps its peers" test "$(segments ko_idx)" -gt 1

finish
