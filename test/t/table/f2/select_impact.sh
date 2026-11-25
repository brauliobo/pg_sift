#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# What UPDATE and DELETE churn does to the reads of Format Major 2 and BM25 (Spec 97, track 2), and whether the reads come back once the maintenance has settled. Six read shapes, each
# measured clean, at three churn points (1x, 2x, 4x: cycles of narrow column-delta updates of 12.5% of the rows each, or deletes of 10%, 25% and 50%) and after the VACUUM and the
# maintenance worker settled, by the median buffers (EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)) and the median execution time of the runs of one session, and against an oracle that
# never took the churn path (a heap twin, the heap plan, the score scan). The reads are the statements a user writes, planned as the planner plans them:
#   1 pruned time window of a multi-part sift_table   2 SiftAgg GROUP BY over a column no update touched, the patched one, the pre-aggregates   3 top-N ORDER BY .. LIMIT 20
#   4 scope_v6 of one key and of every key            5 point read of a sift_table under column deltas                                           6 BM25 ranked search
# Every case prints CHURN_ROW case | 1x | 2x | 4x | growth_factor | limit | verdict as a TAP comment. The limits are those of Spec 97: dirty reads <= 2.0x the clean ones (2.5x
# for the pruned window), the third cycle <= 1.25x the first, and the settled reads <= 1.15x the clean ones. A time passes when its median or its best run is within the limit of the clean one plus 3 ms:
# a read of a few milliseconds is not a measurement of the host that runs a test beside others.
. "$(dirname "$0")/lib.bash"

plan 30
node_init cs 76 "autovacuum = off
shared_buffers = 64MB
max_parallel_workers_per_gather = 0
maintenance_work_mem = '4MB'
temp_file_limit = '1MB'
sift.backend_cache_kb = 2048
sift.async_maintenance = on
sift.maintenance_naptime = 200"
node_start cs || bail "start failed"

declare -A V
declare -A RUNS=([topn]=3)
SLACK_MS=3

