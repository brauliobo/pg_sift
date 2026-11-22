#!/usr/bin/env bash
# tap: pooled
# The journal index that CREATE INDEX of a scope index made (src/col/scope_make.c) through pg_dump and restore: the dump holds the journal as an index with its comment (the mark that it goes with the last scope
# index that uses it) before the scope indexes that read it, and the restore, serial, in parallel (six runs) and through psql, ends with the same journals: the same names, comments and dependencies, none made twice, and
# the scope indexes that read them. Dropping the restored scope indexes drops the journals, as in the source; a journal of the user's (no comment) stays. A scope of two paths (three journals) and two scope indexes that
# share one journal go through the same restores.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_twohop.bash"

plan 8
node_init sjd 53 "$TWOHOP_CONF"
node_start sjd || bail "start failed"
D=$(dir sjd)
S=("-h" "$(node_sock sjd)" "-p" "$(port sjd)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
fresh() { dbq "$(node_db sjd)" "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }

# every sift_col index of the tables of the pairs: its name, whether it has a comment, and the scope indexes that depend on it
JOURNALS="SELECT coalesce(string_agg(c.relname || ':' || (obj_description(c.oid, 'pg_class') IS NOT NULL)::text || ':' ||
	coalesce((SELECT string_agg(d.objid::regclass::text, ',' ORDER BY d.objid::regclass::text) FROM pg_depend d WHERE d.refobjid = c.oid AND d.classid = 'pg_class'::regclass AND d.deptype = 'n'), '-'), ' ' ORDER BY c.relname), 'none')
	FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid WHERE i.indrelid IN ('tenant_scope'::regclass, 'grants'::regclass, 'memberships'::regclass)
	AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col')"
state() { echo "$(dbq "$1" "$JOURNALS") | $(TWOHOP_IX=hop_col twohop_reads "$1")"; }

TWOHOP_SQL=dbq
fresh src
out=$(dbq src "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 5) j WHERE j < m % 7 ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 420 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 9000) i" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(64)')" \
	"CREATE INDEX events_col2 ON events USING sift_col (id, dim_a_id, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(64)')" 2>&1) || bail "setup failed: $(tail -3 <<<"$out")"
# the scope of two paths: its tables and btrees, without the journals the fixture makes by hand
dbq src "${TWOHOP_BASE[@]:0:9}" "${TWOHOP_BASE[@]:12:3}" "CREATE INDEX hop_col ON hop_events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = '$TWOHOP_SCOPE')" \
	"VACUUM (FREEZE, ANALYZE) hop_events, grants, memberships, tenant_scope, events" >/dev/null || bail "the scope of two paths failed"
SRC=$(state src)
diag "$SRC"
check "the source: one journal for both scope indexes of one path, and one for each of the three relations of the scope of two paths, all commented" \
	test "$(grep -o 'sift_journal:true:[a-z_0-9,]*' <<<"$SRC" | tr '\n' ' ')" == "sift_journal:true:hop_col sift_journal:true:hop_col sift_journal:true:hop_col sift_journal:true:events_col,events_col2 " -a "${SRC##*| }" == same
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
line() { grep -n "$1" "$D/src.sql" | head -n 1 | cut -d: -f1; }
check "the dump has the journal as an index with its comment, before the scope indexes that read it" \
	test "$(grep -c 'COMMENT ON INDEX public.tenant_scope_dim_a_id_tenant_id_sift_journal' "$D/src.sql")" == 1 \
	-a "$(line 'CREATE INDEX tenant_scope_dim_a_id_tenant_id_sift_journal')" -lt "$(line 'CREATE INDEX events_col ')" \
	-a "$(line 'CREATE INDEX grants_dim_a_id_tenant_id_sift_journal')" -lt "$(line 'CREATE INDEX hop_col ')"

fresh r1
check "pg_restore, serial: no error, the same journals (none made twice) and reads" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(state r1)" == "$SRC"
ok_runs=0
for run in 1 2 3 4 5 6; do
	fresh rp
	out=$(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1)
	[[ $(errors <<<"$out") == 0 && $(state rp) == "$SRC" ]] && ok_runs=$((ok_runs + 1)) || diag "run $run: $(grep -m1 -A3 'ERROR' <<<"$out" | tr '\n' ' ') state: $(state rp)"
done
check "pg_restore -j8, six runs: no error and the same journals" test "$ok_runs" == 6
fresh r2
check "pg_dump | psql: no error and the same journals" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(state r2)" == "$SRC"

# the mark survived the restore: the last scope index takes its journal with it, and a journal of the user's stays
dbq r1 "DROP INDEX events_col" >/dev/null
check "restored: dropping one of two scope indexes keeps the journal, dropping the last drops it" \
	test "$(dbq r1 "SELECT count(*) FROM pg_class WHERE relname = 'tenant_scope_dim_a_id_tenant_id_sift_journal'")" == 1 \
	-a "$(dbq r1 "DROP INDEX events_col2" "SELECT count(*) FROM pg_class WHERE relname = 'tenant_scope_dim_a_id_tenant_id_sift_journal'" | tail -n 1)" == 0
dbq r1 "DROP INDEX hop_col" >/dev/null
check "restored: dropping the scope of two paths drops its three journals" test "$(dbq r1 "$JOURNALS")" == none
dbq r2 "COMMENT ON INDEX tenant_scope_dim_a_id_tenant_id_sift_journal IS NULL" "DROP INDEX events_col" "DROP INDEX events_col2" >/dev/null
check "a journal whose comment was removed is the user's: it stays" test "$(dbq r2 "SELECT count(*) FROM pg_class WHERE relname = 'tenant_scope_dim_a_id_tenant_id_sift_journal'")" == 1
finish
