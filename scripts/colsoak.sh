#!/usr/bin/env bash
# Soak test of the column index under writers, VACUUM and maintenance, on the scripts/pgtest.sh cluster.
#
#   scripts/colsoak.sh [--duration SEC] [--clients N]
#
# pgbench clients insert, update (indexed columns, so every update is a new TID) and delete, run VACUUM, and promote and merge the index,
# while checkers run in REPEATABLE READ transactions: one snapshot, the plain plan, then SiftAgg, then the plain plan again, with
# concurrent maintenance between them. The three results must be equal (a VACUUM that removes tuples and turns their pages all-visible
# between the snapshot and the reads must not change what the snapshot sees). At the end the index is verified and the
# totals of the plain plan and SiftAgg compared. Exit 1 on a difference, a verification error, a failed transaction or a backend whose
# private memory keeps growing (scripts/leakcheck.sh).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/leakcheck.sh"
eval "$("$REPO/scripts/pgtest.sh" env)"
export PGDATABASE=sift_colsoak PGOPTIONS='-c client_min_messages=warning'
OUT=${SIFT_TMP:-/tmp/sift-arch}/colsoak
DURATION=300 CLIENTS=8
while (( $# )); do
	case $1 in
		--duration) DURATION=$2 ;; --clients) CLIENTS=$2 ;;
		*) sed -n '2,12p' "$0"; exit 1 ;;
	esac
	shift 2
done

q() { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
log() { printf '[colsoak %s] %s\n' "$(date +%T)" "$*" | tee -a "$OUT/colsoak.log" >&2; }

rm -rf "$OUT" && mkdir -p "$OUT"
psql -X -qAt -d postgres -c "DROP DATABASE IF EXISTS sift_colsoak WITH (FORCE)" -c "CREATE DATABASE sift_colsoak"
q <<-'SQL'
	CREATE EXTENSION pg_sift;
	CREATE TABLE s (id bigserial, g int, n int, f float8, t text) WITH (autovacuum_enabled = off);
	CREATE TABLE bad (what text);
	INSERT INTO s (g, n, f, t) SELECT i % 7, i % 100, i / 8.0, 'tag' || i % 40 FROM generate_series(1, 60000) i;
	CREATE INDEX s_col ON s USING sift_col (id, g, n, f, t) WITH (write_buffer_limit = 2048);
	VACUUM ANALYZE s;
	CREATE FUNCTION chk(q text) RETURNS void LANGUAGE plpgsql AS $f$
	DECLARE a text; b text; c text;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE q INTO a;
		PERFORM pg_sleep(random() * 0.15);
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE q INTO b;
		PERFORM pg_sleep(random() * 0.15);
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE q INTO c;
		IF a IS DISTINCT FROM b OR a IS DISTINCT FROM c THEN
			INSERT INTO bad VALUES (format('%s: plain %s / SiftAgg %s / plain %s', q, a, b, c));
		END IF;
	END $f$;
SQL

cat >"$OUT/check.sql" <<-'SQL'
	\set v random(0, 3)
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SET LOCAL cpu_tuple_cost = 0.5;
	SET LOCAL max_parallel_workers_per_gather = 0;
	SELECT chk(CASE :v
		WHEN 0 THEN 'SELECT count(*)::text || '':'' || sum(n)::text || '':'' || sum(g)::text FROM s'
		WHEN 1 THEN 'SELECT string_agg(x, '','' ORDER BY x) FROM (SELECT g::text || '':'' || count(*)::text || '':'' || sum(n)::text AS x FROM s GROUP BY g) z'
		WHEN 2 THEN 'SELECT count(*)::text || '':'' || count(DISTINCT t)::text || '':'' || min(id)::text || max(id)::text FROM s WHERE g IN (1, 2, 3)'
		ELSE 'SELECT count(*)::text || '':'' || sum(f)::text FROM s WHERE n < 50 AND t = ''tag7''' END);
	COMMIT;
SQL
cat >"$OUT/write.sql" <<-'SQL'
	\set k random(1, 60000)
	INSERT INTO s (g, n, f, t) SELECT i % 7, i % 100, i / 8.0, 'tag' || i % 40 FROM generate_series(1, 60) i;
	UPDATE s SET n = n + 1, g = (g + 1) % 7 WHERE id BETWEEN :k AND :k + 80;
	DELETE FROM s WHERE id BETWEEN :k + 100 AND :k + 130;
SQL
echo "VACUUM s;" >"$OUT/vacuum.sql"
echo "SELECT sift.col_promote('s_col') >= 0, sift.col_merge('s_col') >= 0;" >"$OUT/maint.sql"

soak_backends() { q -c "SELECT pid FROM pg_stat_activity WHERE backend_type = 'client backend' AND datname = 'sift_colsoak' AND pid <> pg_backend_pid()"; }
leak_watch_start "$OUT/memory.tsv" soak_backends
trap 'kill $LEAK_WATCH 2>/dev/null || true' EXIT
pgbench -n -T "$DURATION" -c "$CLIENTS" -j 4 -f "$OUT/check.sql@6" -f "$OUT/write.sql@6" -f "$OUT/vacuum.sql@3" -f "$OUT/maint.sql@3" >"$OUT/pgbench.txt" 2>&1 || true
trap - EXIT
leak_watch_stop "$OUT/memory.tsv" >"$OUT/memory.txt" || { log "MEMORY GROWTH (see memory.txt)"; bad=1; }

bad=${bad:-0}
failed=$(awk '/^number of failed transactions/ { print $5; exit }' "$OUT/pgbench.txt")
checks=$(awk '/^number of transactions actually processed/ { print $6; exit }' "$OUT/pgbench.txt")
diffs=$(q -c "SELECT count(*) FROM bad")
q -c "SELECT what FROM bad LIMIT 5" >&2
q -c "VACUUM s" -c "SELECT sift.col_verify('s_col', true)" >/dev/null 2>"$OUT/verify.err" || { log "VERIFY FAILED: $(head -1 "$OUT/verify.err")"; bad=1; }
totals=$(q -c "SET sift.enable_col = off; SET sift.enable_agg = off; SELECT count(*), sum(n) FROM s" | tail -1)
agg=$(q -c "SET cpu_tuple_cost = 0.5; SET max_parallel_workers_per_gather = 0; SELECT count(*), sum(n) FROM s" | tail -1)
[[ $totals == "$agg" ]] || { log "TOTALS DIFFER: plain $totals / SiftAgg $agg"; bad=1; }
log "$(grep -c . "$OUT/memory.txt") backends sampled for memory; $(grep -m1 -E 'tps = ' "$OUT/pgbench.txt")"
[[ $diffs == 0 && ${failed:-?} == 0 && $bad == 0 ]] || { log "FAIL: $diffs checks differ, ${failed:-?} failed transactions"; exit 1; }
log "PASS (${checks:-?} transactions, the checks among them all equal)"
