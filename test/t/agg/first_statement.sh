#!/usr/bin/env bash
# The first statement of a session that has not preloaded the library loads it while the planner opens the column index: the aggregates of
# that statement still run in the scan, like the statements after it. A fresh psql is a fresh backend: nothing is loaded before the statement
# under test, and sift.stats(local => true) reads the scans that backend ran.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

plan 19
node_init af 27 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start af || bail "start failed"
sql af "CREATE EXTENSION pg_sift" \
	"CREATE TABLE dim_a (id int PRIMARY KEY, kind text)" \
	"INSERT INTO dim_a SELECT i, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4] FROM generate_series(1, 60) i" \
	"CREATE TABLE facts (id int, day date, dim_a_id int, deleted_at timestamp, items jsonb)" \
	"INSERT INTO facts SELECT i, '2026-01-01'::date + (i % 15), 1 + i % 70, CASE WHEN i % 17 = 0 THEN now() END,
		jsonb_build_array(jsonb_build_object('qty', i % 5), jsonb_build_object('qty', i % 3)) FROM generate_series(1, 60000) i" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, day, dim_a_id, deleted_at, items) WITH (paths = 'items[]{qty float8}')" \
	"VACUUM (FREEZE, ANALYZE) facts" "ANALYZE dim_a" >/dev/null || bail "setup failed"

# two arms of a UNION ALL, each an aggregate the scan runs
SRC="facts f JOIN dim_a e ON e.id = f.dim_a_id CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS i(qty float8) WHERE f.deleted_at IS NULL"
Q="SELECT * FROM (
  SELECT 'day' AS grp, f.day::text AS k, count(DISTINCT f.id) AS n, sum(i.qty) AS q FROM $SRC GROUP BY f.day
  UNION ALL
  SELECT 'kind', e.kind, count(DISTINCT f.id), sum(i.qty) FROM $SRC GROUP BY e.kind
) q ORDER BY 1, 2"

# session STATEMENT...: one fresh backend runs the statements; the last two lines are the scans it ran and the checksum of the first statement's rows
session() {
	{ for s in "$@"; do echo "$s;"; done; echo "SELECT 'scans', value FROM sift.stats(local => true) WHERE name = 'scans';"; } | sql af
}
scans() { sed -n 's/^scans|//p' <<<"$1"; }

on=$(session "$Q")
check "the first statement of a session runs in the scan" test "$(scans "$on")" -ge 1
off=$(session "SET sift.enable_agg = off" "$Q")
check "with the pushdown off it does not" test "$(scans "$off")" == 0
check "the rows are those of the normal plan" test "$(grep -v '^scans|' <<<"$on" | "$REPO/scripts/sha256.sh")" == "$(grep -v '^scans|' <<<"$off" | "$REPO/scripts/sha256.sh")"

two=$(session "$Q" "$Q")
check "the second statement runs in the scan too" test "$(scans "$two")" -ge 2
pre=$(session "PREPARE q AS $Q" "EXECUTE q" "EXECUTE q" "EXECUTE q")
check "a prepared statement is not left with the plan made without the hook" test "$(scans "$pre")" -ge 3
multi=$(session "SELECT 1" "$Q")
check "a statement after one that did not touch the index is planned the same" test "$(scans "$multi")" -ge 1
param=$(session "PREPARE p(int) AS SELECT count(*) FROM facts WHERE dim_a_id = \$1" "EXECUTE p(3)" "SELECT count(*) FROM facts WHERE dim_a_id = 3")
check "a prepared statement with a parameter returns the rows of the literal one" test "$(grep -c '^[0-9]' <<<"$param")" == 2

