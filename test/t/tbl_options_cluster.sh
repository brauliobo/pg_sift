#!/usr/bin/env bash
# Per-table options of a sift_table (sift.table_set_option) across everything that moves a table: pg_dump | psql, pg_restore (also parallel, also with a table
# left out of the dump, also a dump of one table), a row of a table that is gone (written by nobody: the event trigger removes it, with event triggers off the dump
# skips it), crash recovery, a hot standby (the rows replay; a standby that read the options sees a change), and pg_upgrade (the options of every database
# come across, and the tables keep the OIDs the rows are keyed by).
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

# every option the tables set, as one string: schema.table.name=value
QO="SELECT coalesce(string_agg(format('%s.%s.%s=%s', n.nspname, c.relname, o.name, o.value), ',' ORDER BY n.nspname, c.relname, o.name), '')
	FROM sift.table_options() o JOIN pg_class c ON c.oid = o.rel JOIN pg_namespace n ON n.oid = c.relnamespace WHERE o.source = 'table'"
EXPECT='public.plain_t.compression=zstd:3,public.plain_t.write_buffer_limit=2MB,public.renamed.dict_lookup=off,public.skipme.hot_update=off,s1.sales.hot_update=off,s1.sales.wide_rows=off'
EXPECT_NO_SKIPME=${EXPECT/public.skipme.hot_update=off,/}
# sqld NODE DB QUERY...
sqld() { local n=$1 db=$2; shift 2; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$n")/sock" -p "$(port "$n")" -U postgres -d "$db" "${@/#/--command=}"; }
pgt() { local n=$1 tool=$2; shift 2; "$tool" -h "$(dir "$n")/sock" -p "$(port "$n")" -U postgres "$@"; }
# the rows the dump writes for the table of the options
dumped_rows() { pgt a pg_dump -d postgres "$@" | awk '/^COPY sift.table_option_values /{on=1; next} /^\\\.$/{on=0} on' | wc -l; }

node_init a 2 "wal_level = replica
max_wal_senders = 4
autovacuum = off"
node_start a || bail "node a did not start"
sqld a postgres "CREATE EXTENSION pg_sift" "CREATE SCHEMA s1" \
	"CREATE TABLE filler1 (a int)" "CREATE TABLE filler2 (a int)" "CREATE TABLE filler3 (a int)" \
	"CREATE TABLE public.plain_t (id int, v text) USING sift_table" "CREATE TABLE s1.sales (id int, v text) USING sift_table" \
	"CREATE TABLE public.skipme (id int) USING sift_table" "CREATE TABLE public.gone_t (id int) USING sift_table" "CREATE TABLE public.to_rename (id int) USING sift_table" \
	"INSERT INTO plain_t SELECT g, 'x' || g FROM generate_series(1, 3000) g" "INSERT INTO s1.sales SELECT g, 'y' || g FROM generate_series(1, 3000) g" \
	"SELECT sift.table_set_option('plain_t', 'write_buffer_limit', '2MB'), sift.table_set_option('plain_t', 'compression', 'zstd:3')" \
	"SELECT sift.table_set_option('s1.sales', 'hot_update', 'off'), sift.table_set_option('s1.sales', 'wide_rows', 'off')" \
	"SELECT sift.table_set_option('skipme', 'hot_update', 'off'), sift.table_set_option('gone_t', 'hot_update', 'off'), sift.table_set_option('to_rename', 'dict_lookup', 'off')" \
	"ALTER TABLE to_rename RENAME TO renamed" "DROP TABLE gone_t" \
	"CREATE TABLE stale_t (id int) USING sift_table" "SELECT sift.table_set_option('stale_t', 'hot_update', 'off')" >/dev/null || bail "setup failed"
check "the options of the tables are those set; the dropped table left no row, stale_t is still there" test "$(sqld a postgres "$QO")" == "${EXPECT/public.skipme.hot_update=off,/public.skipme.hot_update=off,public.stale_t.hot_update=off,}"
check "seven rows: the six options and the one of stale_t" test "$(sqld a postgres "SELECT count(*) FROM sift.table_option_values")" == 7
sqld a postgres "SET event_triggers = off" "DROP TABLE stale_t" >/dev/null
check "with event triggers off the row of a dropped table stays, unreadable" test "$(sqld a postgres "SELECT count(*) FROM sift.table_option_values")/$(sqld a postgres "$QO")" == "7/$EXPECT"
check "pg_dump writes the six rows of the tables there are, and not the stale one" test "$(dumped_rows)" == 6

