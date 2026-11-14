#!/usr/bin/env bash
# A publication FOR ALL TABLES publishes the tables of the extension too, and a published table that publishes deletes needs a replica identity: the sql_drop trigger of
# sift.table_option_values (4.2.0 to 4.3.0) deleted from it without one, so every DROP of every relation failed. The update to 4.3.1 and 5.0 gives the table its identity: DROP INDEX, DROP TABLE and
# DROP EXTENSION work with such a publication (after the update from 4.2.0 or from 4.3.0, and from a fresh install), and a subscriber receives the options.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
newroot=$ROOT
ext=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$REPO/pg_sift.control")
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
plan 20

CONF="wal_level = logical
max_replication_slots = 5
max_wal_senders = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16
shared_preload_libraries = 'pg_sift'"
TABLES="CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL)
CREATE INDEX h_c ON h USING sift_col (id, g, v)
CREATE TABLE t (id int, v int) USING sift_table
CREATE TABLE plain (id int)"
define() { local n=$1 q; while IFS= read -r q; do sql "$n" "$q" >/dev/null || bail "ddl failed on $n: $q"; done <<<"CREATE EXTENSION pg_sift
$TABLES"; }
drops() { sql "$1" "DROP INDEX h_c" "DROP TABLE t" "DROP TABLE plain" >/dev/null 2>&1; }
identity() { sql "$1" "SELECT relreplident FROM pg_class WHERE oid = 'sift.table_option_values'::regclass"; }

# a release: every drop fails, and the update repairs it
upgrade() {
	local ref=$1 n=$2
	git -C "$REPO" rev-parse -q --verify "refs/tags/$ref" >/dev/null || { skip 5 "no release tag $ref"; return; }
	build_old "$ref"
	node_init "u$n" "$n" "$CONF"
	use_root "u$n" "$OLDROOT"
	node_start "u$n" || bail "cannot start $ref"
	sql "u$n" "CREATE PUBLICATION pu FOR ALL TABLES" >/dev/null || bail "publication failed"
	define "u$n"
	check "$ref: the table of the options has no replica identity" test "$(identity "u$n")" == d
	check "$ref: DROP INDEX with a FOR ALL TABLES publication fails" grep -q 'does not have a replica identity' <<<"$(sql "u$n" "DROP INDEX h_c" 2>&1)"
	node_stop "u$n"
	use_root "u$n" "$newroot"
	node_start "u$n" || bail "cannot start this tree on $ref data"
	sql "u$n" "ALTER EXTENSION pg_sift UPDATE" >/dev/null 2>"$TAPDIR/upd.err"
	check "ALTER EXTENSION UPDATE from $ref to $ext, and the table of the options is identified by its key index" test "$(sql "u$n" "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")|$(identity "u$n")" == "$ext|i"
	drops "u$n"
	check "... DROP INDEX, DROP TABLE of a sift_table and DROP TABLE of a plain table work" test "$(sql "u$n" "SELECT count(*) FROM pg_class WHERE relname IN ('h_c', 't', 'plain')")" == 0
	check "... and so does DROP EXTENSION" sql "u$n" "DROP TABLE h" "DROP EXTENSION pg_sift"
}
upgrade v4.2.0 76
upgrade v4.3.0 79

# a fresh install: the same, and a subscriber that has the tables
node_init p 77 "$CONF"
node_init s 78 "$CONF"
node_start p || bail "publisher start failed"
node_start s || bail "subscriber start failed"
sql p "CREATE PUBLICATION pall FOR ALL TABLES" >/dev/null || bail "publication failed"
define p
define s
check "a fresh install: the table of the options is identified by its key index" test "$(identity p)" == i
check "every table of the extension has a replica identity (a primary key or an index)" test "$(sql p "SELECT coalesce(string_agg(c.relname, ','), '') FROM pg_class c WHERE c.relnamespace = 'sift'::regnamespace AND c.relkind IN ('r', 'p') AND c.relreplident = 'd' AND NOT EXISTS (SELECT 1 FROM pg_index i WHERE i.indrelid = c.oid AND i.indisprimary)")" == ""
sql s "CREATE SUBSCRIPTION sall CONNECTION '$(conninfo p)' PUBLICATION pall" >/dev/null || bail "subscribe failed"
sql p "SELECT sift.table_set_option('t', 'hot_update', 'off')" "INSERT INTO t VALUES (1, 1)" >/dev/null
opts() { sql "$1" "SELECT count(*) FROM sift.table_option_values WHERE name = 'hot_update'"; }
check "the subscriber receives the options of a table" wait_for '[[ $(opts s) == 1 ]]' 120
check "... and the rows of the table" wait_for '[[ $(sql s "SELECT count(*) FROM t") == 1 ]]' 120
check "DROP INDEX with the publication" sql p "DROP INDEX h_c"
check "DROP TABLE of a sift_table with options removes them" test "$(sql p "DROP TABLE t" >/dev/null; echo $?)|$(opts p)" == "0|0"
sql p "INSERT INTO plain VALUES (7)" >/dev/null
check "... the subscriber keeps the options of the table it still has (DDL is not replicated), and replication goes on" wait_for '[[ $(sql s "SELECT count(*) FROM plain") == 1 && $(opts s) == 1 ]]' 120
check "DROP TABLE of a plain table with the publication" sql p "DROP TABLE plain"
sql s "DROP SUBSCRIPTION sall" >/dev/null
check "DROP EXTENSION with the publication" sql p "DROP TABLE h" "DROP EXTENSION pg_sift"
check "... and CREATE EXTENSION again" test "$(sql p "CREATE EXTENSION pg_sift" "SELECT relreplident FROM pg_class WHERE oid = 'sift.table_option_values'::regclass" | tail -n 1)" == i
finish