# measure_all STAGE CASE=QUERY...: the median time, the median buffers and the best time of the runs (5, RUNS) of every QUERY, in one session
measure_all() {
	local stage=$1 spec out r i=0 names=() stmts=()
	shift
	for spec; do names+=("${spec%%=*}"); stmts+=("SELECT churn_measure(\$q\$${spec#*=}\$q\$, ${RUNS[${spec%%=*}]:-5})"); done
	out=$(sql cs "LOAD 'pg_sift'" "${stmts[@]}") || bail "measure $stage failed: $out"
	for r in $out; do IFS='|' read -r V[ms.${names[i]}.$stage] V[bf.${names[i]}.$stage] V[mn.${names[i]}.$stage] <<<"$r"; i=$((i + 1)); done
	((i == ${#names[@]})) || bail "measure $stage: ${#names[@]} cases, $i answers: $out"
}

# emit KIND CASE LIMIT S1 S2 S4 BASE: the rows of the buffers and of the time of CASE; false when one is over LIMIT.
# dirty: the largest of the three churn points over BASE; cycles: the third over the first; post: S4 over BASE. A time row shows the medians and passes by the medians or by the best runs
emit() {
	local kind=$1 name=$2 limit=$3 s1=$4 s2=$5 s4=$6 base=$7 m slack ok=0 line
	for m in bf ms; do
		slack=0; [[ $m == ms ]] && slack=$SLACK_MS
		line=$(awk -v kind="$kind" -v name="${name}_$m" -v limit="$limit" -v slack="$slack" -v a="${V[$m.$name.$s1]}" -v b="${V[$m.$name.$s2]}" -v c="${V[$m.$name.$s4]}" \
			-v base="${V[$m.$name.$base]}" -v na="${V[mn.$name.$s1]}" -v nb="${V[mn.$name.$s2]}" -v nc="${V[mn.$name.$s4]}" -v nbase="${V[mn.$name.$base]}" '
			function over(a, b, c, base, first,   mx, v, den) {
				mx = a; if (b > mx) mx = b; if (c > mx) mx = c
				v = kind == "dirty" ? mx : c; den = kind == "cycles" ? first : base
				return v > limit * den + slack }
			BEGIN {
				mx = a; if (b > mx) mx = b; if (c > mx) mx = c
				v = kind == "dirty" ? mx : c; den = kind == "cycles" ? a : base
				fail = over(a, b, c, base, a) && (name !~ /_ms$/ || over(na, nb, nc, nbase, na))
				printf "CHURN_ROW %s_%s | %s | %s | %s | %.3f | %.2f | %s", name, kind, a, b, c, v / (den ? den : 1), limit, fail ? "FAIL" : "PASS" }')
		diag "$line"
		[[ $line == *PASS ]] || ok=1
	done
	return $ok
}
# emit_all KIND LIMIT S1 S2 S4 BASE CASE...: emit for every case; false when one failed
emit_all() { local kind=$1 limit=$2 s1=$3 s2=$4 s4=$5 base=$6 name rc=0; shift 6; for name; do emit "$kind" "$name" "$limit" "$s1" "$s2" "$s4" "$base" || rc=1; done; return $rc; }

# digests SETTING QUERY...: the md5 of the rows of every QUERY, read in one session under SETTING
digests() {
	local setting=$1 q stmts=()
	shift
	for q; do stmts+=("SELECT coalesce(md5(string_agg(r::text, '|' ORDER BY r::text)), '') FROM ($q) r"); done
	sql cs "LOAD 'pg_sift'" "$setting" "${stmts[@]}" 2>&1
}
# the rows of every QUERY are the rows the oracle reads: the same md5 line by line, as out for a failed check
same_digests() { out="$1"$'\n'"$2"; [[ $1 == "$2" && -n $1 && $1 != *ERROR* ]]; }
# same_rows TABLE ORACLE QUERY...: every QUERY (with @T for the table) answers on TABLE the rows it answers on the table ORACLE
same_rows() { local t=$1 oracle=$2; shift 2; same_digests "$(digests "SET sift.enable_agg = on" "${@//@T/$t}")" "$(digests "SET sift.enable_agg = on" "${@//@T/$oracle}")"; }
# same_plain QUERY...: every QUERY answers with the aggregate the rows of the heap plan
same_plain() { same_digests "$(digests "SET sift.enable_agg = on" "$@")" "$(digests "SET sift.enable_agg = off" "$@")"; }
# same_scan QUERY: the ranked search answers the rows of the score scan of the heap
same_scan() { same_digests "$(digests "SET sift.enable_scan = on" "$1")" "$(digests "SET sift.enable_scan = off" "$1")"; }
# settle RELATION STATE: the VACUUM of RELATION, then the background work has ended and stopped changing what the query STATE shows
settle() { sql cs "VACUUM $1" >/dev/null && wait_stable cs "$2" 60; }
both() { sql cs "${1//@T/fact_tbl}" "${1//@T/fact_ref}" >/dev/null || bail "churn failed: $1"; }
# the 10%, then 25%, then 50% of the rows by id deleted
DELETE_STEPS=("id % 10 = 0" "id % 20 IN (1, 3, 5)" "id % 20 IN (7, 9, 11, 13, 15)")

sql cs "CREATE EXTENSION pg_sift" "
CREATE FUNCTION churn_measure(q text, runs int) RETURNS text LANGUAGE plpgsql AS \$f\$
DECLARE j json; ms float8[] := '{}'; bf bigint[] := '{}';
BEGIN
	FOR i IN 1..runs LOOP
		EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || q INTO j;
		ms := ms || (j->0->>'Execution Time')::float8;
		bf := bf || (coalesce((j->0->'Plan'->>'Shared Hit Blocks')::bigint, 0) + coalesce((j->0->'Plan'->>'Shared Read Blocks')::bigint, 0));
	END LOOP;
	RETURN (SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY m)::numeric, 3) FROM unnest(ms) m) || '|' || (SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY b) FROM unnest(bf) b)
		|| '|' || (SELECT round(min(m)::numeric, 3) FROM unnest(ms) m);
END \$f\$" >/dev/null || bail "extension failed"

# 1, 2, 3, 5: a sift_table of 60000 rows in four parts of fifteen days, and the heap twin that takes the same statements
sql cs "CREATE TABLE fact_tbl (id bigint PRIMARY KEY, ts timestamptz NOT NULL, grp int, x bigint, y bigint, label text) USING sift_table" \
	"SELECT sift.table_set_option('fact_tbl', 'key_order', 'ts, id')" >/dev/null || bail "table failed"
for lo in 1 15001 30001 45001; do
	sql cs "INSERT INTO fact_tbl SELECT g, '2026-01-01'::timestamptz + (g / 1000) * interval '1 day', g % 64, g % 1000, (g * 7) % 500, 'cat_' || (g % 16) FROM generate_series($lo, $((lo + 14999))) g" >/dev/null
done
sql cs "CREATE TABLE fact_ref AS SELECT * FROM fact_tbl" "VACUUM (FREEZE, ANALYZE) fact_tbl" "ANALYZE fact_ref" >/dev/null || bail "load failed"
TBL_STATE="SELECT count(*) || '/' || coalesce(sum(bytes), 0) FROM sift.table_segments('fact_tbl')"
wait_stable cs "$TBL_STATE" 60

Q_WIN="SELECT count(*), sum(y) FROM @T WHERE ts >= '2026-02-20' AND ts < '2026-02-25'"
Q_UNT="SELECT grp, count(*), sum(y) FROM @T GROUP BY grp ORDER BY grp"
Q_PAT="SELECT grp, sum(x) FROM @T GROUP BY grp ORDER BY grp"
Q_TOP="SELECT id, ts, y FROM @T ORDER BY ts DESC, id DESC LIMIT 20"
Q_PNT="SELECT id, x, y FROM @T WHERE id = 52000"
READS=("$Q_WIN" "$Q_UNT" "$Q_PAT" "$Q_TOP" "$Q_PNT")
measure_tbl() { measure_all "$1" "window=${Q_WIN//@T/fact_tbl}" "untouched=${Q_UNT//@T/fact_tbl}" "patched=${Q_PAT//@T/fact_tbl}" "topn=${Q_TOP//@T/fact_tbl}" "point=${Q_PNT//@T/fact_tbl}"; }
measure_tbl clean

# the time of one row moved: the parts that hold no row of the window keep out of its reads
both "UPDATE @T SET ts = ts + interval '1 second' WHERE id = 10"
measure_all t1 "window=${Q_WIN//@T/fact_tbl}"
check "1 pruned time window: one row moved in time reads <= 2.5x the clean one" emit dirty window 2.50 t1 t1 t1 clean

# 1x, 2x, 4x: cycles of narrow updates of the column x of an eighth of the rows each (12.5%, 25%, 50%)
for c in 1 2 3 4; do
	both "UPDATE @T SET x = x + 1 WHERE id % 8 = $((c - 1))"
	case $c in 1) measure_tbl c1 ;; 2) measure_tbl c2 ;; 4) measure_tbl c4 ;; esac