# the same statement with a parameter, as the extended protocol (psql \bind) and EXECUTE send it: planned with the parameters' types and values
QP="${Q//IS NULL/IS NULL AND f.day >= \$1::date}"
bound=$({ printf '%s \\bind %s \\g\n' "$QP" "'2026-01-05'"; echo "SELECT 'scans', value FROM sift.stats(local => true) WHERE name = 'scans';"; } | sql af)
check "the first statement of a session with a bound parameter runs in the scan" test "$(scans "$bound")" -ge 1
boundoff=$({ echo "SET sift.enable_agg = off;"; printf '%s \\bind %s \\g\n' "$QP" "'2026-01-05'"; } | sql af | grep -v '^SET$')
check "its rows are those of the normal plan" test "$(grep -v '^scans|' <<<"$bound" | "$REPO/scripts/sha256.sh")" == "$("$REPO/scripts/sha256.sh" <<<"$boundoff")"
prep=$(session "PREPARE pq(date) AS $QP" "EXECUTE pq('2026-01-05')" "EXECUTE pq('2026-01-05')" "EXECUTE pq('2026-01-05')")
check "a prepared statement with a parameter runs in the scan from its first execution" test "$(scans "$prep")" -ge 3
# the plan EXPLAIN shows is the one the statement gets, whether it runs or not; CREATE TABLE AS plans its query like a SELECT
exp=$(session "EXPLAIN (COSTS OFF) $Q")
check "EXPLAIN as the first statement of a session shows the scan" test "$(grep -c SiftAgg <<<"$exp")" -ge 1
expa=$(session "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $Q")
check "EXPLAIN ANALYZE as the first statement of a session shows the scan and runs it" test "$(grep -c SiftAgg <<<"$expa")" -ge 1 -a "$(scans "$expa")" -ge 1
ctas=$(session "CREATE TEMP TABLE first_ctas AS $Q" "SELECT count(*) FROM first_ctas")
check "CREATE TABLE AS as the first statement of a session runs its query in the scan" test "$(scans "$ctas")" -ge 1
ctasoff=$(session "SET sift.enable_agg = off" "CREATE TEMP TABLE first_ctas AS $Q" "SELECT count(*) FROM first_ctas")
check "and creates the rows of the normal plan" test "$(grep -v '^scans|' <<<"$ctas")" == "$(grep -v '^scans|' <<<"$ctasoff")"
# an EXPLAIN that follows other statements of its query string (psql \;) is planned again from its own text, not from the first statement of the string
expm=$({ echo "SELECT 1 \\; EXPLAIN (COSTS OFF) $Q;"; } | sql af)
check "EXPLAIN after another statement of the same query string shows the scan" test "$(grep -c SiftAgg <<<"$expm")" -ge 1
expset=$({ echo "SET enable_seqscan = on \\; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $Q;"; echo "SELECT 'scans', value FROM sift.stats(local => true) WHERE name = 'scans';"; } | sql af)
check "EXPLAIN ANALYZE after a SET of the same query string runs the scan" test "$(grep -c SiftAgg <<<"$expset")" -ge 1 -a "$(scans "$expset")" -ge 1
# a table the cursor below reads
sql af "CREATE TABLE lfacts (id int PRIMARY KEY, g int, items jsonb)" \
	"INSERT INTO lfacts SELECT i, i % 9, jsonb_build_array(jsonb_build_object('qty', i % 5), jsonb_build_object('qty', i % 3)) FROM generate_series(1, 20000) i" \
	"CREATE INDEX lfacts_col ON lfacts USING sift_col (id, g, items) WITH (paths = 'items[]{qty float8}')" "VACUUM (FREEZE, ANALYZE) lfacts" >/dev/null || bail "cursor setup failed"
# a scrollable cursor is planned with the Materialize that its backward fetches need, which a plan made again from the text would lack (an assert build stops at ExecInitNestLoop)
scroll=$(session "DO \$\$ DECLARE c refcursor; r record; fwd text := ''; bwd text := ''; BEGIN
	OPEN c SCROLL FOR SELECT v.g, x.id FROM generate_series(0, 8) v(g), LATERAL (SELECT id FROM lfacts WHERE g = v.g ORDER BY id LIMIT 1) x;
	LOOP FETCH NEXT FROM c INTO r; EXIT WHEN NOT FOUND; fwd := fwd || r::text; END LOOP;
	LOOP FETCH PRIOR FROM c INTO r; EXIT WHEN NOT FOUND; bwd := r::text || bwd; END LOOP;
	RAISE WARNING 'cursor: %', CASE WHEN fwd = bwd AND fwd <> '' THEN 'backward is the same' ELSE 'DIFFERS' END; END \$\$" 2>&1)
check "a scrollable cursor as the first statement of a session is not planned again, and fetches backward what it fetched forward" test "$(grep -c 'backward is the same' <<<"$scroll")" == 1
check "the server is up" test "$(sql af "SELECT 1")" == 1
check "the server log shows no error" test "$(grep -c -E 'ERROR|FATAL|PANIC' "$(dir af)/postgres.log")" == 0

finish
