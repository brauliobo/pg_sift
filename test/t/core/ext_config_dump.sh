#!/usr/bin/env bash
# The extension tables that keep the definitions of a sift_table by the OID of the table (sift.projection_defs, sift.ttl_policies, sift.table_paths) are dumped with the database, and a dump
# or a restore that does not have the table must neither fail nor carry a dangling row: a restore without the table reads the key of sift.projection_defs as 0, so that two tables that are
# missing with a projection of one name must not meet in its unique key; a row that names a table that is gone (a stale OID prints as a number) is not written to the dump, and the
# preflight of an update names the tables that hold such rows.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 8
node_init ed 0 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off"
node_start ed || bail "start failed"
sql ed "CREATE EXTENSION pg_sift" \
	"CREATE TABLE tt (id int PRIMARY KEY, ts timestamptz NOT NULL, doc jsonb) USING sift_table" \
	"INSERT INTO tt SELECT g, now(), '{\"k\": 1}' FROM generate_series(1, 200) g" \
	"SELECT sift.set_ttl('tt', 'ts', interval '30 days')" \
	"SELECT sift.table_set_paths('tt', 'doc', 'doc.k int4')" \
	"SELECT sift.create_projection('tt', 'by_ts', 'ORDER BY ts')" >/dev/null || bail "setup failed"

dump() { pg_dump -h "$(dir ed)/sock" -p "$(port ed)" -U postgres -d postgres -f "$1"; }
upgrade_orphans() { sql ed "SELECT count(*) FROM sift.upgrade_check() WHERE kind = 'option' AND object IN ('sift.table_paths', 'sift.ttl_policies', 'sift.projection_defs')"; }

out=$(printf 'public.gone_a\tby_b\tORDER BY b\npublic.gone_b\tby_b\tORDER BY b\n' | sql ed "COPY sift.projection_defs (rel, name, spec) FROM STDIN" 2>&1)
check "a restore without two tables that each had a projection of one name has no error" test -z "$out"
out=$(sql ed "SELECT count(*) FROM sift.projection_defs")
check "and keeps no row of them" test "$out" == 1

sql ed "INSERT INTO sift.ttl_policies VALUES (4000000000::oid::regclass, 'ts', interval '1 day')" \
	"INSERT INTO sift.table_paths VALUES (4000000000::oid::regclass, 'doc', 'doc.k int4')" >/dev/null
dump "$(dir ed)/dump.sql" || bail "pg_dump failed"
out=$(grep -c "^public.tt	" "$(dir ed)/dump.sql")
check "the dump holds the TTL, the paths and the projection of the table that is there" test "$out" -ge 3
out=$(grep -c "^4000000000	" "$(dir ed)/dump.sql")
check "and no row of a table that is gone" test "$out" == 0
out=$(sql ed "SELECT count(*) FROM pg_extension e, unnest(e.extconfig, e.extcondition) AS u(c, w) WHERE e.extname = 'pg_sift' AND u.c::regclass::text IN ('sift.ttl_policies', 'sift.table_paths') AND u.w LIKE '%pg_class%'")
check "the two tables are dumped under a condition on the tables that are there" test "$out" == 2

out=$(upgrade_orphans)
check "the preflight names the two tables that hold rows of a table that is gone" test "$out" == 2
sql ed "DELETE FROM sift.ttl_policies WHERE relid::oid = 4000000000" "DELETE FROM sift.table_paths WHERE relid::oid = 4000000000" >/dev/null
out=$(upgrade_orphans)
check "and names none once they are deleted" test "$out" == 0
out=$(sql ed "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'sift.projection_defs'::regclass AND NOT tgisinternal")
check "sift.projection_defs has the trigger that skips the rows of a table that is not there" test "$out" == 1
finish