TMPD=$(dir a)/dumps
mkdir -p "$TMPD"
pgt a pg_dump -d postgres -Fc -f "$TMPD/full.dump"
for db in r1 r2 r3 r4; do sqld a postgres "CREATE DATABASE $db" >/dev/null; done

pgt a pg_restore -d r1 -j 2 -e "$TMPD/full.dump" >"$TMPD/r1.log" 2>&1
check "pg_restore -j 2 succeeds" test $? -eq 0
check "the options come back on the restored tables" test "$(sqld a r1 "$QO")" == "$EXPECT"
check "the restored database has no row for the stale table" test "$(sqld a r1 "SELECT count(*) FROM sift.table_option_values")" == 6
check "the restored tables are not the OIDs of the source (the rows are keyed by name in the dump)" test "$(sqld a r1 "SELECT oid FROM pg_class WHERE relname = 'plain_t'")" != "$(sqld a postgres "SELECT oid FROM pg_class WHERE relname = 'plain_t'")"
check "an option acts on the restored table: wide_rows off refuses the row" test "$(sqld a r1 "INSERT INTO s1.sales VALUES (1, repeat('a', 5000000))" 2>&1 | grep -c 'is too large')" == 1

pgt a pg_dump -d postgres | pgt a psql -X -q -v ON_ERROR_STOP=1 -d r3 >"$TMPD/r3.log" 2>&1
check "pg_dump | psql succeeds" test ${PIPESTATUS[1]} -eq 0
check "the options come back through the plain dump" test "$(sqld a r3 "$QO")" == "$EXPECT"

# a table left out of the dump: the dump still has its row (the table exists at dump time), the restore finds no such table and skips the row
pgt a pg_dump -d postgres -Fc -T public.skipme -f "$TMPD/skip.dump"
check "the dump without skipme still carries its row of options" test "$(pgt a pg_restore -f - "$TMPD/skip.dump" | awk '/^COPY sift.table_option_values /{on=1; next} /^\\\.$/{on=0} on' | grep -c skipme)" == 1
pgt a pg_restore -d r2 -e "$TMPD/skip.dump" >"$TMPD/r2.log" 2>&1
check "pg_restore of a dump that leaves a table out succeeds, whatever its row says" test $? -eq 0
check "the other options come back, and none for the table that is not there" test "$(sqld a r2 "$QO")/$(sqld a r2 "SELECT count(*) FROM sift.table_option_values")" == "$EXPECT_NO_SKIPME/5"
# a restore into a database that has the extension and not the table: the same, with the rows copied straight in
sqld a r4 "CREATE EXTENSION pg_sift" "CREATE SCHEMA s1" "CREATE TABLE s1.sales (id int, v text) USING sift_table" >/dev/null
pgt a pg_restore -d r4 -e --data-only -t table_option_values -n sift "$TMPD/full.dump" >"$TMPD/r4.log" 2>&1
check "only the data of the options restored into a database with one of the tables succeeds" test $? -eq 0
check "it keeps the options of the table that is there and drops the rest" test "$(sqld a r4 "$QO")/$(sqld a r4 "SELECT count(*) FROM sift.table_option_values")" == "s1.sales.hot_update=off,s1.sales.wide_rows=off/2"
# a dump of one table does not carry the options (they belong to the extension's data, not to the table)
pgt a pg_dump -d postgres -t s1.sales -Fc -f "$TMPD/one.dump"
check "a dump of one table does not carry the table of options" test "$(pgt a pg_restore -f - "$TMPD/one.dump" | grep -c 'table_option_values')" == 0