done
check "sift_table with 50% narrow updates: every read answers the rows of the heap twin" same_rows fact_tbl fact_ref "${READS[@]}"
settle fact_tbl "$TBL_STATE"
measure_tbl post
check "sift_table settled: every read answers the rows of the heap twin" same_rows fact_tbl fact_ref "${READS[@]}"

check "1 pruned time window: reads <= 2.5x the clean ones under column deltas" emit_all dirty 2.50 c1 c2 c4 clean window
check "1 pruned time window: the 4th cycle reads <= 1.25x the first" emit_all cycles 1.25 c1 c2 c4 clean window
check "1 pruned time window: settled reads <= 1.15x the clean ones" emit_all post 1.15 clean c4 post clean window
check "2 SiftAgg of the untouched and the patched column: reads <= 2.0x the clean ones under column deltas" emit_all dirty 2.00 c1 c2 c4 clean untouched patched
check "2 SiftAgg of the untouched and the patched column: the 4th cycle reads <= 1.25x the first" emit_all cycles 1.25 c1 c2 c4 clean untouched patched
check "2 SiftAgg of the untouched and the patched column: settled reads <= 1.15x the clean ones" emit_all post 1.15 clean c4 post clean untouched patched
check "3 top-N: reads <= 2.0x the clean ones under column deltas" emit_all dirty 2.00 c1 c2 c4 clean topn
check "3 top-N: the 4th cycle reads <= 1.25x the first" emit_all cycles 1.25 c1 c2 c4 clean topn
check "5 point read: reads <= 2.0x the clean ones under column deltas" emit_all dirty 2.00 c1 c2 c4 clean point
check "5 point read: the 4th cycle reads <= 1.25x the first" emit_all cycles 1.25 c1 c2 c4 clean point
check "5 point read: settled reads <= 1.15x the clean ones" emit_all post 1.15 clean c4 post clean point

