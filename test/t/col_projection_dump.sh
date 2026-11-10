#!/usr/bin/env bash
# A database with named projections (sift.projection_create, sift.projection_index) restored from pg_dump: the custom archive serially and in parallel and a plain dump through
# psql. The definitions (sift.projections) are extension configuration, dumped as data; the indexes carry the names in their option projection and are created before or without
# the data of the definitions (a restore of the schema alone is what pg_upgrade runs), so the names are resolved by sift.projection_status and never by CREATE INDEX. The status,
# the plan that reads the index that serves a projection and the guard of the definitions are the source's after every restore. DROP EXTENSION and ALTER EXTENSION UPDATE
# (from 4.3, which has no projections) leave nothing behind and nothing broken. pg_upgrade of such a database is in pg_upgrade.sh.
. "$(dirname "$0")/lib.bash"

plan 20
node_init r 55 "autovacuum = off"
node_start r || bail "start failed"
D=$(dir r)
S=("-h" "$D/sock" "-p" "$(port r)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
fresh() { dbq postgres "DROP DATABASE IF EXISTS $1 WITH (FORCE)" "CREATE DATABASE $1" >/dev/null; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
STATUS="SELECT string_agg(projection || ':' || coalesce(index, '-') || ':' || state || ':' || coalesce(reason, ''), ',' ORDER BY projection, index) FROM sift.projection_status"
# the rows of the three classes the projections stand for, and the index the plan reads for the first
ANSWERS="SELECT (SELECT string_agg(kind || '=' || c, ',' ORDER BY kind) FROM (SELECT kind, count(*) c FROM events WHERE dim_a_id < 20 GROUP BY kind) a) || ';' ||
	(SELECT string_agg(bucket || '=' || c, ',' ORDER BY bucket) FROM (SELECT qty / 10 AS bucket, count(*) c FROM events WHERE dim_a_id < 20 GROUP BY 1) b) || ';' ||
	(SELECT string_agg(kind || '=' || c, ',' ORDER BY kind) FROM (SELECT kind, count(*) c FROM events WHERE status = 'open' GROUP BY kind) c)"
plan_index() { dbq "$1" "SET sift.enable_agg = on" "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 1000" "SET random_page_cost = 1000" \
	"EXPLAIN (COSTS OFF) SELECT kind, count(*) FROM events WHERE dim_a_id < 20 GROUP BY kind" | sed -n 's/^ *Index: //p'; }
same() { [[ $(dbq "$1" "$STATUS") == "$WANT_STATUS" && $(dbq "$1" "$ANSWERS") == "$WANT_ANSWERS" && $(plan_index "$1") == events_wide ]]; }

dbq postgres "CREATE EXTENSION pg_sift" "CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind int, qty int, status text, created_at timestamptz)" \
	"INSERT INTO events SELECT i, i % 40, i % 5, i % 97, (ARRAY['new', 'open', 'held'])[1 + i % 3], timestamptz '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 6000) i" \
	"SELECT sift.projection_create('wide', 'events', ARRAY['dim_a_id', 'created_at', 'kind', '(qty / 10)', 'status', 'qty'], rollups => 'dim_a_id, kind: qty')" \
	"SELECT sift.projection_create('by_kind', 'events', ARRAY['dim_a_id', 'kind'])" \
	"SELECT sift.projection_create('by_bucket', 'events', ARRAY['dim_a_id', '(qty / 10)'])" \
	"SELECT sift.projection_create('open_kinds', 'events', ARRAY['dim_a_id', 'kind', 'status'], filter => \$\$status = 'open'\$\$)" \
	"SELECT sift.projection_index('events_wide', ARRAY['wide'], '{\"key_order\": \"dim_a_id, created_at\", \"track_changes\": \"off\"}')" \
	"SELECT sift.projection_index('events_by_kind', ARRAY['by_kind', 'by_bucket'], '{\"key_order\": \"dim_a_id, created_at\"}')" \
	"SELECT sift.projection_index('events_open', ARRAY['open_kinds'], '{\"track_changes\": \"off\", \"key_order\": \"kind\"}')" \
	"VACUUM (FREEZE, ANALYZE) events" >/dev/null || bail "setup failed"
WANT_STATUS=$(dbq postgres "$STATUS")
WANT_ANSWERS=$(dbq postgres "$ANSWERS")
check "the source: the wide index serves two projections, a third has an index of its own, every status is ok" \
	test "$WANT_STATUS" == "by_bucket:events_wide:ok:,by_kind:events_wide:ok:,open_kinds:events_open:ok:,wide:events_wide:ok:" -a "$(plan_index postgres)" == events_wide
pg_dump "${S[@]}" -Fc -f "$D/src.custom" postgres
pg_dump "${S[@]}" -Fp -f "$D/src.sql" postgres
check "the dump holds the definitions as data and the names in the index statements" \
	test "$(grep -c '^COPY sift.projections ' "$D/src.sql")" == 1 -a "$(grep -c "projection=" "$D/src.sql")" -ge 2

fresh r1
check "pg_restore, serial: no error" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0
check "... the status, the answers and the plan are the source's" same r1
fresh rp
check "pg_restore -j8: no error" test "$(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1 | errors)" == 0
check "... the status, the answers and the plan are the source's" same rp
fresh r2
check "pg_dump | psql: no error" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0
check "... the status, the answers and the plan are the source's" same r2

# the order pg_upgrade works in: the schema first, the data of the definitions after
fresh r3
pg_dump "${S[@]}" --schema-only -Fp -f "$D/schema.sql" postgres
pg_dump "${S[@]}" --data-only -t sift.projections -Fp -f "$D/data.sql" postgres
check "the schema alone restores without error: the indexes are created before any definition exists" test "$(psql -X -q "${S[@]}" -d r3 -f "$D/schema.sql" 2>&1 | errors)" == 0
check "... and every projection that an index lists is broken as not defined, nothing is lost silently" \
	test "$(dbq r3 "SELECT count(*) FILTER (WHERE reason = 'the projection is not defined') || '/' || count(*) FROM sift.projection_status")" == 4/4
check "the data of the definitions, restored after the indexes, makes every status ok again" test "$(psql -X -q "${S[@]}" -d r3 -f "$D/data.sql" 2>&1 | errors)" == 0 -a "$(dbq r3 "$STATUS")" == "$WANT_STATUS"

# the guard and the one call, on a restored database
guard() { dbq r1 "DELETE FROM sift.projections WHERE name = 'wide'" 2>&1 | grep -c 'is held by events_wide'; }
check "the guard of the definitions works on a restored database" test "$(guard)" == 1
check "sift.projection_index of a held projection is the same index, built again nowhere" test "$(dbq r1 "SELECT sift.projection_index('events_by_kind_again', ARRAY['by_kind'], '{\"key_order\": \"dim_a_id, created_at\"}')")" == events_wide \
	-a "$(dbq r1 "SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col'")" == 2
dbq r1 "INSERT INTO events SELECT 6000 + i, i % 40, i % 5, i % 97, 'open', now() FROM generate_series(1, 500) i" "UPDATE events SET qty = qty + 30 WHERE id % 9 = 0" "DELETE FROM events WHERE id % 13 = 0" >/dev/null
check "after changes of the table the rows through the index equal the normal plan" \
	test "$(dbq r1 "SET sift.enable_agg = on" "$ANSWERS")" == "$(dbq r1 "SET sift.enable_agg = off" "$ANSWERS")"

# DROP EXTENSION: the indexes depend on the access method, so it needs CASCADE; the table stays and nothing of sift is left
check "DROP EXTENSION without CASCADE is refused while the indexes exist" test "$(dbq r2 "DROP EXTENSION pg_sift" 2>&1 | grep -c 'cannot drop extension pg_sift')" == 1
check "DROP EXTENSION .. CASCADE drops the indexes and the definitions, keeps the table and its rows" \
	test "$(dbq r2 "DROP EXTENSION pg_sift CASCADE" "SELECT count(*) FROM events")" == 6000
check "... and no relation, function or trigger of sift is left (PostgreSQL keeps the empty schema of the control file)" \
	test "$(dbq r2 "SELECT (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'sift') + (SELECT count(*) FROM pg_trigger WHERE NOT tgisinternal) + (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'sift')")" == 0
check "a database with the extension dropped is created again and restored from the same dump" \
	test "$(dbq r2 "CREATE EXTENSION pg_sift" "SELECT count(*) FROM sift.projections")" == 0

# ALTER EXTENSION UPDATE from 4.3, which has no definitions: an index that lists a projection is broken as not defined until it is defined
fresh u
check "a database at 4.3 takes an index that lists a projection" test "$(dbq u "CREATE EXTENSION pg_sift VERSION '4.3'" "CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, kind int)" \
	"INSERT INTO events SELECT i, i % 40, i % 5 FROM generate_series(1, 3000) i" "CREATE INDEX events_kind ON events USING sift_col (dim_a_id, kind) WITH (projection = 'by_kind', track_changes = off)" \
	"SELECT count(*) FROM events")" == 3000
check "ALTER EXTENSION UPDATE creates the definitions table and the status; the listed name is not defined, defining it makes it ok" \
	test "$(dbq u "ALTER EXTENSION pg_sift UPDATE" "SELECT reason FROM sift.projection_status" "SELECT sift.projection_create('by_kind', 'events', ARRAY['dim_a_id', 'kind'])" \
	"SELECT state FROM sift.projection_status" | tr '\n' ' ')" == "the projection is not defined  ok "
finish
