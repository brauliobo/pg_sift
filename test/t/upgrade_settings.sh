#!/usr/bin/env bash
# The settings across an update: a database that the newest 3.2 release wrote, with ALTER DATABASE / ALTER ROLE ... SET sift.* stored (settings that exist in 3.2
# and 3.3, and one the new library does not know: a setting a release removed or renamed). After the library is replaced and ALTER EXTENSION UPDATE: the session still
# starts (the unknown name is a warning, "removing it", not an error), the stored settings apply, the defaults that 3.3 changed are the shipped ones
# (sift.col_auto_merge on, within 5% of the WAL), the update prints nothing about the auto merge and sift.col_maintenance_status answers for the old index,
# CREATE INDEX prints the auto merge NOTICE once, and pg_dump -C | psql (the stored settings of the database and of the role in it travel with the dump) brings
# the same settings and a verifying index back.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v3.2.*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v3.2.*'; exit 0; }
newroot=$ROOT
# qd DB QUERY...: stdout; stderr (the warnings of the session) in $ERR_OUT
qd() { local db=$1; shift; PGOPTIONS="-c client_min_messages=notice" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir us)/sock" -p "$(port us)" -U postgres -d "$db" "${@/#/--command=}" 2>"$TAPDIR/err.txt"; }
err() { cat "$TAPDIR/err.txt"; }

plan 16
build_old "$ref"; oldroot=$OLDROOT
node_init us 3 'autovacuum = off'
use_root us "$oldroot"
node_start us || bail "cannot start $ref"
# the stored settings are written by a session that has not loaded the library: the names are placeholders
qd postgres "CREATE DATABASE app" >/dev/null
qd app "ALTER DATABASE app SET sift.enable_agg = off" "ALTER DATABASE app SET sift.retired_setting = '1'" "ALTER ROLE postgres IN DATABASE app SET sift.simd = 'scalar'" \
	"CREATE EXTENSION pg_sift" "CREATE TABLE docs (id int PRIMARY KEY, g int, body text)" \
	"INSERT INTO docs SELECT i, i % 9, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 4000) i" \
	"CREATE INDEX docs_col ON docs USING sift_col (id, g)" "CREATE INDEX docs_idx ON docs USING sift (body)" >/dev/null || bail "the release cannot be set up"
want=$(qd app "SET sift.enable_agg = off" "SELECT g, count(*) FROM docs GROUP BY g ORDER BY g" | tr '\n' ' ')
node_stop us

use_root us "$newroot"
for k in table_auto_defrag col_auto_merge col_auto_merge_wal_pct; do sed_i "/^sift\\.$k = /d" "$(dir us)/data/postgresql.conf"; done   # the shipped defaults, not those of the test clusters
node_start us || bail "cannot start this tree on $ref data"
upd=$(qd app "ALTER EXTENSION pg_sift UPDATE" 2>&1); rc=$?
check "ALTER EXTENSION UPDATE from $ref" test $rc -eq 0
check "the update prints nothing about the auto merge" test "$(grep -c 'col_auto_merge' <<<"$upd$(err)")" == 0
qd app "SELECT sift.version()" >/dev/null; rc=$?
check "a session starts with a stored setting the library does not know" test $rc -eq 0
check "it says that the setting is removed, as a warning" grep -q 'invalid configuration parameter name "sift.retired_setting", removing it' "$TAPDIR/err.txt"
check "the stored settings that still exist apply (database and role in it)" test "$(qd app "SELECT sift.version()" "SHOW sift.enable_agg" "SHOW sift.simd" | tail -n 2 | tr '\n' ' ')" == "off scalar "
check "sift.col_auto_merge is on in a database that never set it" test "$(qd postgres "LOAD 'pg_sift'" "SHOW sift.col_auto_merge")" == on
check "sift.col_auto_merge_wal_pct is the shipped 5" test "$(qd postgres "LOAD 'pg_sift'" "SHOW sift.col_auto_merge_wal_pct")" == 5
check "sift.col_maintenance_status answers for the index the release wrote" test "$(qd app "SELECT count(*) FROM sift.col_maintenance_status('docs_col')")" -ge 1
check "the index the release wrote answers as it did" test "$(qd app "SET sift.enable_agg = off" "SELECT g, count(*) FROM docs GROUP BY g ORDER BY g" | tr '\n' ' ')" == "$want"
check "col_verify and verify pass" test -z "$(qd app "SELECT sift.col_verify('docs_col', true)" "SELECT sift.verify('docs_idx', true)" 2>&1 | grep -v '^$' | grep -iv 'true\|^t$')"
qd app "CREATE INDEX docs_col2 ON docs USING sift_col (id, g)" >/dev/null
check "CREATE INDEX prints the auto merge NOTICE once" test "$(grep -c 'sift.col_auto_merge keeps it answerable' "$TAPDIR/err.txt")" == 1

# the dump: the database with its settings, dropped and restored
PGHOST=$(dir us)/sock PGPORT=$(port us) PGUSER=postgres pg_dump -C -d app -f "$(dir us)/app.sql" || bail "pg_dump failed"
check "the dump carries the database and the role settings" test "$(grep -c '^ALTER \(DATABASE\|ROLE\)' "$(dir us)/app.sql")" -ge 3
qd postgres "DROP DATABASE app" >/dev/null
PGHOST=$(dir us)/sock PGPORT=$(port us) PGUSER=postgres psql -X -q -d postgres -f "$(dir us)/app.sql" >/dev/null 2>"$TAPDIR/restore.err"
check "the restore succeeds" test $? -eq 0
qd app "SELECT sift.version()" >/dev/null
check "the restored database starts a session and the removed setting is a warning" grep -q 'sift.retired_setting", removing it' "$TAPDIR/err.txt"
check "the settings are back (database and role in it)" test "$(qd app "SELECT sift.version()" "SHOW sift.enable_agg" "SHOW sift.simd" | tail -n 2 | tr '\n' ' ')" == "off scalar "
check "the restored indexes verify and answer the same" test "$(qd app "SELECT sift.col_verify('docs_col', true)" >/dev/null; qd app "SET sift.enable_agg = off" "SELECT g, count(*) FROM docs GROUP BY g ORDER BY g" | tr '\n' ' ')" == "$want"
finish