# 25% and 50% of the rows deleted from the settled table
both "DELETE FROM @T WHERE id % 4 = 1"
measure_all d25 "topn=${Q_TOP//@T/fact_tbl}"
both "DELETE FROM @T WHERE id % 4 = 2"
measure_all d50 "topn=${Q_TOP//@T/fact_tbl}"
check "sift_table with 50% deleted: every read answers the rows of the heap twin" same_rows fact_tbl fact_ref "${READS[@]}"
settle fact_tbl "$TBL_STATE"
measure_all dpost "topn=${Q_TOP//@T/fact_tbl}"
check "3 top-N: reads <= 2.0x the clean ones with 25% and 50% of the rows deleted" emit_all dirty 2.00 d25 d50 d50 post topn
check "3 top-N: settled reads <= 1.15x the clean ones after the deletes" emit_all post 1.15 post d50 dpost post topn

# 2, 4: a sift_col index with pre-aggregates and a scope on a heap table, 10%, 25% and 50% of the facts deleted
sql cs "CREATE TABLE dim_scope (member_id int NOT NULL, scope_id int NOT NULL)" "CREATE UNIQUE INDEX dim_scope_u ON dim_scope (member_id, scope_id)" \
	"INSERT INTO dim_scope SELECT g, g % 50 FROM generate_series(1, 1000) g" \
	"CREATE TABLE fact_col (id int PRIMARY KEY, member_id int NOT NULL, ts timestamptz NOT NULL, grp int, x int, y int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO fact_col SELECT g, 1 + (g % 1000), '2026-01-01'::timestamptz + (g / 800) * interval '1 day', g % 32, g % 100, g % 500 FROM generate_series(1, 40000) g" \
	"CREATE INDEX fact_col_idx ON fact_col USING sift_col (id, member_id, ts, grp, x, y) WITH (key_order = 'ts, id', preagg = 'auto', scope = 's0: member_id -> dim_scope(member_id, scope_id)', track_changes = 'ctid')" \
	"VACUUM (FREEZE, ANALYZE) fact_col, dim_scope" >/dev/null || bail "sift_col failed"
