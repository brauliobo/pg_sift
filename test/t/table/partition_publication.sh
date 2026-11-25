#!/usr/bin/env bash
# A sift_table that becomes a partition of a published table is published too, and logical replication of it needs the resource manager that only a preloaded library registers: ATTACH PARTITION
# of an existing sift_table and CREATE TABLE ... PARTITION OF it are refused on a server without pg_sift in shared_preload_libraries, as an INSERT into a sift_table that is published is.
. "$(dirname "$0")/lib.bash"
plan 8

node_init p 0 "wal_level = logical
autovacuum = off"
node_start p || bail "start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE TABLE parent (id int, k int) PARTITION BY RANGE (k)" "CREATE TABLE open_parent (id int, k int) PARTITION BY RANGE (k)" \
	"CREATE PUBLICATION pub FOR TABLE parent" >/dev/null || bail "setup failed"
refused() { grep -q 'is published and wal_level is "logical", but pg_sift is not in shared_preload_libraries' <<<"$out"; }
children() { sql p "SELECT coalesce(string_agg(c.relname, ',' ORDER BY c.relname), '') FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid WHERE i.inhparent = '$1'::regclass"; }

out=$(sql p "LOAD 'pg_sift'" "CREATE TABLE attached (id int, k int) USING sift_table" "ALTER TABLE parent ATTACH PARTITION attached FOR VALUES FROM (0) TO (100)" 2>&1)
check "ATTACH PARTITION of a sift_table under a published table is refused" refused
check "... and the table is not a partition" test "$(children parent)" == ""
out=$(sql p "LOAD 'pg_sift'" "CREATE TABLE created PARTITION OF parent FOR VALUES FROM (100) TO (200) USING sift_table" 2>&1)
check "CREATE TABLE PARTITION OF a published table with the sift_table method is refused" refused
check "... and the table is not made" test "$(sql p "SELECT count(*) FROM pg_class WHERE relname = 'created'")" == 0

check "a sift_table partition of a table that is not published is made" sql p "LOAD 'pg_sift'" "CREATE TABLE open_created PARTITION OF open_parent FOR VALUES FROM (0) TO (100) USING sift_table"
check "... and another one is attached" sql p "LOAD 'pg_sift'" "CREATE TABLE open_attached (id int, k int) USING sift_table" "ALTER TABLE open_parent ATTACH PARTITION open_attached FOR VALUES FROM (100) TO (200)"
check "... and both take rows" test "$(sql p "INSERT INTO open_parent SELECT g, g FROM generate_series(0, 199) g" "SELECT count(*) FROM open_parent")" == 200
check "a heap partition of a published table is attached" sql p "CREATE TABLE heap_part (id int, k int)" "ALTER TABLE parent ATTACH PARTITION heap_part FOR VALUES FROM (0) TO (100)"
finish
