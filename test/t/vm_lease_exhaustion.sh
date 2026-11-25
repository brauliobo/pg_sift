#!/usr/bin/env bash
# An INSERT into a sift_table never fails for want of a vblock lease. A backend holds one slot of the 2048 of the registry for every table whose run of vblocks it still fills, so one session that
# inserts into more tables than there are slots, and a server whose registry cannot be had (sift.test_dsm_fail = sift_shm_manifest: no registry, as with a full /dev/shm), used to end the INSERT with
# "could not reserve a vblock lease". The rows are exact in both, and VACUUM still runs.
. "$(dirname "$0")/lib.bash"

TABLES=2100
out=
plan 8
node_init vl 80 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off"
node_start vl || bail "start failed"
sql vl "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
out=$(sql vl "DO \$\$ BEGIN FOR i IN 1..$TABLES LOOP EXECUTE format('CREATE TABLE lt_%s (k int, v int) USING sift_table', i); COMMIT; END LOOP; END \$\$" 2>&1)
check "created $TABLES sift_tables" test -z "$out"
out=$(sql vl "DO \$\$ BEGIN FOR i IN 1..$TABLES LOOP EXECUTE format('INSERT INTO lt_%s SELECT g, g FROM generate_series(1, 3) g', i); COMMIT; END LOOP; END \$\$" 2>&1)
check "one session inserts into more tables than the registry has leases" test -z "$out"
out=$(sql vl "DO \$\$ DECLARE n bigint := 0; BEGIN FOR i IN 1..$TABLES LOOP EXECUTE format('SELECT count(*) FROM lt_%s', i) INTO n; IF n <> 3 THEN RAISE EXCEPTION 'lt_% has % rows', i, n; END IF; END LOOP; END \$\$" 2>&1)
check "every table holds its rows" test -z "$out"
out=$(sql vl "INSERT INTO lt_1 SELECT g, g FROM generate_series(1, 100000) g" "VACUUM lt_1" "SELECT count(*) FROM lt_1" 2>&1 | tail -1)
check "a bulk insert and VACUUM of the first table follow" test "$out" = 100003
node_stop vl

node_init v0 81 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.test_dsm_fail = 'sift_shm_manifest'"
node_start v0 || bail "start of the server without the registry failed"
sql v0 "CREATE EXTENSION pg_sift" "CREATE TABLE z (k int, v int) USING sift_table" >/dev/null || bail "setup failed"
out=$(sql v0 "INSERT INTO z SELECT g, g FROM generate_series(1, 5000) g" 2>&1)
check "INSERT into a sift_table with no registry of leases" test -z "$out"
out=$(sql v0 "INSERT INTO z VALUES (1, 1)" "COPY z FROM PROGRAM 'printf \"7\\t7\\n8\\t8\\n\"'" 2>&1)
check "and single rows and COPY" test -z "$out"
check "the rows are exact" test "$(sql v0 "SELECT count(*) || ' ' || sum(v) FROM z")" = "5003 12502516"
out=$(sql v0 "VACUUM z" 2>&1)
check "and VACUUM runs" test -z "$out"
finish