COL_STATE="SELECT count(*) || '/' || coalesce(sum(bytes), 0) || '/' || coalesce(sum(dead_rows), 0) FROM sift.col_segments('fact_col_idx')"
wait_stable cs "$COL_STATE" 60
Q_PRE="SELECT grp, count(*), sum(y) FROM fact_col GROUP BY grp ORDER BY grp"
Q_ONE="SELECT count(*), sum(f.y) FROM fact_col f JOIN dim_scope d ON d.member_id = f.member_id WHERE d.scope_id = 7"
Q_KEYS="SELECT d.scope_id, count(*), sum(f.y) FROM fact_col f JOIN dim_scope d ON d.member_id = f.member_id GROUP BY d.scope_id ORDER BY d.scope_id"
measure_col() { measure_all "$1" "preagg=$Q_PRE" "scope1=$Q_ONE" "scopekeys=$Q_KEYS"; }
delete_col() { sql cs "DELETE FROM fact_col WHERE ${DELETE_STEPS[$1]}" >/dev/null || bail "delete failed"; }
measure_col clean
delete_col 0
measure_col d10
settle fact_col "$COL_STATE"
measure_col p10
delete_col 1
measure_col d25
delete_col 2
measure_col d50
check "sift_col with 50% deleted: every aggregate answers the rows of the heap plan" same_plain "$Q_PRE" "$Q_ONE" "$Q_KEYS"
settle fact_col "$COL_STATE"
measure_col p50
check "sift_col settled: every aggregate answers the rows of the heap plan" same_plain "$Q_PRE" "$Q_ONE" "$Q_KEYS"
check "2 SiftAgg pre-aggregates: reads <= 2.0x the clean ones with 10%, 25% and 50% of the rows deleted" emit_all dirty 2.00 d10 d25 d50 clean preagg
check "2 SiftAgg pre-aggregates: settled reads <= 1.15x the clean ones after 10% deleted" emit_all post 1.15 clean d10 p10 clean preagg
check "2 SiftAgg pre-aggregates: settled reads <= 1.15x the clean ones after 50% deleted" emit_all post 1.15 clean d50 p50 clean preagg
check "4 scope_v6 of one key and of every key: reads <= 2.0x the clean ones with 10%, 25% and 50% of the facts deleted" emit_all dirty 2.00 d10 d25 d50 clean scope1 scopekeys
check "4 scope_v6 of one key and of every key: settled reads <= 1.15x the clean ones after 10% deleted" emit_all post 1.15 clean d10 p10 clean scope1 scopekeys
check "4 scope_v6 of one key and of every key: settled reads <= 1.15x the clean ones after 50% deleted" emit_all post 1.15 clean d50 p50 clean scope1 scopekeys

# 6: a BM25 index, the 25% of the documents with the highest scores deleted, then 25% updated
sql cs "CREATE TABLE docs_bm25 (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO docs_bm25 SELECT g, CASE WHEN g % 40 = 0 THEN 'alpha alpha alpha alpha' ELSE 'alpha beta ' || repeat('filler' || (g % 200) || ' ', 12 + (g % 20)) END FROM generate_series(1, 20000) g" \
	"CREATE INDEX docs_bm25_idx ON docs_bm25 USING sift (body)" "VACUUM (ANALYZE) docs_bm25" >/dev/null || bail "bm25 failed"
BM_STATE="SELECT count(*) || '/' || coalesce(sum(bytes), 0) || '/' || coalesce(sum(dead_docs), 0) FROM sift.segments('docs_bm25_idx')"
wait_stable cs "$BM_STATE" 60
Q_BM="SELECT id FROM docs_bm25 WHERE body @@> 'alpha' ORDER BY sift.score(ctid) DESC LIMIT 20"
measure_all clean "bm25=$Q_BM"
sql cs "DELETE FROM docs_bm25 WHERE id % 4 = 0" >/dev/null || bail "delete failed"
measure_all d25 "bm25=$Q_BM"
check "sift with 25% deleted: the ranked search answers the rows of the score scan" same_scan "$Q_BM"
settle docs_bm25 "$BM_STATE"
measure_all p25 "bm25=$Q_BM"
sql cs "UPDATE docs_bm25 SET body = body || ' gamma' WHERE id % 4 = 1" >/dev/null || bail "update failed"
measure_all u25 "bm25=$Q_BM"
settle docs_bm25 "$BM_STATE"
measure_all p50 "bm25=$Q_BM"
check "sift settled after 25% deleted and 25% updated: the ranked search answers the rows of the score scan" same_scan "$Q_BM"
check "6 BM25 search: reads <= 2.0x the clean ones with 25% deleted and 25% updated" emit_all dirty 2.00 d25 u25 u25 clean bm25
check "6 BM25 search: reads <= 1.15x the clean ones once the 25% deleted are settled" emit_all post 1.15 clean d25 p25 clean bm25
check "6 BM25 search: reads <= 1.15x the clean ones once the 25% deleted and the 25% updated are settled" emit_all post 1.15 clean u25 p50 clean bm25

diag "wall clock ${SECONDS}s"
finish
