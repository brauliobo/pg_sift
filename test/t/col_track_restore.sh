#!/usr/bin/env bash
# Tables with a primary key and a sift_col index (track_changes is on by default and pg_dump writes it as the key's name) restored from pg_dump: the custom archive serially and
# in parallel (pg_restore -j makes the index and the key in either order), a plain dump through psql, and a dump whose indexes come before the keys, the order a restore
# for pg_upgrade has and the one the dumps of 4.0.0 hold. Every restore must be without an error, keep the indexes tracked, and answer as the heap does.
. "$(dirname "$0")/lib.bash"

plan 10
N=$(knob SIFT_RESTORE_TABLES 24 12)
ROWS=$(knob SIFT_RESTORE_ROWS 1500 600)
node_init r 36 "autovacuum = off"
node_start r || bail "start failed"
D=$(dir r)
S=("-h" "$D/sock" "-p" "$(port r)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }

# the number of tables whose grouped read by the column index differs from the heap's, and how many indexes track and read without the heap
CHECK_FN="CREATE OR REPLACE FUNCTION differing() RETURNS int LANGUAGE plpgsql AS \$\$
DECLARE t text; n int := 0; a text; b text;
BEGIN
  FOR t IN SELECT c.relname FROM pg_class c WHERE c.relname ~ '^w[0-9]+\$' AND c.relkind = 'r' ORDER BY 1 LOOP
    SET LOCAL max_parallel_workers_per_gather = 0; SET LOCAL seq_page_cost = 10000;
    SET LOCAL sift.enable_agg = on;
    EXECUTE format('SELECT string_agg(g || '':'' || c || '':'' || s, '','' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM %I GROUP BY g) x', t) INTO a;
    SET LOCAL sift.enable_agg = off;
    EXECUTE format('SELECT string_agg(g || '':'' || c || '':'' || s, '','' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM %I GROUP BY g) x', t) INTO b;
    IF a IS DISTINCT FROM b THEN n := n + 1; END IF;
  END LOOP;
  RETURN n;
END \$\$"
TRACKED="SELECT count(*) FILTER (WHERE (SELECT track_changes FROM sift.col_tracking(c.oid))) || ',' || count(*) FILTER (WHERE (SELECT heap_free FROM sift.col_tracking(c.oid))) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col'"
state() { dbq "$1" "$CHECK_FN" "SELECT differing()" "$TRACKED" | tail -n 2 | tr '\n' ' '; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
fresh() { dbq postgres "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }

{
	echo "CREATE EXTENSION pg_sift;"
	for i in $(seq "$N"); do
		echo "CREATE TABLE w$i (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);"
		echo "INSERT INTO w$i SELECT x, x % 7, x FROM generate_series(1, $ROWS) x;"
		echo "CREATE INDEX w${i}_c ON w$i USING sift_col (id, g, v);"
		echo "UPDATE w$i SET id = id + 100000 WHERE id % 10 = 0; DELETE FROM w$i WHERE id % 13 = 0;"
	done
} >"$D/setup.sql"
fresh src
psql -X -q -v ON_ERROR_STOP=1 "${S[@]}" -d src -f "$D/setup.sql" >/dev/null || bail "setup failed"
WANT="0 $N,$N "
check "the source: $N tracked indexes, none stale, every one reads without the heap" test "$(state src)" == "$WANT"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
check "pg_dump writes the key's name as track_changes" test "$(grep -c 'track_changes=id' "$D/src.sql")" == "$N"

fresh r1
check "pg_restore, serial: no error, tracked and as the heap" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(state r1)" == "$WANT"

ok_runs=0
for run in 1 2 3 4 5; do
	fresh rp
	[[ $(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1 | errors) == 0 && $(state rp) == "$WANT" ]] && ok_runs=$((ok_runs + 1))
done
check "pg_restore -j8, five runs: no error, tracked and as the heap" test "$ok_runs" == 5

fresh r2
check "pg_dump | psql: no error, tracked and as the heap" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(state r2)" == "$WANT"

# the indexes before the keys: the order of pg_upgrade's restore, and of every dump made by 4.0.0 that a parallel restore or pg_upgrade met
for sec in pre-data data post-data; do pg_dump "${S[@]}" -Fp --section=$sec -f "$D/$sec.sql" src; done
python3 -I - "$D/post-data.sql" "$D/post-reordered.sql" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
head, *blocks = re.split(r'(?m)^(?=--\n-- Name: )', text)
idx = [b for b in blocks if 'Type: INDEX;' in b]
rest = [b for b in blocks if 'Type: INDEX;' not in b]
open(sys.argv[2], 'w').write(head + ''.join(idx) + ''.join(rest))
PY
check "the reordered dump has the indexes before the keys" test "$(grep -n 'CREATE INDEX' "$D/post-reordered.sql" | head -n 1 | cut -d: -f1)" -lt "$(grep -n 'PRIMARY KEY' "$D/post-reordered.sql" | head -n 1 | cut -d: -f1)"
fresh r3
RES=$( { for f in pre-data data post-reordered; do psql -X -q "${S[@]}" -d r3 -f "$D/$f.sql" 2>&1; done; } | errors)
check "indexes before the keys: no error, tracked, every one reads without the heap and as the heap does" test "$RES" == 0 -a "$(state r3)" == "$WANT"

dbq r3 "UPDATE w1 SET id = id + 200000 WHERE id % 7 = 3" "DELETE FROM w1 WHERE id % 11 = 0" "INSERT INTO w1 SELECT x, x % 7, x FROM generate_series(900001, 900100) x" >/dev/null
check "after writes the restored indexes still answer as the heap" test "$(dbq r3 "$CHECK_FN" "SELECT differing()" | tail -n 1)" == 0

# a statement that runs before the key exists is not in the log: the log is not complete after the key
fresh r4
psql -X -q -v ON_ERROR_STOP=1 "${S[@]}" -d r4 -f "$D/pre-data.sql" >/dev/null 2>&1
dbq r4 "CREATE INDEX w1_early ON w1 USING sift_col (id, g, v) WITH (track_changes = 'id')" "INSERT INTO w1 SELECT x, x % 7, x FROM generate_series(1, 500) x" \
	"DELETE FROM w1 WHERE id % 2 = 0" "UPDATE w1 SET id = id + 1000 WHERE id % 5 = 1" "ALTER TABLE w1 ADD PRIMARY KEY (id)" >/dev/null
check "rows written before the key exists: reads do not use the log, and the answer is the heap's" test "$(dbq r4 "SELECT heap_free FROM sift.col_tracking('w1_early')")" == f -a "$(dbq r4 "$CHECK_FN" "SELECT differing()" | tail -n 1)" == 0
check "REINDEX gives it back the baseline" test "$(dbq r4 "REINDEX INDEX w1_early" "SELECT heap_free FROM sift.col_tracking('w1_early')" | tail -n 1)" == t
finish
