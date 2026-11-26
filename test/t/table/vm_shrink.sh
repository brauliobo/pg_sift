#!/usr/bin/env bash
# The tail shrink of VACUUM cuts the file of a sift_table (core/f2/fsm/fsm.c) and keeps its visibility map: the map is indexed by the virtual block of a row, which the size of the file does
# not bound, so a cut of the map to the blocks of the file would clear the bits of the vblocks past it, and the next VACUUM would set them again.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init vs 4 "autovacuum = off
sift.async_maintenance = off"
node_start vs || bail "start failed"
sql vs "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" "CREATE TABLE v (id int, pad text) USING sift_table" \
	"INSERT INTO v SELECT g, md5(g::text) FROM generate_series(1, 8192) g" "INSERT INTO v SELECT g, md5(g::text) FROM generate_series(8193, 16384) g" >/dev/null || bail "setup failed"

blocks() { sql vs "SELECT pg_relation_size('v') / current_setting('block_size')::int"; }
vm_records() { # the records of the map that a VACUUM writes
	sql vs "SELECT pg_current_wal_insert_lsn()" >"$(dir vs)/lsn"
	sql vs "VACUUM v" >/dev/null
	sql vs "SELECT count(*) FROM pg_get_wal_records_info('$(cat "$(dir vs)/lsn")', pg_current_wal_insert_lsn()) WHERE resource_manager = 'pg_sift_table_vm'"
}

before=$(blocks)
vm_records >/dev/null
after=$(blocks)
check "VACUUM merges the two loads and cuts the file ($before -> $after blocks)" test "$after" -lt "$before"
vblocks=$(sql vs "SELECT max((ctid::text::point)[0])::int + 1 FROM v")
check "the rows take more vblocks than the file has blocks ($vblocks > $after)" test "$vblocks" -gt "$after"
out=$(vm_records)
check "the next VACUUM finds every bit of the map still set ($out records of the map)" test "$out" == 0
out=$(sql vs "SET enable_seqscan = off" "SELECT count(*) FROM v WHERE id > 0")
check "the rows are all there" test "$out" == 16384

finish
