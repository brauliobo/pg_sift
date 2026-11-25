#!/usr/bin/env bash
# tap: pooled
# Partitioned tables with a primary key and a sift_col index made through the parent (every partition's index tracks its changes, with its own triggers and those of the parent) restored from pg_dump:
# the custom archive serially and in parallel (pg_restore -j makes the partitions, their indexes and their keys in any order), a plain dump through psql, and a dump whose indexes come before the keys.
# Every restore must be without an error, keep every partition's index tracked with the triggers of its parent, answer as the heap does, and keep answering after updates, key changes that
# move rows between partitions, and deletes through the parent.
. "$(dirname "$0")/lib.bash"

plan 6
N=$(knob SIFT_RESTORE_TABLES 6 3)
ROWS=$(knob SIFT_RESTORE_ROWS 1500 600)
PARTS=4
LEAVES=$((N * PARTS))
node_init r 38 "autovacuum = off"
node_start r || bail "start failed"
D=$(dir r)
S=("-h" "$(node_sock r)" "-p" "$(port r)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }

# the number of partitions whose grouped read by the column index differs from the heap's
CHECK_FN="CREATE OR REPLACE FUNCTION differing() RETURNS int LANGUAGE plpgsql AS \$\$
DECLARE t text; n int := 0; a text; b text;
BEGIN
  FOR t IN SELECT c.relname FROM pg_class c WHERE c.relname ~ '^w[0-9]+_[0-9]+\$' AND c.relkind = 'r' ORDER BY 1 LOOP
    SET LOCAL max_parallel_workers_per_gather = 0; SET LOCAL seq_page_cost = 10000;
    SET LOCAL sift.enable_agg = on;
    EXECUTE format('SELECT string_agg(g || '':'' || c || '':'' || s, '','' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM %I GROUP BY g) x', t) INTO a;
    SET LOCAL sift.enable_agg = off;
    EXECUTE format('SELECT string_agg(g || '':'' || c || '':'' || s, '','' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM %I GROUP BY g) x', t) INTO b;
    IF a IS DISTINCT FROM b THEN n := n + 1; END IF;
  END LOOP;
  RETURN n;
END \$\$"
# the indexes of the partitions: how many track and read without the heap; the track_changes triggers: two on each partition and two of its parent for each
TRACKED="SELECT count(*) FILTER (WHERE (SELECT track_changes FROM sift.col_tracking(c.oid))) || ',' || count(*) FILTER (WHERE (SELECT heap_free FROM sift.col_tracking(c.oid))) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' AND c.relkind = 'i'"
TRIGGERS="SELECT count(*) FROM pg_trigger WHERE tgfoid = 'sift.col_track_old_rows'::regproc"
state() { dbq "$1" "$CHECK_FN" "SELECT differing()" "$TRACKED" "$TRIGGERS" | tail -n 3 | tr '\n' ' '; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
fresh() { dbq "$(node_db r)" "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }

{
	echo "CREATE EXTENSION pg_sift;"
	for i in $(seq "$N"); do
		echo "CREATE TABLE w$i (id int PRIMARY KEY, g int NOT NULL, v int) PARTITION BY RANGE (id);"
		for j in $(seq "$PARTS"); do
			echo "CREATE TABLE w${i}_$j PARTITION OF w$i FOR VALUES FROM ($(((j - 1) * ROWS * 2))) TO ($((j * ROWS * 2))) WITH (autovacuum_enabled = false);"
		done
		echo "INSERT INTO w$i SELECT x, x % 7, x FROM generate_series(0, $((PARTS * ROWS * 2 - 1)), 3) x;"
		echo "CREATE INDEX w${i}_c ON w$i USING sift_col (id, g, v);"
		echo "UPDATE w$i SET id = id + 1 WHERE id % 30 = 0; DELETE FROM w$i WHERE id % 14 = 0;"
	done
} >"$D/setup.sql"
fresh src
psql -X -q -v ON_ERROR_STOP=1 "${S[@]}" -d src -f "$D/setup.sql" >/dev/null || bail "setup failed"
WANT="0 $LEAVES,$LEAVES $((LEAVES * 4)) "
check "the source: $LEAVES tracked partition indexes, none stale, every one reads without the heap, 4 triggers each" test "$(state src)" == "$WANT"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src

fresh r1
check "pg_restore, serial: no error, tracked with the triggers of the parents, as the heap" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(state r1)" == "$WANT"

ok_runs=0
for run in 1 2 3 4 5; do
	fresh rp
	[[ $(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1 | errors) == 0 && $(state rp) == "$WANT" ]] && ok_runs=$((ok_runs + 1))
done
check "pg_restore -j8, five runs: no error, tracked with the triggers of the parents, as the heap" test "$ok_runs" == 5

fresh r2
check "pg_dump | psql: no error, tracked with the triggers of the parents, as the heap" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(state r2)" == "$WANT"

# the indexes before the keys: the order of pg_upgrade's restore
for sec in pre-data data post-data; do pg_dump "${S[@]}" -Fp --section=$sec -f "$D/$sec.sql" src; done
python3 -I - "$D/post-data.sql" "$D/post-reordered.sql" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
head, *blocks = re.split(r'(?m)^(?=--\n-- Name: )', text)
idx = [b for b in blocks if 'Type: INDEX;' in b]
rest = [b for b in blocks if 'Type: INDEX;' not in b]
open(sys.argv[2], 'w').write(head + ''.join(idx) + ''.join(rest))
PY
fresh r3
RES=$( { for f in pre-data data post-reordered; do psql -X -q "${S[@]}" -d r3 -f "$D/$f.sql" 2>&1; done; } | errors)
check "indexes before the keys: no error, as the heap" test "$RES" == 0 -a "$(dbq r3 "$CHECK_FN" "SELECT differing()" | tail -n 1)" == 0

# after a restore the statements of the parent are heard: updates, key changes that move rows to another partition, deletes
# (the keys are multiples of 3, and a key that moves lands on 1 or 2 modulo 3, where no key is)
dbq r1 "UPDATE w1 SET v = v + 1 WHERE id % 7 = 3" "UPDATE w1 SET id = id + $((ROWS * 2 + 1)) WHERE id % 36 = 3 AND id < $((ROWS * 2))" "DELETE FROM w1 WHERE id % 11 = 0" >/dev/null
dbq r1 "VACUUM w1" "UPDATE w1 SET v = v + 1 WHERE id % 13 = 3" "UPDATE w1 SET id = id + $((ROWS * 2 + 2)) WHERE id % 60 = 9 AND id < $((ROWS * 2))" "DELETE FROM w1 WHERE id % 17 = 0" >/dev/null
check "after writes through the parent, the restored indexes answer as the heap and read without it" test "$(state r1)" == "$WANT"
finish
