#!/usr/bin/env bash
# Instruction budget of the statements that follow a burst of inserts into a sift_col index, in callgrind instructions of a single-user backend, relative to the same table
# once the write buffer is promoted: a scan read, sorted and decoded every record of the buffer on each statement (5,000 instructions a row: six reads after 2,000 inserts into 100,000 rows
# cost 1.71 times the promoted table's for the page of the newest rows and 1.77 for the grouped count); it now keeps the batches for the next scan, which reads the records
# appended since (1.10 and 1.19). Fails when a ratio exceeds its bound.
#
#   scripts/instr-budget-wbuf.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)

set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
PAGE_RATIO=${PAGE_RATIO:-1.4}
COUNT_RATIO=${COUNT_RATIO:-1.4}
N=${N:-100000}
BURST=${BURST:-2000}
READS=${READS:-6}
ib_begin instr-budget-wbuf 56233 <<SQL
$(for t in wb_buffered wb_promoted; do cat <<TABLE
CREATE TABLE $t (id int PRIMARY KEY, group_id int NOT NULL, kind int NOT NULL, day date NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off);
INSERT INTO $t SELECT g, 1 + g % 1500, g % 9, DATE '2026-01-01' + g % 300, g % 1000 FROM generate_series(1, $N) g;
CREATE INDEX ${t}_col ON $t USING sift_col (id, group_id, kind, day, amount) WITH (key_cut = group_id);
VACUUM (FREEZE, ANALYZE) $t;
INSERT INTO $t SELECT g, 1 + g % 1500, g % 9, DATE '2026-10-01' + g % 28, g % 1000 FROM generate_series($N + 1, $N + $BURST) g;
VACUUM $t;
TABLE
done)
SELECT sift.col_promote('wb_promoted_col');
SQL
page() { echo "SELECT id FROM $1 WHERE group_id IN (SELECT g FROM generate_series(1, 1500, 7) g) AND day >= DATE '2026-10-08' ORDER BY day DESC, id LIMIT 25"; }
count() { echo "SELECT kind, count(*), sum(amount) FROM $1 WHERE day >= DATE '2026-03-01' GROUP BY kind"; }
irs=()
for t in wb_buffered wb_promoted; do
	for q in page count; do
		f=$SIFT_TMP/ibw_${t}_$q.sql
		{ echo "SET max_parallel_workers_per_gather = 0"; for _ in $(seq "$READS"); do $q $t; done; } > "$f"
		irs+=("$(ib_ir "$f" PortalRun)")
	done
done
ib_end
read -r bp bc pp pc <<< "${irs[*]}"
echo "instr-budget-wbuf: $READS reads after $BURST inserts into $N rows: page $bp against $pp promoted, count $bc against $pc (bounds x$PAGE_RATIO, x$COUNT_RATIO)"
awk -v bp="$bp" -v pp="$pp" -v bc="$bc" -v pc="$pc" -v pr="$PAGE_RATIO" -v cr="$COUNT_RATIO" 'BEGIN { exit !(bp <= pp * pr && bc <= pc * cr) }'
