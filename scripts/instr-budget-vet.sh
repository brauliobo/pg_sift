#!/usr/bin/env bash
# Instruction budget of the heap check of the rows of an index that no VACUUM covers (no page all-visible: a table loaded and not vacuumed, or one an old transaction
# holds back), in callgrind instructions of a single-user backend: instructions do not move with the load of the host, so the budget is a number, not a ratio. The aggregate
# below sends every row through sift_vis_filter_page; the instructions inside it (the buffer reads of the pages included) are a cost of each row. Fails over BUDGET.
#
#   scripts/instr-budget-vet.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
#
# Measured at 200,000 rows: 20,024,000 instructions with a call through a TID test for each row, 12,030,000 with the test inline; BUDGET is 16,000,000.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUDGET=${BUDGET:-16000000}
command -v valgrind >/dev/null || { echo "instr-budget-vet: valgrind is not installed, skipped"; exit 0; }
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-instr} SIFT_PORT=${SIFT_PORT:-56231}
cd "$REPO"
scripts/pgtest.sh stop >/dev/null 2>&1 || true
rm -rf "$SIFT_TMP/data"
scripts/pgtest.sh start >/dev/null
eval "$(scripts/pgtest.sh env | grep '^export')"
P="psql -X -q -At -d postgres"
$P -c "CREATE EXTENSION pg_sift" -c "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
$P >/dev/null <<'SQL'
CREATE TABLE vb (id int, grp int, amount int) WITH (autovacuum_enabled = off);
INSERT INTO vb SELECT g, g % 50, g % 1000 FROM generate_series(1, 200000) g;
CREATE INDEX vb_idx ON vb USING sift_col (id, grp, amount);
SELECT count(*) FROM vb;
SQL
scripts/pgtest.sh stop >/dev/null
Q="SELECT grp, count(*), sum(amount) FROM vb GROUP BY grp"
{ echo "SET max_parallel_workers_per_gather = 0"; echo "$Q"; } > "$SIFT_TMP/vb.sql"
ir=$(valgrind --tool=callgrind --collect-atstart=no --toggle-collect=sift_vis_filter_page --callgrind-out-file="$SIFT_TMP/vb.cg" \
	postgres --single -D "$SIFT_TMP/data" -c sift.async_maintenance=off -c jit=off -c autovacuum=off postgres < "$SIFT_TMP/vb.sql" 2>&1 >/dev/null | sed -n 's/.*Collected : *\([0-9]*\).*/\1/p' | tail -n 1)
rm -rf "$SIFT_TMP/data"
echo "instr-budget-vet: 200,000 rows checked by the heap in $ir instructions (budget $BUDGET)"
[[ -n $ir && $ir -le $BUDGET ]]
