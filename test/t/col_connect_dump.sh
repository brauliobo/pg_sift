#!/usr/bin/env bash
# A child sift_col index with the reloption connect (two connections, two parents) and one that connected itself through a foreign key, restored from pg_dump: the custom archive serially and in parallel, and a plain dump through psql. The text of the option is in the dump,
# the child index is made after the parent index (it depends on it), and the restore ends with the same connection: the dependency, the parent's journal, a stamp that is vouched and
# the keys changed after it.
. "$(dirname "$0")/lib.bash"

plan 9
node_init r 37 "autovacuum = off"
node_start r || bail "start failed"
D=$(dir r)
S=("-h" "$D/sock" "-p" "$(port r)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
fresh() { dbq postgres "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }

# the connection as a user sees it: the dependency, the journal, then a stamp, one UPDATE of the parent, and the keys that changed since the stamp
STATE="SELECT count(*) FROM pg_depend d JOIN pg_class c ON c.oid = d.objid WHERE c.relname = 'a_events_col' AND d.refobjid IN ('dim_a_col'::regclass, 'dim_b_col'::regclass) AND d.deptype = 'n'"
JOURNAL="SELECT bool_and(lineage <> 0) FROM (VALUES ('dim_a_col'), ('dim_b_col')) v(i), LATERAL sift._test_conn_gen(v.i::regclass)"
CHANGED="CREATE TEMP TABLE st AS SELECT * FROM sift._test_conn_take('dim_a_col'); SELECT 1 FROM st WHERE why = 'ok'; UPDATE dim_a SET category_id = category_id + 1 WHERE id = 3; SELECT string_agg(why || ':' || coalesce(key::text, ''), ',') FROM sift._test_conn_changed('dim_a_col', (SELECT lineage FROM st), (SELECT horizon FROM st))"
AUTO="SELECT declaration || ' ' || automatic FROM sift.col_connections WHERE child = 'b_events_col'::regclass"
state() { dbq "$1" "$STATE" "$JOURNAL" "$CHANGED" "$AUTO" | tr '\n' ' '; }

fresh src
dbq src "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE dim_a (id int PRIMARY KEY, category_id int NOT NULL, label text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO dim_a SELECT i, i % 10, 'l' || i FROM generate_series(1, 2000) i" \
	"CREATE TABLE dim_b (id int PRIMARY KEY, zone text NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO dim_b SELECT i, 'r' || i % 5 FROM generate_series(1, 300) i" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int NOT NULL, dim_b_id int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, 1 + i % 2000, 1 + i % 300, i FROM generate_series(1, 6000) i" \
	"CREATE INDEX a_events_col ON events USING sift_col (id, dim_a_id, dim_b_id, value_num) WITH (connect = 'dim_a_id -> dim_a(id) carries category_id, label; dim_b_id -> dim_b(id) carries zone')" \
	"CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label)" >/dev/null 2>&1
check "the source refuses a child index whose parent has no index yet" test "$(dbq src "SELECT count(*) FROM pg_class WHERE relname = 'a_events_col'")" == 0
dbq src "DROP INDEX dim_a_col" >/dev/null 2>&1
dbq src "CREATE INDEX dim_a_col ON dim_a USING sift_col (id, category_id, label)" "CREATE INDEX dim_b_col ON dim_b USING sift_col (id, zone)" \
	"CREATE INDEX a_events_col ON events USING sift_col (id, dim_a_id, dim_b_id, value_num) WITH (connect = 'dim_a_id -> dim_a(id) carries category_id, label; dim_b_id -> dim_b(id) carries zone')" \
	"CREATE TABLE events_fk (id int PRIMARY KEY, dim_a_id int NOT NULL REFERENCES dim_a, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events_fk SELECT i, 1 + i % 2000, i FROM generate_series(1, 3000) i" \
	"CREATE INDEX b_events_col ON events_fk USING sift_col (id, dim_a_id, v)" >/dev/null 2>&1
WANT="2 t 1 ok:3 dim_a_id -> dim_a(id) carries category_id, label true "
check "the source: one dependency, the journal, a stamp that vouches and the changed key" test "$(state src)" == "$WANT"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
check "pg_dump writes the text of the option" test "$(grep -c "connect='dim_a_id -> dim_a(id) carries category_id, label; dim_b_id -> dim_b(id) carries zone'" "$D/src.sql")" == 1
check "pg_dump writes the connection an index chose by itself, and a restore needs no constraint for it" test "$(grep -c "connect='auto: dim_a_id -> dim_a(id) carries category_id, label'" "$D/src.sql")" == 1
check "the child index is made after both parent indexes" test "$(grep -n 'CREATE INDEX dim_a_col' "$D/src.sql" | head -n 1 | cut -d: -f1)" -lt "$(grep -n 'CREATE INDEX a_events_col' "$D/src.sql" | head -n 1 | cut -d: -f1)" -a "$(grep -n 'CREATE INDEX dim_b_col' "$D/src.sql" | head -n 1 | cut -d: -f1)" -lt "$(grep -n 'CREATE INDEX a_events_col' "$D/src.sql" | head -n 1 | cut -d: -f1)"

fresh r1
check "pg_restore, serial: no error and the same connection" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(state r1)" == "$WANT"

ok_runs=0
for run in 1 2 3 4 5 6 7 8; do
	fresh rp
	out=$(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1)
	[[ $(errors <<<"$out") == 0 && $(state rp) == "$WANT" ]] && ok_runs=$((ok_runs + 1)) || diag "run $run: $(grep -m1 -A3 'ERROR' <<<"$out" | tr '\n' ' ') state: $(state rp)"
done
check "pg_restore -j8, eight runs: no error and the same connection" test "$ok_runs" == 8

fresh r2
check "pg_dump | psql: no error and the same connection" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(state r2)" == "$WANT"

# a REINDEX of the restored parent keeps the connection
dbq r2 "REINDEX INDEX dim_a_col" "REINDEX INDEX dim_b_col" >/dev/null 2>&1
check "REINDEX of the parent indexes after the restore: both dependencies and a journal each" test "$(dbq r2 "$STATE" "$JOURNAL" | tr '\n' ' ')" == "2 t "
finish