# crash recovery: a committed option survives kill -9, a reset too
sqld a postgres "SELECT sift.table_set_option('plain_t', 'hot_update', 'off')" >/dev/null
crash a
node_start a || bail "node a did not restart after the crash"
check "after kill -9 the committed option is there" test "$(sqld a postgres "SELECT sift.table_get_option('plain_t', 'hot_update')")" == off
sqld a postgres "SELECT sift.table_reset_option('plain_t', 'hot_update')" >/dev/null
crash a
node_start a || bail "node a did not restart after the second crash"
check "after kill -9 the reset is there" test "$(sqld a postgres "SELECT sift.table_get_option('plain_t', 'hot_update')")" == on
check "and the others are untouched" test "$(sqld a postgres "$QO")" == "$EXPECT"

# a hot standby: the rows replay, and a standby that already read them sees a change
node_clone a b 3 "hot_standby_feedback = on"
node_start b || bail "standby did not start"
check "the standby has the options of the base backup" test "$(sqld b postgres "$QO")" == "$EXPECT"
sqld b postgres "SELECT sift.table_get_option('plain_t', 'compression')" >/dev/null
sqld a postgres "SELECT sift.table_set_option('plain_t', 'compression', 'none')" "SELECT sift.table_set_option('s1.sales', 'hot_update', 'on')" "SELECT sift.table_reset_option('skipme')" >/dev/null
catchup a b
check "the standby reads the new value, the new row and the removed row after replay" test "$(sqld b postgres "$QO")" == "public.plain_t.compression=none,public.plain_t.write_buffer_limit=2MB,public.renamed.dict_lookup=off,s1.sales.hot_update=on,s1.sales.wide_rows=off"
sqld a postgres "DROP TABLE s1.sales"
catchup a b
# 3 rows of the tables there are and the stale one, which replays like any row
check "a table dropped on the primary leaves no row on the standby" test "$(sqld b postgres "SELECT count(*) FROM sift.table_option_values")" == 4
node_stop b

# pg_upgrade: every database, the OIDs the rows are keyed by come across with the tables
sqld a postgres "CREATE TABLE s1.sales (id int, v text) USING sift_table" "SELECT sift.table_set_option('s1.sales', 'hot_update', 'off'), sift.table_set_option('s1.sales', 'wide_rows', 'off')" \
	"SELECT sift.table_set_option('skipme', 'hot_update', 'off'), sift.table_set_option('plain_t', 'compression', 'zstd:3')" >/dev/null
OIDS="SELECT string_agg(c.oid::text, ',' ORDER BY c.relname) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_table' AND c.relkind = 'r'"
before=$(for db in postgres r1 r3; do sqld a $db "$QO"; done)
oids_before=$(sqld a postgres "$OIDS")
if [[ ! -f $(pg_config --pkglibdir)/pg_sift$DLSUFFIX ]]; then
	skip 5 "pg_upgrade needs sift installed in $(pg_config --pkglibdir)"
	plan $NTEST
	finish
fi
node_stop a
node_init up 8
BIN=$(pg_config --bindir)
(cd "$(dir up)" && LD_PRELOAD=${SIFT_PRELOAD:-} pg_upgrade -b "$BIN" -B "$BIN" -d "$(dir a)/data" -D "$(dir up)/data" \
	-U postgres -p "$(port a)" -P "$(port up)" -s "$(dir up)/sock" >"$(dir up)/pg_upgrade.log" 2>&1)
upgraded=$?
check "pg_upgrade of the databases with options succeeds" test $upgraded -eq 0
[[ $upgraded -eq 0 ]] || upgrade_failed up
node_start up || bail "upgraded cluster did not start"
check "the options of every database are the same after pg_upgrade" test "$(for db in postgres r1 r3; do sqld up $db "$QO"; done)" == "$before"
check "the sift_tables have the OIDs they had" test "$(sqld up postgres "$OIDS")" == "$oids_before"
check "an option acts after pg_upgrade: wide_rows off refuses the row" test "$(sqld up postgres "INSERT INTO s1.sales VALUES (1, repeat('a', 5000000))" 2>&1 | grep -c 'is too large')" == 1
check "the upgraded cluster takes a new option" test "$(sqld up postgres "SELECT sift.table_set_option('plain_t', 'hot_update', 'off')" "SELECT sift.table_get_option('plain_t', 'hot_update')" | tr -d '\n')" == off
plan $NTEST
finish
